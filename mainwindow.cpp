#include "mainwindow.h"
#include "tracerowheader.h"
#include "ui_mainwindow.h"
#include "can_structs.h"
#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QtSerialPort/QSerialPortInfo>
#include "connections/canconmanager.h"
#include "connections/connectionwindow.h"
#include "helpwindow.h"
#include "utility.h"
#include "filterutility.h"
#include "enabledfeatures.h"
#include "theme.h"
#include "aboutdialog.h"
#include "tracedatadelegate.h"
#ifdef Q_OS_WASM
#include <QInputDialog>
#include "wasm/disklog/webdisklog.h"
#endif

#include <QClipboard>
#include <QActionGroup>
#include <QTimer>
#include <QProgressDialog>
#include <limits>
#include "framestore.h"
#if defined(Q_OS_WASM) && QT_CONFIG(accessibility)
#include <QAccessibleWidget>
#include <QHeaderView>
// web-savvycan: the trace (its table, headers and viewport) goes to Qt's accessibility as plain widgets, not as a table
// of cells. When a child of a table is shown (its scroll bar), Qt's browser accessibility goes over every child of the
// table, and Qt's table interface makes and keeps an interface for each cell it is asked for: a loaded log of 600 000
// frames took ~1.3 KB of memory a frame and ~35 s more that way (measured 2026-10-29, with the screen reader layer on,
// as in the browser tests). The trace is one "Frames" element for a screen reader (millions of cells weren't usable).
static QAccessibleInterface *traceAccessible(const QString &, QObject *object)
{
    QWidget *w = qobject_cast<QWidget *>(object);
    if (!w) return nullptr;
    auto isTrace = [](const QWidget *v) { return v && v->objectName() == QLatin1String("canFramesView"); };
    if (isTrace(w)) return new QAccessibleWidget(w, QAccessible::Table, QObject::tr("Frames"));
    if (isTrace(w->parentWidget())
        && (qobject_cast<QHeaderView *>(w) || w->objectName() == QLatin1String("qt_scrollarea_viewport")))
        return new QAccessibleWidget(w, QAccessible::Client);
    return nullptr;
}
#endif
/*
Some notes on things I'd like to put into the program but haven't put on github (yet)

Allow scripts to read/write signals from DBC files
allow scripts to load DBC files in support of the script - maybe the graphing system too.
*/

QString MainWindow::loadedFileName = "";
MainWindow *MainWindow::selfRef = nullptr;

MainWindow *MainWindow::getReference()
{
    return selfRef;
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
#if defined(Q_OS_WASM) && QT_CONFIG(accessibility)
    QAccessible::installFactory(traceAccessible); //before the trace exists (see traceAccessible)
#endif
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan (owner 2026-10-29): the menu bar's and the status bar's text in the middle of the bands at the
    // top and bottom of the page: no empty layout margin between them and the frame list (it made the top band
    // 36 px with the menu text at its top, the bottom one 34 px with the status text at its bottom).
    if (QLayout *central = ui->centralWidget->layout()) {
        const QMargins m = central->contentsMargins();
        central->setContentsMargins(m.left(), 0, m.right(), 0);
    }
#endif
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    qRegisterMetaTypeStreamOperators<QVector<QString>>();
    qRegisterMetaTypeStreamOperators<QVector<int>>();
#endif

    useHex = true;
    useColorsByCanId = false;
    selfRef = this;

    this->setWindowTitle("Savvy CAN V" + QString::number(VERSION) + " [Built " + QString(__DATE__) +"]");

    model = new CommFrameModel(this); // set parent to mainwindow to prevent canframemodel to change thread (might be done by setModel but just in case)

    //web-savvycan: the view shows the model directly. The QSortFilterProxyModel that sat in between
    //sorted and filtered nothing (sorting is CommFrameModel::sortByColumn, filtering its index list)
    //but kept its own row mapping, rebuilt on every insert: O(rows) per GUI tick and two ints per row.
    ui->canFramesView->setModel(model);

    settingsDialog = new MainSettingsDialog(); //instantiate the settings dialog so it can initialize settings if this is the first run or the config file was deleted.
    settingsDialog->updateSettings(); //write out all the settings. If this is the first run it'll write defaults out.

    readSettings();

    ui->canFramesView->setVerticalHeader(new TraceRowHeader(Qt::Vertical, ui->canFramesView)); //see tracerowheader.h
    QHeaderView *verticalHeader = ui->canFramesView->verticalHeader();
    verticalHeader->setSectionResizeMode(QHeaderView::Fixed);
    QSettings settings;
    int fontSize = settings.value("Main/FontSize", 9).toUInt();
    QFont sysFont;
    if(settings.value("Main/FontFixedWidth", false).toBool())
        sysFont = QFontDatabase::systemFont(QFontDatabase::FixedFont); //get default fixed width font
    else
        sysFont = QFont();  //get default font
    sysFont.setPointSize(fontSize);
    verticalHeader->setDefaultSectionSize(sysFont.pixelSize());
    verticalHeader->setFont(QFont());
    ui->canFramesView->setFont(sysFont);

    QHeaderView *HorzHdr = ui->canFramesView->horizontalHeader();
    HorzHdr->setFont(QFont());
    HorzHdr->setStretchLastSection(true); //causes the data column to automatically fill the tableview
    connect(HorzHdr, SIGNAL(sectionClicked(int)), this, SLOT(headerClicked(int)));

    lastGraphingWindow = nullptr;
    frameInfoWindow = nullptr;
    playbackWindow = nullptr;
    flowViewWindow = nullptr;
    frameSenderWindow = nullptr;
    dbcMainEditor = nullptr;
    comparatorWindow = nullptr;
    settingsDialog = nullptr;
    udsFirmwareUploaderWindow = nullptr;
    discreteStateWindow = nullptr;
    connectionWindow = nullptr;
    scriptingWindow = nullptr;
    rangeWindow = nullptr;
    dbcFileWindow = nullptr;
    fuzzingWindow = nullptr;
    udsScanWindow = nullptr;
    motorctrlConfigWindow = nullptr;
    isoWindow = nullptr;
    snifferWindow = nullptr;
    bisectWindow = nullptr;
    signalViewerWindow = nullptr;
    temporalGraphWindow = nullptr;
    dbcComparatorWindow = nullptr;
    canBridgeWindow = nullptr;
    searchWindow = nullptr;
    dbcHandler = DBCHandler::getReference();
    bDirty = false;
    inhibitFilterUpdate = false;
    rxFrames = 0;
    framesPerSec = 0;
    continuousLogging = false;
    continuousLoggingUseFilter = false;
    continuousLogFlushCounter = 0;

    //handlers for all menu entries
    connect(ui->actionSetup, SIGNAL(triggered(bool)), SLOT(showConnectionSettingsWindow()));
    connect(ui->actionOpen_Log_File, &QAction::triggered, this, &MainWindow::handleLoadFile);
    connect(ui->actionGraph_Dta, &QAction::triggered, this, &MainWindow::showGraphingWindow);
    connect(ui->actionFrame_Data_Analysis, &QAction::triggered, this, &MainWindow::showFrameDataAnalysis);
    connect(ui->actionSave_Log_File, &QAction::triggered, this, &MainWindow::handleSaveFile);
    connect(ui->actionSave_Filtered_Log_File, &QAction::triggered, this, &MainWindow::handleSaveFilteredFile);
    connect(ui->actionLoad_Filter_Definition, &QAction::triggered, this, &MainWindow::handleLoadFilters);
    connect(ui->actionSave_Filter_Definition, &QAction::triggered, this, &MainWindow::handleSaveFilters);
    connect(ui->action_Playback, &QAction::triggered, this, &MainWindow::showPlaybackWindow);
    connect(ui->actionFlow_View, &QAction::triggered, this, &MainWindow::showFlowViewWindow);
    connect(ui->action_Custom, &QAction::triggered, this, &MainWindow::showFrameSenderWindow);
    connect(ui->actionExit_Application, &QAction::triggered, this, &MainWindow::exitApp);
    connect(ui->actionFuzzy_Scope, &QAction::triggered, this, &MainWindow::showFuzzyScopeWindow);
    connect(ui->actionRange_State_2, &QAction::triggered, this, &MainWindow::showRangeWindow);
    connect(ui->actionSave_Decoded_Frames, &QAction::triggered, this, &MainWindow::handleSaveDecoded);
    connect(ui->actionSave_Decoded_Frames_CSV, &QAction::triggered, this, &MainWindow::handleSaveDecodedCsv);
    connect(ui->actionCrop_Log, &QAction::triggered, this, &MainWindow::handleCropLog);
    connect(ui->actionSingle_Multi_State_2, &QAction::triggered, this, &MainWindow::showSingleMultiWindow);
    connect(ui->actionFile_Comparison, &QAction::triggered, this, &MainWindow::showComparisonWindow);
    connect(ui->actionDBC_Comparison, &QAction::triggered, this, &MainWindow::showDBCComparisonWindow);
    connect(ui->actionScripting_INterface, &QAction::triggered, this, &MainWindow::showScriptingWindow);
    connect(ui->actionPreferences, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    connect(ui->actionUDS_Firmware_Update, &QAction::triggered, this, &MainWindow::showUDSFirmwareUploaderWindow);
    connect(ui->actionDBC_File_Manager, &QAction::triggered, this, &MainWindow::showDBCFileWindow);
    connect(ui->actionFuzzing, &QAction::triggered, this, &MainWindow::showFuzzingWindow);
    connect(ui->actionUDS_Scanner, &QAction::triggered, this, &MainWindow::showUDSScanWindow);
    connect(ui->actionISO_TP_Decoder, &QAction::triggered, this, &MainWindow::showISOInterpreterWindow);
    connect(ui->actionSniffer, &QAction::triggered, this, &MainWindow::showSnifferWindow);
    connect(ui->actionMotorControlConfig, &QAction::triggered, this, &MainWindow::showMCConfigWindow);
    connect(ui->actionCapture_Bisector, &QAction::triggered, this, &MainWindow::showBisectWindow);
    connect(ui->actionSignal_Viewer, &QAction::triggered, this, &MainWindow::showSignalViewer);
    connect(ui->actionSave_Continuous_Logfile, &QAction::triggered, this, &MainWindow::handleContinousLogging);
    connect(ui->actionSave_Continuous_Filtered_Logfile, &QAction::triggered, this, &MainWindow::handleContinousLoggingFiltered);
#ifdef Q_OS_WASM
    //browser: the download is named by the start time; this asks for a name when logging stops
    webLogAskNameAction = new QAction(tr("Choose the Log's Name When Logging Stops..."), this);
    webLogAskNameAction->setObjectName("actionWebLogAskName");
    webLogAskNameAction->setCheckable(true);
    webLogAskNameAction->setChecked(QSettings().value("WebLog/AskName", false).toBool());
    connect(webLogAskNameAction, &QAction::toggled, this, [](bool on) { QSettings().setValue("WebLog/AskName", on); });
    const QList<QAction *> fileMenu = ui->menuFile->actions(); //after Start Continuous Logging (Filtered)
    ui->menuFile->insertAction(fileMenu.value(fileMenu.indexOf(ui->actionSave_Continuous_Filtered_Logfile) + 1),
                               webLogAskNameAction);
#endif
    connect(ui->actionTemporal_Graph, &QAction::triggered, this, &MainWindow::showTemporalGraphWindow);
    connect(ui->actionCAN_Bridge, &QAction::triggered, this, &MainWindow::showCANBridgeWindow);
    connect(ui->actionFrame_Search, &QAction::triggered, this, &MainWindow::showSearchWindow);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);
    {
        // View > Light Theme / Dark Theme / Follow System Theme (one of three, as Preferences > Theme)
        QActionGroup *themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);
        themeGroup->addAction(ui->actionLight_Theme);
        themeGroup->addAction(ui->actionDark_Theme);
        themeGroup->addAction(ui->actionSystem_Theme);
        connect(ui->actionLight_Theme, &QAction::triggered, this, [] { Theme::instance()->setMode(Theme::Mode::Light); });
        connect(ui->actionDark_Theme, &QAction::triggered, this, [] { Theme::instance()->setMode(Theme::Mode::Dark); });
        connect(ui->actionSystem_Theme, &QAction::triggered, this, [] { Theme::instance()->setMode(Theme::Mode::System); });
        connect(Theme::instance(), &Theme::changed, this, &MainWindow::themeChanged);
        themeChanged(Theme::isDark());
    }

    //handlers fror interactions with the main can frame view table
    connect(ui->canFramesView, &QAbstractItemView::clicked, this, &MainWindow::gridClicked);
    connect(ui->canFramesView, &QAbstractItemView::doubleClicked, this, &MainWindow::gridDoubleClicked);
    ui->canFramesView->setContextMenuPolicy(Qt::CustomContextMenu);

    copyAct = new QAction(tr("Copy"), this);
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, &MainWindow::copyFromTable);
    ui->canFramesView->addAction(copyAct);
    connect(ui->canFramesView, &QAbstractItemView::customContextMenuRequested, this, &MainWindow::gridContextMenuRequest);

    connect(model, &CommFrameModel::updatedFiltersList, this, &MainWindow::updateFilterList);
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, model, &CommFrameModel::addFrames);
    //new implementation for continuous logging
    connect(CANConManager::getInstance(), &CANConManager::framesReceived, this, &MainWindow::logReceivedFrame);

    connect(ui->cbAutoScroll, &QAbstractButton::toggled, this, &MainWindow::interpretAutoScroll);
    connect(ui->cbInterpret, &QAbstractButton::toggled, this, &MainWindow::interpretToggled);
    connect(ui->cbOverwrite, &QAbstractButton::toggled, this, &MainWindow::overwriteToggled);
    connect(ui->cbChangingOnly, &QAbstractButton::toggled, this, &MainWindow::changingOnlyToggled);
    connect(ui->cbPersistentFilters, &QAbstractButton::toggled, this, &MainWindow::presistentFiltersToggled);
    connect(ui->listFilters, &QListWidget::itemChanged, this, &MainWindow::filterListItemChanged);
    connect(ui->listBusFilters, &QListWidget::itemChanged, this, &MainWindow::busFilterListItemChanged);
    connect(ui->listDirFilters, &QListWidget::itemChanged, this, &MainWindow::dirFilterListItemChanged);
    connect(ui->listLengthFilters, &QListWidget::itemChanged, this, &MainWindow::lengthFilterListItemChanged);
    connect(ui->listTypeFilters, &QListWidget::itemChanged, this, &MainWindow::typeFilterListItemChanged);

    connect(ui->btnCaptureToggle, &QAbstractButton::clicked, this, &MainWindow::toggleCapture);
    connect(ui->btnClearFrames, &QAbstractButton::clicked, this, &MainWindow::clearFrames);
    connect(ui->btnNormalize, &QAbstractButton::clicked, this, &MainWindow::normalizeTiming);
    connect(ui->btnFilterAll, &QAbstractButton::clicked, this, &MainWindow::filterSetAll);
    connect(ui->btnFilterNone, &QAbstractButton::clicked, this, &MainWindow::filterClearAll);
    connect(ui->btnExpandAll, &QAbstractButton::clicked, this, &MainWindow::expandAllRows);
    connect(ui->btnCollapseAll, &QAbstractButton::clicked, this, &MainWindow::collapseAllRows);

    connect(ui->tableSimpleSender, SIGNAL(cellChanged(int,int)), this, SLOT(onSenderCellChanged(int,int)));

    //Frame count column should be hidden by default. Only used in overwrite mode
    ui->canFramesView->hideColumn((int)Column::FrameCount);
    //If overwrite mode was restored from settings, show the column again
    if (ui->cbOverwrite->isChecked()) {
        ui->canFramesView->showColumn((int)Column::FrameCount);
    }

    lbStatusConnected.setText(tr("Connected to 0 buses"));
    // web-savvycan: no "Press F1 on any screen for help" (owner 2026-10-29: the help pages aren't part of this build)
    updateFileStatus();
    //lbStatusDatabase.setText(tr("No DBC database loaded"));
    ui->statusBar->insertWidget(0, &lbStatusConnected, 1);
    ui->statusBar->insertWidget(1, &lbStatusFilename, 1);
    //web-savvycan: frames lost because the GUI fell behind the bus (counted by the connections)
    lbStatusDropped.setToolTip(tr("Frames received from the device but discarded because the trace fell behind. "
                                  "Reset by Clear Frames."));
    ui->statusBar->insertWidget(2, &lbStatusDropped, 0);
    updateDroppedStatus();
    //ui->statusBar->addWidget(&lbStatusDatabase);
    ui->lblRemoteConn->setVisible(false);
    ui->lineRemoteKey->setVisible(false);

    ui->lbFPS->setText("0");
    ui->lbNumFrames->setText("0");

    // Prevent annoying accidental horizontal scrolling when filter list is populated with long interpreted message names
    ui->listFilters->horizontalScrollBar()->setEnabled(false);
    // Allow drag-select and Shift/Ctrl+click multi-selection in the ID filter list
    ui->listFilters->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->listFilters->installEventFilter(this);

    // Wrapping filter lists: items reflow on resize and height is recalculated via event filter
    ui->listBusFilters->setWrapping(true);
    ui->listBusFilters->setResizeMode(QListView::Adjust);
    ui->listDirFilters->setWrapping(true);
    ui->listDirFilters->setResizeMode(QListView::Adjust);
    ui->listLengthFilters->setWrapping(true);
    ui->listLengthFilters->setResizeMode(QListView::Adjust);
    ui->listBusFilters->installEventFilter(this);
    ui->listDirFilters->installEventFilter(this);
    ui->listTypeFilters->setWrapping(true);
    ui->listTypeFilters->setResizeMode(QListView::Adjust);
    ui->listLengthFilters->installEventFilter(this);
    ui->listTypeFilters->installEventFilter(this);

    // web-savvycan: CAN FD payloads in the Data column (a "+N B" badge for bytes out of sight)
    ui->canFramesView->setItemDelegateForColumn((int)Column::Data, new TraceDataDelegate(ui->canFramesView));

    // Restore or default splitterLeft after the window has real geometry.
    // QTimer::singleShot(0) defers to first event loop tick.
    QTimer::singleShot(0, this, [this]() {
        QSettings s;
        QByteArray state = s.value("Main/SplitterLeft").toByteArray();
        if (!state.isEmpty()) {
            ui->splitterLeft->restoreState(state);
        } else {
            int h = ui->splitterLeft->height();
            if (h > 0)
                ui->splitterLeft->setSizes({h * 3 / 4, h / 4}); // 75% frame table, 25% send panel
        }
    });

    ui->leSearchFilter->setClearButtonEnabled(true);
    connect(ui->leSearchFilter, &QLineEdit::textChanged, this, &MainWindow::onSearchFilterChanged);

    connect(&updateTimer, &QTimer::timeout, this, &MainWindow::tickGUIUpdate);
    updateTimer.setInterval(250);
    updateTimer.start();

    elapsedTime = new QElapsedTimer;
    elapsedTime->start();

    isConnected = false;
    allowCapture = true;

    //create a temporary frame to be able to capture the correct
    //default height of an item in the table. Need to do this in case
    //of scaling or font differences between different computers.
    CommFrame temp;
    temp.setBus(0);
    temp.setFrameId(0x100);
    temp.setReceived(true);
    temp.setTimeStamp(CommFrame::TimeStamp(0, 100000000));
    model->addFrame(temp, true);
    ui->canFramesView->resizeRowToContents(0);      // Resize the row to fit the contents so we get a proper height value
    qApp->processEvents();
    tickGUIUpdate(); //force a GUI refresh so that the row exists to measure
    normalRowHeight = ui->canFramesView->rowHeight(0);
    if (normalRowHeight == 0) normalRowHeight = 30; //should not be necessary but provides a sane number if something stupid happened.
    qDebug() << "normal row height = " << normalRowHeight;
    model->clearFrames();

    ui->canFramesView->verticalHeader()->setDefaultSectionSize(normalRowHeight);    // Set the default height for all rows to the height that was calculated

    //connect(CANConManager::getInstance(), CANConManager::connectionStatusUpdated, this, MainWindow::connectionStatusUpdated);
    connect(CANConManager::getInstance(), SIGNAL(connectionStatusUpdated(int)), this, SLOT(connectionStatusUpdated(int)));

    //Automatically create the connection window so it can be updated even if we never opened it.
    connectionWindow = new ConnectionWindow();
    connect(this, SIGNAL(suspendCapturing(bool)), connectionWindow, SLOT(setSuspendAll(bool)));

    //these either are unfinished/not working or are not for general use. But,they exist
    //so if you want to enable them and play with them then go for it.
    //ui->actionFirmware_Update->setVisible(false);
    ui->actionMotorControlConfig->setVisible(false);
    ui->actionSingle_Multi_State_2->setVisible(false);

    QStringList headers;
    headers << "En" << "Bus" << "ID" << "Ext" << "Rem" << "Data"
            << "Interval" << "Count";
    ui->tableSimpleSender->setColumnCount(8);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_EN, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_BUS, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_ID, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_EXT, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_REM, 70);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_DATA, 300);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_INTERVAL, 100);
    ui->tableSimpleSender->setColumnWidth(SIMP_COL::SC_COL_COUNT, 100);
    ui->tableSimpleSender->setHorizontalHeaderLabels(headers);

    createSenderRow();

    frameSender = new FrameSenderObject(model->getListReference());

    frameSender->initialize(); //creates the thread and sets things up
    frameSender->startSending(); //start the timer in the object so enabled things can send

    applyFeatures();

    installEventFilter(this);
}

// web-savvycan: hide everything whose feature is off in enabledfeatures.h (menus, buttons, panels), on the
// desktop and in the browser. Hidden actions are also disabled, so their shortcuts do nothing.
void MainWindow::applyFeatures()
{
    using Features::Feature;
    const QList<QPair<QAction *, Feature>> actions = {
        // File
        {ui->actionOpen_Log_File, Feature::LoadLog},
        {ui->actionSave_Log_File, Feature::Logging},
        {ui->actionSave_Filtered_Log_File, Feature::Logging},
        {ui->actionSave_Continuous_Logfile, Feature::Logging},
        {ui->actionSave_Continuous_Filtered_Logfile, Feature::Logging},
        {ui->actionLoad_Filter_Definition, Feature::FilterFiles},
        {ui->actionSave_Filter_Definition, Feature::FilterFiles},
        {ui->actionDBC_File_Manager, Feature::DBC},
        {ui->actionSave_Decoded_Frames, Feature::DBC},
        {ui->actionSave_Decoded_Frames_CSV, Feature::DBC},
        {ui->actionCrop_Log, Feature::CropLog},
        {ui->actionPreferences, Feature::Preferences},
        // View, Help
        {ui->actionLight_Theme, Feature::ThemeSwitch},
        {ui->actionDark_Theme, Feature::ThemeSwitch},
        {ui->actionSystem_Theme, Feature::ThemeSwitch},
        {ui->actionAbout, Feature::About},
        // RE Tools
        {ui->actionFlow_View, Feature::FlowView},
        {ui->actionGraph_Dta, Feature::Graphs},
        {ui->actionFrame_Data_Analysis, Feature::FrameInfo},
        {ui->actionFile_Comparison, Feature::FileComparator},
        {ui->actionDBC_Comparison, Feature::DBCComparator},
        {ui->actionRange_State_2, Feature::RangeState},
        {ui->actionSingle_Multi_State_2, Feature::DiscreteState},
        {ui->actionISO_TP_Decoder, Feature::ISOTP},
        {ui->actionSniffer, Feature::Sniffer},
        {ui->actionCapture_Bisector, Feature::Bisector},
        {ui->actionSignal_Viewer, Feature::SignalViewer},
        {ui->actionTemporal_Graph, Feature::TemporalGraph},
        {ui->actionFrame_Search, Feature::FrameSearch},
        // Send Frames
        {ui->action_Playback, Feature::Playback},
        {ui->action_Custom, Feature::FrameSender},
        {ui->actionScripting_INterface, Feature::Scripting},
        {ui->actionFuzzing, Feature::Fuzzing},
        {ui->actionUDS_Scanner, Feature::UDSScanner},
        {ui->actionUDS_Firmware_Update, Feature::UDSFirmwareUpdate},
        {ui->actionMotorControlConfig, Feature::MotorControlConfig},
        {ui->actionCAN_Bridge, Feature::CANBridge},
        // Connection
        {ui->actionSetup, Feature::Connection},
    };
    for (const auto &entry : actions) {
        const bool enabled = Features::on(entry.second);
        entry.first->setVisible(enabled);
        entry.first->setEnabled(enabled);
    }
    // Actions in the .ui that no menu shows (no handler or unfinished upstream): never reachable.
    for (QAction *a : {ui->actionFuzzy_Scope, ui->actionLoad_DBC_File, ui->actionEdit_Messages_Signals,
                       ui->actionSave_DBC_File, ui->actionFirmware_Uploader, ui->actionConnect,
                       ui->actionFirmware_Update}) {
        a->setVisible(false);
        a->setEnabled(false);
    }
#ifdef Q_OS_WASM
    // A web page can't quit; closing the tab is the way out.
    ui->actionExit_Application->setVisible(false);
    ui->actionExit_Application->setEnabled(false);
#endif

    // A menu with nothing left in it is hidden as a whole.
    for (QMenu *menu : ui->menuBar->findChildren<QMenu *>(Qt::FindDirectChildrenOnly)) {
        bool any = false;
        for (QAction *a : menu->actions())
            if (!a->isSeparator() && a->isVisible()) any = true;
        menu->menuAction()->setVisible(any);
    }
    // No View menu (owner 2026-10-29): the theme is chosen in File > Preferences > Theme.
    ui->menuView->menuAction()->setVisible(false);

    // The main window's own controls.
    ui->cbInterpret->setVisible(Features::on(Feature::DBC));
    if (!Features::on(Feature::FrameSender)) ui->tableSimpleSender->setVisible(false);
}

void MainWindow::showAboutDialog()
{
    if (!Features::on(Features::Feature::About)) return;
    // Modal (exec()): tests/web's smoke spec checks that a modal dialog works in the browser (Asyncify).
    AboutDialog about(this);
    about.exec();
}

void MainWindow::themeChanged(bool dark)
{
    Q_UNUSED(dark);
    switch (Theme::instance()->mode()) {
    case Theme::Mode::Light:  ui->actionLight_Theme->setChecked(true); break;
    case Theme::Mode::Dark:   ui->actionDark_Theme->setChecked(true); break;
    case Theme::Mode::System: ui->actionSystem_Theme->setChecked(true); break;
    }
    // Row colours come from the model (palette, per-ID colours): repaint with the new set.
    ui->canFramesView->viewport()->update();
}

MainWindow::~MainWindow()
{
    updateTimer.stop();
    frameSender->stopSending();
    killEmAll(); //Ride the lightning
    delete ui;
    delete model;
    delete elapsedTime;
    delete dbcHandler;
}

//kill every sub window that could be open. At the moment a hard coded list
//but eventually each window should be registered and be able to be iterated.
void MainWindow::killEmAll()
{
    foreach (GraphingWindow *win, graphWindows)
    {
        killWindow(win);
    }
    killWindow(frameInfoWindow);
    killWindow(playbackWindow);
    killWindow(flowViewWindow);
    killWindow(frameSenderWindow);
    killWindow(comparatorWindow);
    killWindow(dbcMainEditor);
    killWindow(settingsDialog);
    killWindow(discreteStateWindow);
#ifndef Q_OS_WASM //no scripting (Qt Qml) in the browser build
    killWindow(scriptingWindow);
#endif
    killWindow(rangeWindow);
    killWindow(dbcFileWindow);
    killWindow(fuzzingWindow);
    killWindow(udsScanWindow);
    killWindow(isoWindow);
    killWindow(snifferWindow);
    killWindow(bisectWindow);
    killWindow(udsFirmwareUploaderWindow);
    killWindow(motorctrlConfigWindow);
    killWindow(signalViewerWindow);
    killWindow(temporalGraphWindow);
    killWindow(canBridgeWindow);

    //trying to kill this window can cause a fault to happen. It's closed last just in case.
    killWindow(connectionWindow);
}

//forcefully close the window, kill it, and salt the earth
//note, for some stupid reason this function causes a seg fault
//it seems that when it runs just before the program closes it'll
//fault out when trying to close the connection window. I assume
//this could be because that window has long running threads open and doesn't
//close quickly or maybe cleanly. Investigate.
void MainWindow::killWindow(QDialog *win)
{
    if (win)
    {
        win->close();
        delete win;
        win = nullptr;
    }
}

void MainWindow::exitApp()
{
    this->close();
    QApplication::quit(); //forces the whole application to terminate when the main window is closed
}


//the close event can be trapped and ignored so put unsaved warnings in here so the user can abort the program closing if they forgot to save things.
void MainWindow::closeEvent(QCloseEvent *event)
{
    QMessageBox::StandardButton confirmDialog;

    for (int i = 0; i < dbcHandler->getFileCount(); i++)
    {
        DBCFile *file = dbcHandler->getFileByIdx(i);
        if (file->getDirtyFlag())
        {
            confirmDialog = QMessageBox::question(this, "Unsaved DBC", "DBC File:\n" + file->getFilename() + "\nAppears to have unsaved changes\nReally close without saving?", QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog != QMessageBox::Yes)
            {
                event->ignore();
                return;
            }
        }
    }

    removeEventFilter(this);
    writeSettings();
    exitApp();
    event->accept();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    // Space key on the ID filter list: toggle checkbox state for all selected items
    if (obj == ui->listFilters && event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Space) {
            QList<QListWidgetItem*> selected = ui->listFilters->selectedItems();
            if (!selected.isEmpty()) {
                Qt::CheckState newState = (selected.first()->checkState() == Qt::Checked)
                                          ? Qt::Unchecked : Qt::Checked;
                inhibitFilterUpdate = true;
                for (QListWidgetItem *item : selected)
                    item->setCheckState(newState);
                inhibitFilterUpdate = false;
                QList<QPair<int,bool>> updates;
                updates.reserve(selected.size());
                for (QListWidgetItem *item : selected)
                    updates.append({FilterUtility::getIdAsInt(item), newState == Qt::Checked});
                model->setFilterStatesBatch(updates);
                manageRowExpansion();
                return true;
            }
        }
    }
    // Recalculate height of wrapping filter lists when they are resized (panel drag etc.)
    if ((obj == ui->listBusFilters || obj == ui->listDirFilters || obj == ui->listLengthFilters || obj == ui->listTypeFilters)
        && event->type() == QEvent::Resize) {
        QListWidget *list = qobject_cast<QListWidget*>(obj);
        QTimer::singleShot(0, this, [this, list]() {
            if (list->count() > 0) {
                QRect r = list->visualItemRect(list->item(list->count() - 1));
                int h = r.bottom() + 1 + 2 * list->frameWidth();
                if (h > 0 && h != list->height()) list->setFixedHeight(h);
            }
        });
        return false;
    }
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("mainscreen.md");
            return true;
            break;
        }
        return false;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}


void MainWindow::updateSettings()
{
    readUpdateableSettings();
    emit settingsUpdated();
}

void MainWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("Main/WindowSize", QSize(800, 750)).toSize());
        move(Utility::constrainedWindowPos(settings.value("Main/WindowPos", QPoint(100, 100)).toPoint()));

        ui->canFramesView->setColumnWidth(0, settings.value("Main/TimeColumn", 150).toUInt()); //time stamp
        ui->canFramesView->setColumnWidth(1, settings.value("Main/IDColumn", 70).toUInt()); //frame ID
        ui->canFramesView->setColumnWidth(2, settings.value("Main/ExtColumn", 40).toUInt()); //extended
        ui->canFramesView->setColumnWidth(3, settings.value("Main/RemColumn", 40).toUInt()); //remote
        ui->canFramesView->setColumnWidth(4, settings.value("Main/DirColumn", 40).toUInt()); //direction
        ui->canFramesView->setColumnWidth(5, settings.value("Main/BusColumn", 40).toUInt()); //bus
        ui->canFramesView->setColumnWidth(6, settings.value("Main/LengthColumn", 40).toUInt()); //length
        ui->canFramesView->setColumnWidth(7, settings.value("Main/AsciiColumn", 50).toUInt()); //ascii
        //readUpdateableSettings() runs after this, so read the flag straight from the settings here
        if (settings.value("Main/UseHexAndDec", false).toBool())
            ui->canFramesView->setColumnWidth(8, settings.value("Main/DataColumn", 300).toUInt()); //data

        QByteArray splitterMainState = settings.value("Main/SplitterMain").toByteArray();
        if (!splitterMainState.isEmpty())
            ui->splitterMain->restoreState(splitterMainState);
        else
            ui->splitterMain->setSizes({750, 250});

        // splitterLeft is restored/defaulted via deferred QTimer in constructor
    }
    else
    {
        ui->splitterMain->setSizes({750, 250});
        // splitterLeft is defaulted via deferred QTimer in constructor
    }

    if (settings.value("Main/AutoScroll", false).toBool())
    {
        ui->cbAutoScroll->setChecked(Qt::Checked);
    }

    if (settings.value("Main/Overwrite", false).toBool())
    {
        ui->cbOverwrite->setChecked(Qt::Checked);
        model->setOverwriteMode(true);
    }
    else
    {
        model->setOverwriteMode(false);
        rowExpansionActive = false;
    }

    if (settings.value("Main/PersistentFilters", false).toBool())
    {
        ui->cbPersistentFilters->setChecked(Qt::Checked);
        model->setClearMode(true);
    }

    if (settings.value("Main/Overwrite", false).toBool())
    {
        ui->cbOverwrite->setChecked(Qt::Checked);
        model->setOverwriteMode(true);
    }
    else
    {
        model->setOverwriteMode(false);
        rowExpansionActive = false;
    }

    if (settings.value("Main/ChangingOnly", false).toBool())
    {
        ui->cbChangingOnly->setChecked(Qt::Checked);
        model->setChangingOnly(true);
    }
    else
        model->setChangingOnly(false);

    if (settings.value("Main/PersistentFilters", false).toBool())
    {
        ui->cbPersistentFilters->setChecked(Qt::Checked);
        model->setClearMode(true);
    }
    else
        model->setClearMode(false);

    int fontSize = settings.value("Main/FontSize", 9).toUInt();
    QFont newFont = QFont(ui->canFramesView->font());
    newFont.setPointSize(fontSize);
    ui->canFramesView->setFont(newFont);

    readUpdateableSettings();
}


/*
 * TODO: The way the frame timing mode is specified is DEAD STUPID. There shouldn't be three boolean values
 * for this. Instead switch it all to an ENUM or something sane.
*/
void MainWindow::readUpdateableSettings()
{
    QSettings settings;
    useHex = settings.value("Main/UseHex", true).toBool();
    model->setHexMode(useHex);
    Utility::decimalMode = !useHex;

    useColorsByCanId = settings.value("Main/ColorsByCanId", false).toBool();
    model->setUseColorsByCanId(useColorsByCanId);
    useHexAndDec = settings.value("Main/UseHexAndDec", false).toBool();
    model->setHexAndDecMode(useHexAndDec);

    bool tempBool;
    TimeStyle ts = TS_MICROS;
    tempBool = settings.value("Main/TimeSeconds", false).toBool();
    if (tempBool) ts = TS_SECONDS;
    tempBool = settings.value("Main/TimeClock", false).toBool();
    if (tempBool) ts = TS_CLOCK;
    tempBool = settings.value("Main/TimeMillis", false).toBool();
    if (tempBool) ts = TS_MILLIS;
    model->setTimeStyle(ts);

    useFiltered = settings.value("Main/UseFiltered", false).toBool();
    model->setTimeFormat(settings.value("Main/TimeFormat", "MMM-dd HH:mm:ss.zzz").toString());
    ignoreDBCColors = settings.value("Main/IgnoreDBCColors", false).toBool();
    model->setIgnoreDBCColors(ignoreDBCColors);
    int bpl = settings.value("Main/BytesPerLine", 16).toInt();
    model->setBytesPerLine(bpl);

    CSVAbsTime = settings.value("Main/CSVAbsTime", false).toBool();

    // web-savvycan (owner 2026-10-29): the ID filter list fills the side panel's width, as the filter box and
    // buttons above and below it do. Upstream capped it at 175 px (250 with filter labels), so its background
    // ended partway across the panel.

    bool showSendPanel = settings.value("Main/ShowSendPanel", true).toBool()
                         && Features::on(Features::Feature::FrameSender);
    ui->tableSimpleSender->setVisible(showSendPanel);

    updateFilterList();    
}    


void MainWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("Main/WindowSize", size());
        settings.setValue("Main/WindowPos", pos());
        settings.setValue("Main/TimeColumn", ui->canFramesView->columnWidth(0));
        settings.setValue("Main/IDColumn", ui->canFramesView->columnWidth(1));
        settings.setValue("Main/ExtColumn", ui->canFramesView->columnWidth(2));
        settings.setValue("Main/RemColumn", ui->canFramesView->columnWidth(3));
        settings.setValue("Main/DirColumn", ui->canFramesView->columnWidth(4));
        settings.setValue("Main/BusColumn", ui->canFramesView->columnWidth(5));
        settings.setValue("Main/LengthColumn", ui->canFramesView->columnWidth(6));
        settings.setValue("Main/AsciiColumn", ui->canFramesView->columnWidth(7));
        if (useHexAndDec)
            settings.setValue("Main/DataColumn", ui->canFramesView->columnWidth(8));
        settings.setValue("Main/SplitterMain", ui->splitterMain->saveState());
    }
    // Always save splitterLeft so send-panel height is restored on next launch
    settings.setValue("Main/SplitterLeft", ui->splitterLeft->saveState());
}

void MainWindow::onSenderCellChanged(int row, int col)
{
    if (inhibitSenderChanged) return;
    qDebug() << "onCellChanged";
    if (row == ui->tableSimpleSender->rowCount() - 1)
    {
        createSenderRow();
    }

    processSenderCellChange(row, col);
}

void MainWindow::onSearchFilterChanged(const QString &text)
{
    // Apply search text to the model (filters main table rows)
    model->setSearchFilter(text);

    // Show/hide items in the filter list to match the search text
    QString stripped = text;
    if (stripped.startsWith("0x", Qt::CaseInsensitive))
        stripped = stripped.mid(2);

    for (int i = 0; i < ui->listFilters->count(); ++i) {
        QListWidgetItem *item = ui->listFilters->item(i);
        if (stripped.isEmpty())
            item->setHidden(false);
        else {
            QString idStr = FilterUtility::getId(item);
            // strip leading "0x" from the displayed id string too
            if (idStr.startsWith("0x", Qt::CaseInsensitive))
                idStr = idStr.mid(2);
            item->setHidden(!idStr.contains(stripped, Qt::CaseInsensitive));
        }
    }
}

void MainWindow::processSenderCellChange(int line, int col)
{
    qDebug() << "processSenderCellChange";
    FrameSendData *tempData;
    QStringList tokens;
    int tempVal;

    int numBuses = CANConManager::getInstance()->getNumBuses();
    QByteArray arr;

    tempData = frameSender->getSendRecordRef(line);

    if (!tempData)
    {
        qDebug() << "Need to set up a new entry in senders";
        FrameSendData dat;
        dat.enabled = false;
        dat.count = 0;
        dat.setFrameCount(0);
        dat.setBus(0);
        frameSender->addSendRecord(dat);
        tempData = frameSender->getSendRecordRef(line);
    }

    if (!tempData)
    {
        qDebug() << "No data to modify in processSenderCellChange. This is a bug!";
        return;
    }

    switch (col)
    {
    case SIMP_COL::SC_COL_EN: //Enable check box
        if (ui->tableSimpleSender->item(line, 0)->checkState() == Qt::Checked)
        {
            tempData->enabled = true;
        }
        else tempData->enabled = false;
        qDebug() << "Setting enabled to " << tempData->enabled;
        break;
    case SIMP_COL::SC_COL_BUS: //Bus designation
        tempVal = Utility::ParseStringToNum(ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_BUS)->text());
        if (tempVal < -1) tempVal = -1;
        if (tempVal >= numBuses) tempVal = numBuses - 1;
        tempData->setBus(tempVal);
        qDebug() << "Setting bus to " << tempVal;
        break;
    case SIMP_COL::SC_COL_ID: //ID field
        tempVal = Utility::ParseStringToNum(ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_ID)->text());
        if (tempVal < 0) tempVal = 0;
        if (tempVal > 0x7FFFFFFF) tempVal = 0x7FFFFFFF;
        tempData->setFrameId(tempVal);
        if (tempData->frameId() > 0x7FF) {
            tempData->setExtendedFrameFormat(true);
            ui->tableSimpleSender->blockSignals(true);
            ui->tableSimpleSender->item(line, ST_COLS::SENDTAB_COL_EXT)->setCheckState(Qt::Checked);
            ui->tableSimpleSender->blockSignals(false);
        }
        qDebug() << "setting ID to " << tempVal;
        break;
    case SIMP_COL::SC_COL_EXT:
        if (ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_EXT)->checkState() == Qt::Checked) {
            tempData->setExtendedFrameFormat(true);
        } else {
            tempData->setExtendedFrameFormat(false);
        }
        break;
    case SIMP_COL::SC_COL_REM:
        if (ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_REM)->checkState() == Qt::Checked) {
            tempData->setFrameType(CommFrame::RemoteRequestFrame);
        } else {
            tempData->setFrameType(CommFrame::CANDataFrame);
        }
        break;
    case SIMP_COL::SC_COL_DATA: //Data bytes
        //for (int i = 0; i < 8; i++) tempData->payload().data()[i] = 0;

#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
        tokens = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_DATA)->text().split(" ", Qt::SkipEmptyParts);
#else
        tokens = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_DATA)->text().split(" ", QString::SkipEmptyParts);
#endif
        arr.clear();
        arr.reserve(tokens.count());
        for (int j = 0; j < tokens.count(); j++)
        {
            arr.append((uint8_t)Utility::ParseStringToNum(tokens[j]));
        }
        tempData->setPayload(arr);
        break;
    case SIMP_COL::SC_COL_INTERVAL: //interval in ms

        QString trigger = ui->tableSimpleSender->item(line, SIMP_COL::SC_COL_INTERVAL)->text().toUpper();

        Trigger thisTrigger;
        thisTrigger.bus = -1; //-1 means we don't care which
        thisTrigger.ID = -1; //the rest of these being -1 means nothing has changed it
        thisTrigger.maxCount = -1;
        thisTrigger.milliseconds = -1;
        thisTrigger.currCount = 0;
        thisTrigger.msCounter = 0;
        thisTrigger.triggerMask = 0;
        thisTrigger.readyCount = true;

        tempData->triggers.clear();
        tempData->triggers.reserve(1);

        if (trigger != "")
        {
            thisTrigger.milliseconds = Utility::ParseStringToNum(trigger);
            thisTrigger.triggerMask |= TriggerMask::TRG_MS;
        }

        if (thisTrigger.milliseconds < 1) thisTrigger.milliseconds = 1;

        tempData->triggers.append(thisTrigger);

        break;
    }
}

void MainWindow::createSenderRow()
{
    int row = ui->tableSimpleSender->rowCount();
    ui->tableSimpleSender->insertRow(row);

    QTableWidgetItem *item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    inhibitSenderChanged = true;
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_EN, item);

    item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_EXT, item);

    item = new QTableWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    ui->tableSimpleSender->setItem(row, SIMP_COL::SC_COL_REM, item);

    for (int i = 1; i <= SIMP_COL::SC_COL_COUNT; i++)
    {
        if (i != SIMP_COL::SC_COL_EXT && i != SIMP_COL::SC_COL_REM) {
            item = new QTableWidgetItem("");
            ui->tableSimpleSender->setItem(row, i, item);
        }
    }

    inhibitSenderChanged = false;
}

void MainWindow::updateConnectionSettings(QString connectionType, QString port, int speed0, int speed1)
{
    Q_UNUSED(connectionType);
    Q_UNUSED(port);
    Q_UNUSED(speed0);
    Q_UNUSED(speed1);
    //connType = connectionType;
    //portName = port;

    //canSpeed0 = speed0;
    //canSpeed1 = speed1;
    if (isConnected)
    {
        //emit updateBaudRates(speed0, speed1);
    }
}

void MainWindow::headerClicked(int logicalIndex)
{
    //ui->canFramesView->sortByColumn(logicalIndex);
    model->sortByColumn(logicalIndex);

    manageRowExpansion();
}

void MainWindow::expandAllRows()
{
    bool goAhead = false;
    int numRows = ui->canFramesView->model()->rowCount();

    if (numRows > 20000)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Really?", "It's not recommended to use this\non more than 20000 frames.\nIt can take a long time.\n\nYou have been warned!\nStill do it?",
                                  QMessageBox::Yes|QMessageBox::No);

        if (confirmDialog == QMessageBox::Yes) goAhead = true;
    }
    else goAhead = true;

    if (goAhead)
    {
        ui->canFramesView->resizeRowsToContents();

        rowExpansionActive = true;
    }
}

void MainWindow::manageRowExpansion()
{
    int numRows = ui->canFramesView->model()->rowCount();
    if(numRows < 20000)
    {
        if(rowExpansionActive && model->getInterpretMode())
            ui->canFramesView->resizeRowsToContents();
    }
    else
    {
        disableAutoRowExpansion();
    }
}

void MainWindow::disableAutoRowExpansion()
{
    rowExpansionActive = false;
}

void MainWindow::collapseAllRows()
{
    bool goAhead = false;
    int numRows = ui->canFramesView->model()->rowCount();

    if (numRows > 50000)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Really?", "It's not recommended to use this\non more than 50000 frames.\nIt can take a long time.\n\nYou have been warned!\nStill do it?",
                                  QMessageBox::Yes|QMessageBox::No);

        if (confirmDialog == QMessageBox::Yes) goAhead = true;
    }
    else goAhead = true;

    if (goAhead)
    {
        for (int i = 0; i < numRows; i++) ui->canFramesView->setRowHeight(i, normalRowHeight);

        rowExpansionActive = false;
    }
}

void MainWindow::gridClicked(QModelIndex idx)
{
    //qDebug() << "Grid Clicked";
    if (ui->canFramesView->rowHeight(idx.row()) > normalRowHeight)
    {
        ui->canFramesView->setRowHeight(idx.row(), normalRowHeight);
    }
    else {
        ui->canFramesView->resizeRowToContents(idx.row());
    }
}

void MainWindow::gridDoubleClicked(QModelIndex idx)
{
    qDebug() << "Grid double clicked";
    //grab ID and timestamp and send them away
    CommFrame frame = model->getListReference()->at(idx.row());
    emit sendCenterTimeID(frame.frameId(), frame.timeStamp().microSeconds() / 1000000.0);
}

void MainWindow::gridContextMenuRequest(QPoint pos)
{
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    qDebug() << "Pos: " << pos << " Row :" << idx.row() << " Col: " << idx.column();

    if (!idx.isValid()) return;

    QMenu *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    menu->addAction(copyAct);

    if (idx.column() == 8 && Features::on(Features::Feature::Graphs)) //we're over the DATA column
    {
        contextMenuPosition = pos;
        menu->addSeparator();
        menu->addAction(tr("Add to a new graphing window"), this, SLOT(setupAddToNewGraph()));
        menu->addAction(tr("Add to latest graphing window"), this, SLOT(setupSendToLatestGraphWindow()));
    }

    menu->popup(ui->canFramesView->viewport()->mapToGlobal(pos));
}

void MainWindow::copyFromTable()
{
    copySelection();
}

void MainWindow::copySelection()
{
    QItemSelectionModel *selectionModel = ui->canFramesView->selectionModel();
    QModelIndexList selectedIndexes = selectionModel->selectedIndexes();

    if(selectedIndexes.isEmpty())
        return;

    // QModelIndex::operator< sorts by row and then by column.
    std::sort(selectedIndexes.begin(), selectedIndexes.end());

    QString selectedText;
    int currentRow = -1;
    int lastRow = selectedIndexes.last().row();

    for(const QModelIndex &current : selectedIndexes)
    {
        if (currentRow != -1 && current.row() != currentRow)
        {
            // remove last tab
            if (selectedText.endsWith(QLatin1Char('\t')))
                selectedText.chop(1);
            selectedText.append(QLatin1Char('\n'));
        }
        currentRow = current.row();

        QString cellText = current.data(Qt::DisplayRole).toString();

        // Replace newlines within a cell to avoid breaking the table structure in Excel
        cellText.replace(QLatin1Char('\n'), QLatin1String("  "));

        selectedText.append(cellText);

        if (current.row() != lastRow || current != selectedIndexes.last())
        {
            selectedText.append(QLatin1Char('\t'));
        }
    }

    // remove last tab if it exists
    if (selectedText.endsWith(QLatin1Char('\t')))
        selectedText.chop(1);

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(selectedText);
}

QString MainWindow::getSignalNameFromPosition(QPoint pos)
{
    //there's a bit of an issue to solve. The data column is one big string even if there are a number
    //of signals in there. So, the basic idea is to find out how tall the font is and where the user
    //clicked within the cell. Then find out which line that puts us over.
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    int fontHeight = ui->canFramesView->fontMetrics().height();
    int cellBaseY = ui->canFramesView->rowViewportPosition(idx.row());
    int lineOffset = (pos.y() - cellBaseY) / fontHeight;
    qDebug() << "Offset: " << lineOffset;
    QString lineText = idx.data().toString().split("\n")[lineOffset];
    qDebug() << "Line Text: " << lineText;
    return lineText.split(":")[0];
}

uint32_t MainWindow::getMessageIDFromPosition(QPoint pos)
{
    QModelIndex idx = ui->canFramesView->indexAt(pos); //figure out where in the view we clicked (row, column)
    QString idText = ui->canFramesView->model()->index(idx.row(), 1).data().toString();
    return Utility::ParseStringToNum(idText);
}

void MainWindow::setupAddToNewGraph()
{
    if (!Features::on(Features::Feature::Graphs)) return; // web-savvycan: enabledfeatures.h
    showGraphingWindow(); //creates a new window and sets it as latest
    setupSendToLatestGraphWindow(); //then call the other function to finish
}


void MainWindow::setupSendToLatestGraphWindow()
{
    if (!Features::on(Features::Feature::Graphs)) return; // web-savvycan: enabledfeatures.h
    if (!lastGraphingWindow) showGraphingWindow();
    GraphParams param;
    QString signalName = getSignalNameFromPosition(contextMenuPosition);
    param.ID = getMessageIDFromPosition(contextMenuPosition);
    DBC_MESSAGE *msg = dbcHandler->findMessage(param.ID);
    if(msg)
    {
        DBC_SIGNAL *sig = msg->sigHandler->findSignalByName(signalName);
        if(sig)
        {
            param.associatedSignal = sig;
            param.bias = sig->bias;
            param.intelFormat = sig->intelByteOrder;
            param.isSigned = sig->valType == SIGNED_INT ? true : false;
            param.numBits = sig->signalSize;
            param.scale = sig->factor;
            param.startBit = sig->startBit;
            param.stride = 1;
            param.graphName = sig->name;
            param.lineColor = GraphParams::randomLineColor(); // web-savvycan: shows in light and dark
            param.lineWidth = 1;
            param.fillColor = QColor(128, 128, 128, 0);
            param.mask = 0xFFFFFFFFFFFFFFFFull;
            param.drawOnlyPoints = false;
            param.pointType = 0;

            lastGraphingWindow->createGraph(param); //add the new graph to the window
        }
        else
        {
            QMessageBox msgbox;
            QString boxmsg = "Cannot find ID 0x" + QStringLiteral("%1").arg(param.ID, 3, 16, QLatin1Char('0')) + " in DBC message " + msg->name + ". Not adding graph.";
            msgbox.setText(boxmsg);
            msgbox.exec();
        }
    }
    else
    {
        QMessageBox msgbox;
        QString boxmsg = "Cannot find ID 0x" + QStringLiteral("%1").arg(param.ID, 3, 16, QLatin1Char('0')) + " in DBC file(s). Not adding graph.";
        msgbox.setText(boxmsg);
        msgbox.exec();
    }
}

void MainWindow::interpretAutoScroll(bool state)
{
    QSettings settings;
    settings.setValue("Main/AutoScroll", state);
}

void MainWindow::interpretToggled(bool state)
{
    QSettings settings;
    settings.setValue("Main/Interpret", state);

    model->setInterpretMode(state);
    //ui->canFramesView->resizeRowsToContents();   //a VERY costly operation!
}

void MainWindow::overwriteToggled(bool state)
{
    QSettings settings;
    settings.setValue("Main/Overwrite", state);

    if (state)
    {
        QMessageBox::StandardButton confirmDialog;
        confirmDialog = QMessageBox::question(this, "Danger Will Robinson", "Enabling Overwrite mode will\ndelete your captured frames\nand replace them with one\nframe per ID.\n\nAre you ready to do that?",
                                      QMessageBox::Yes|QMessageBox::No);
        if (confirmDialog == QMessageBox::Yes)
        {
            model->setOverwriteMode(true);
            ui->canFramesView->showColumn((int)Column::FrameCount);
        }
        else ui->cbOverwrite->setCheckState(Qt::Unchecked);

    }
    else
    {
        rowExpansionActive = false;
        model->setOverwriteMode(false);
        ui->canFramesView->hideColumn((int)Column::FrameCount);
    }
}

void MainWindow::changingOnlyToggled(bool state)
{
    QSettings settings;
    settings.setValue("Main/ChangingOnly", state);
    model->setChangingOnly(state);
}

void MainWindow::presistentFiltersToggled(bool state)
{    
    QSettings settings;
    settings.setValue("Main/PersistentFilters", state);

    if (state)
    {
        model->setClearMode(true);
    }
    else
    {
        model->setClearMode(false);
    }
}

void MainWindow::updateFilterList()
{
    if (model == nullptr) return;
    const QMap<int, bool> *filters = model->getFiltersReference();
    const QMap<int, bool> *busFilters = model->getBusFiltersReference();
    if (filters == nullptr || busFilters == nullptr) return;

    qDebug() << "updateFilterList called on MainWindow";

    inhibitFilterUpdate = true;

    ui->listFilters->clear();
    ui->listBusFilters->clear();
    ui->listDirFilters->clear();
    ui->listLengthFilters->clear();
    ui->listTypeFilters->clear();

    if (filters->isEmpty()) return;

    QMap<int, bool>::const_iterator filterIter;
    for (filterIter = filters->begin(); filterIter != filters->end(); ++filterIter)
    {
        /*QListWidgetItem *thisItem = */FilterUtility::createCheckableFilterItem(filterIter.key(), filterIter.value(), ui->listFilters);
    }

    if (!busFilters->isEmpty())
    {
        for (filterIter = busFilters->begin(); filterIter != busFilters->end(); ++filterIter)
        {
            FilterUtility::createCheckableBusFilterItem(filterIter.key(), filterIter.value(), ui->listBusFilters);
        }
    }

    // Direction filters
    const QMap<int, bool> *dirFilters = model->getDirFiltersReference();
    if (dirFilters && !dirFilters->isEmpty())
    {
        for (filterIter = dirFilters->begin(); filterIter != dirFilters->end(); ++filterIter)
        {
            QString label = (filterIter.key() == 1) ? tr("Rx") : tr("Tx");
            QListWidgetItem *item = new QListWidgetItem(label, ui->listDirFilters);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(filterIter.value() ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, filterIter.key());
        }
    }

    // Length filters
    const QMap<int, bool> *lengthFilters = model->getLengthFiltersReference();
    if (lengthFilters && !lengthFilters->isEmpty())
    {
        for (filterIter = lengthFilters->begin(); filterIter != lengthFilters->end(); ++filterIter)
        {
            FilterUtility::createCheckableBusFilterItem(filterIter.key(), filterIter.value(), ui->listLengthFilters);
        }
    }

    // Frame type filters (classic CAN / CAN FD / FD with BRS ...)
    const QMap<int, bool> *typeFilters = model->getTypeFiltersReference();
    if (typeFilters && !typeFilters->isEmpty())
    {
        for (filterIter = typeFilters->begin(); filterIter != typeFilters->end(); ++filterIter)
        {
            QListWidgetItem *item = new QListWidgetItem(CommFrameModel::typeFilterLabel(filterIter.key()), ui->listTypeFilters);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(filterIter.value() ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, filterIter.key());
            item->setToolTip(CommFrameModel::typeFilterToolTip(filterIter.key()));
        }
    }

    // Adjust height of the wrapping filter lists to fit their items (deferred so layout is done first)
    QTimer::singleShot(0, this, [this]() {
        auto fitH = [](QListWidget *list) {
            if (list->count() == 0) { list->setFixedHeight(0); return; }
            QRect r = list->visualItemRect(list->item(list->count() - 1));
            int h = r.bottom() + 1 + 2 * list->frameWidth();
            if (h > 0) list->setFixedHeight(h);
        };
        fitH(ui->listBusFilters);
        fitH(ui->listDirFilters);
        fitH(ui->listLengthFilters);
        fitH(ui->listTypeFilters);
    });

    inhibitFilterUpdate = false;

    // Re-apply any active search filter text so items stay hidden/shown after the list is repopulated
    onSearchFilterChanged(ui->leSearchFilter->text());
}

void MainWindow::filterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;
    //qDebug() << item->text();

    // strip away possible filter label
    int ID = FilterUtility::getIdAsInt(item);
    bool isSet = false;
    if (item->checkState() == Qt::Checked) isSet = true;

    model->setFilterState(ID, isSet);

    manageRowExpansion();
}

void MainWindow::busFilterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;

    int ID = FilterUtility::getIdAsInt(item);
    bool isSet = (item->checkState() == Qt::Checked);

    model->setBusFilterState(ID, isSet);

    manageRowExpansion();
}

void MainWindow::dirFilterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;

    int dir = item->data(Qt::UserRole).toInt();
    bool isSet = (item->checkState() == Qt::Checked);

    model->setDirFilterState(dir, isSet);

    manageRowExpansion();
}

void MainWindow::typeFilterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;

    int type = item->data(Qt::UserRole).toInt();
    bool isSet = (item->checkState() == Qt::Checked);

    model->setTypeFilterState(type, isSet);

    manageRowExpansion();
}

void MainWindow::lengthFilterListItemChanged(QListWidgetItem *item)
{
    if (inhibitFilterUpdate) return;

    int len = FilterUtility::getIdAsInt(item);
    bool isSet = (item->checkState() == Qt::Checked);

    model->setLengthFilterState(len, isSet);

    manageRowExpansion();
}



void MainWindow::filterSetAll()
{
    inhibitFilterUpdate = true;
    for (int i = 0; i < ui->listFilters->count(); i++)
    {
        ui->listFilters->item(i)->setCheckState(Qt::Checked);
    }
    inhibitFilterUpdate = false;
    model->setAllFilters(true);

    manageRowExpansion();
}

void MainWindow::filterClearAll()
{
    inhibitFilterUpdate = true;
    for (int i = 0; i < ui->listFilters->count(); i++)
    {
        ui->listFilters->item(i)->setCheckState(Qt::Unchecked);
    }
    inhibitFilterUpdate = false;
    model->setAllFilters(false);
}

void MainWindow::logReceivedFrame(CANConnection* conn, QVector<CommFrame> frames)
{
    Q_UNUSED(conn);
    if (!continuousLogging) return;

    if (!continuousLoggingUseFilter)
    {
        FrameFileIO::writeContinuousNative(&frames, 0);
    }
    else
    {
        QVector<CommFrame> filtered;
        for (const CommFrame &f : std::as_const(frames))
        {
            if (model->passesFrameFilters(f))
                filtered.append(f);
        }
        if (!filtered.isEmpty())
            FrameFileIO::writeContinuousNative(&filtered, 0);
    }
}

void MainWindow::updateDroppedStatus()
{
    const quint64 dropped = CANConManager::getInstance()->getDroppedFrames();
    if (dropped == lastShownDropped) return;
    lastShownDropped = dropped;
    lbStatusDropped.setText(tr("Dropped: %1").arg(dropped));
    lbStatusDropped.setStyleSheet(dropped ? QStringLiteral("color: #d03030; font-weight: bold;") : QString());
}

void MainWindow::tickGUIUpdate()
{
    updateDroppedStatus();
    rxFrames = model->sendBulkRefresh();
    //if(rxFrames>0)
    //{
        int elapsed = elapsedTime->elapsed();
        if(elapsed) {
            framesPerSec = (framesPerSec + (rxFrames * 1000 / elapsed)) / 2;
            elapsedTime->restart();
        }
        else
            framesPerSec = 0;

        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (rxFrames > 0 && /*allowCapture && */ ui->cbAutoScroll->isChecked())
                ui->canFramesView->scrollToBottom();
        ui->lbFPS->setText(QString::number(framesPerSec));
        if (rxFrames > 0)
        {
            bDirty = true;
            emit framesUpdated(rxFrames); //anyone care that frames were updated?
            manageRowExpansion();
        }

        if (model->needsFilterRefresh()) updateFilterList();

        if (continuousLogging)
        {
//            const QVector<CommFrame> *modelFrames = model->getListReference();
//            FrameFileIO::writeContinuousNative(modelFrames, modelFrames->count() - rxFrames);

            continuousLogFlushCounter++;
            if ((continuousLogFlushCounter % 3) == 0)
            {
#ifdef Q_OS_WASM
                ui->lblContMsg->setText(webLogLabel(!ui->lblContMsg->text().startsWith("LOGGING")));
#else
                if (ui->lblContMsg->text().length() > 2)
                {
                    ui->lblContMsg->setText("");
                }
                else
                {
                    ui->lblContMsg->setText("LOGGING");
                }
#endif
            }
            if (continuousLogFlushCounter > 8)
            {
                continuousLogFlushCounter = 0;
                FrameFileIO::flushContinuousNative();
            }
        }

        //refresh the count for all the frame senders
        FrameSendData *tempData;
        int numRows = ui->tableSimpleSender->rowCount();
        for (int i = 0; i < numRows; i++)
        {
            tempData = frameSender->getSendRecordRef(i);
            if (tempData)
            {
                ui->tableSimpleSender->item(i, SIMP_COL::SC_COL_COUNT)->setText(QString::number( tempData->count ));
            }
        }

        rxFrames = 0;
    //}
}

void MainWindow::gotFrames(int framesSinceLastUpdate)
{
    rxFrames += framesSinceLastUpdate;
    emit frameUpdateRapid(framesSinceLastUpdate);
}

void MainWindow::addFrameToDisplay(CommFrame &frame, bool autoRefresh = false)
{
    model->addFrame(frame, autoRefresh);
    if (autoRefresh)
    {
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
    }
}

//A sub-window is sending us a center on timestamp and ID signal
//try to find the relevant frame in the list and focus on it.
void MainWindow::gotCenterTimeID(uint32_t ID, double timestamp)
{
    int idx = model->getIndexFromTimeID(ID, timestamp);
    if (idx > -1)
    {
        ui->canFramesView->selectRow(idx);
    }
}

void MainWindow::clearFrames()
{
    ui->canFramesView->scrollToTop();
    model->clearFrames();
    CANConManager::getInstance()->resetTimeBasis();
    CANConManager::getInstance()->resetDroppedFrames();
    updateDroppedStatus();
    ui->lbNumFrames->setText(QString::number(model->rowCount()));
    bDirty = false;
    loadedFileName = "";
    updateFileStatus();
    emit framesUpdated(-1);
}

void MainWindow::normalizeTiming()
{
    model->normalizeTiming();
    emit framesUpdated(-2); //claim an all new set of frames because every frame was updated.
}

void MainWindow::handleCropLog()
{
    if (!Features::on(Features::Feature::CropLog)) return; // web-savvycan: enabledfeatures.h
    const FrameListView *src = model->getListReference();
    if (src->isEmpty())
    {
        QMessageBox::information(this, tr("Crop Log"), tr("No frames loaded."));
        return;
    }

    CropLogDialog dlg(src, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    int startIdx = dlg.startIndex();
    int endIdx   = dlg.endIndex();
    QVector<CommFrame> cropped = src->mid(startIdx, endIdx - startIdx + 1);

    model->clearFrames();
    model->insertFrames(cropped);
    CANConManager::getInstance()->resetTimeBasis();
    ui->lbNumFrames->setText(QString::number(model->rowCount()));
    bDirty = true;
    emit framesUpdated(-2);
}

void MainWindow::handleLoadFile()
{
    if (!Features::on(Features::Feature::LoadLog)) return; // web-savvycan: enabledfeatures.h
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the file comes from the visitor's computer through the browser's
    // own file picker (Qt's dialog would only see the page's empty in-memory file system). The type is
    // detected from the contents, as SavvyCAN's "Autodetect File Type".
    QFileDialog::getOpenFileContent(QString(), [this](const QString &name, const QByteArray &content) {
        if (!name.isEmpty()) loadBrowserFile(name, content); // empty: the picker was cancelled
    });
    return;
#endif
    QString filename;
    QVector<CommFrame> tempFrames;

    QMessageBox::StandardButton confirmDialog;

    bool loadResult = FrameFileIO::loadFrameFile(filename, &tempFrames);

    if (!loadResult)
    {
        if (tempFrames.count() > 0) //only ask if at least one frame was decoded.
        {
            confirmDialog = QMessageBox::question(this, "Error Loading", "Do you want to salvage what could be loaded?",
                                      QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog == QMessageBox::Yes) {
                loadResult = true;
            }
        }
    }

    if (loadResult)
    {
        disableAutoRowExpansion();
        ui->canFramesView->scrollToTop();
        model->clearFrames();
        model->insertFrames(tempFrames);
        loadedFileName = filename;
        model->recalcOverwrite();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();

        updateFileStatus();
        emit framesUpdated(-1);
    }
}

#ifdef Q_OS_WASM
// A file picked in the browser. SavvyCAN's own GVRET CSV (what the browser version logs) is parsed straight from the
// bytes the browser handed over (loadNativeCSVData); other formats are written to the in-memory file system for
// SavvyCAN's loaders (they read by file name), loaded as a dropped file is, and deleted again. Everything lives in the
// page's memory (wasm32: at most 4 GB), so larger files are refused with a message. Measured 2026-10-29 (headless
// Chromium, tests/web loader.spec): a GVRET CSV of 14 M frames (976 MB) loads in 27-36 s, then takes 0.8 GB of the
// heap (57 bytes a frame); while loading the file's bytes, the frames staged and the trace are in memory together:
// 1.6 GB in use at the peak, the wasm memory 2.1 GB (3 M frames, 207 MB: 6 s, 0.19 GB, peak 0.34-0.39 GB). So up to
// kMaxBrowserFileBytes (~14 M frames, hours of a busy bus), with room left for the graphs and RE tools. Other formats
// go through SavvyCAN's loaders and a vector of whole frames, plus the file's copy in the file system: a 1 M-frame
// candump log (46 MB) took 18 s and 254 MB of the heap at its peak, so they are kept to kMaxBrowserOtherFileBytes.
void MainWindow::loadBrowserFile(const QString &name, const QByteArray &content)
{
    constexpr qint64 kMaxBrowserFileBytes = 1000LL * 1000 * 1000;     // SavvyCAN's GVRET CSV
    constexpr qint64 kMaxBrowserOtherFileBytes = 200LL * 1000 * 1000; // the other formats
    const QString shown = QFileInfo(name).fileName();
    auto tooLarge = [&]() {
        QMessageBox::warning(this, tr("File too large"),
                             tr("%1 is %2 MB. The browser version loads SavvyCAN's GVRET CSV logs up to %3 MB and "
                                "other formats up to %4 MB (a web page has at most 4 GB of memory). Crop or split "
                                "the log, or use the desktop version.")
                                 .arg(shown).arg(content.size() / 1000000).arg(kMaxBrowserFileBytes / 1000000)
                                 .arg(kMaxBrowserOtherFileBytes / 1000000));
    };
    if (content.size() > kMaxBrowserFileBytes) {
        tooLarge();
        return;
    }
    //the autodetect order: Vector BLF ("LOGG") first, then the native CSV
    if (!content.startsWith("LOGG") && FrameFileIO::isNativeCSVData(content)
        && loadNativeCSVData(shown, QByteArrayView(content)))
        return;
    if (content.size() > kMaxBrowserOtherFileBytes) {
        tooLarge();
        return;
    }
    QDir().mkpath(QStringLiteral("/tmp/web-savvycan"));
    const QString path = QStringLiteral("/tmp/web-savvycan/") + shown;
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
            QFile::remove(path);
            QMessageBox::warning(this, tr("Error Loading"), tr("%1 could not be read.").arg(shown));
            return;
        }
    }
    handleDroppedFile(path);
    QFile::remove(path);
    if (loadedFileName == path) {
        loadedFileName = shown;
        updateFileStatus();
    }
}
#endif

// web-savvycan: a log in SavvyCAN's native GVRET CSV from memory, the leaner log loader (RESOLVE.md): parsed in place
// into a compact staging store (FrameFileIO::loadNativeCSVData, no QByteArray per line, field or frame), then handed
// to the trace chunk by chunk, each chunk freed as it goes in. A progress dialog if it takes more than a second; the
// page keeps running meanwhile (FrameFileIO::keepUiAlive). As SavvyCAN's loader: a file with broken rows asks whether
// to keep what could be read; the trace is replaced only when the file is taken. Returns false if not a single frame
// could be read (the caller then tries SavvyCAN's other loaders).
bool MainWindow::loadNativeCSVData(const QString &fileName, QByteArrayView data)
{
    if (!Features::on(Features::Feature::LoadLog)) return true; // web-savvycan: enabledfeatures.h
    QProgressDialog progress(qApp->activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.setLabelText(tr("Loading %1...").arg(fileName));
    progress.setCancelButton(nullptr);
    progress.setRange(0, 1000);
    progress.setMinimumDuration(1000);
    progress.setValue(0);
    FrameFileIO::progressHook = [&progress](qint64 done, qint64 total) {
        progress.setValue(int(qMin<qint64>(990, done * 1000 / total)));
    };
    FrameStore staged(std::numeric_limits<qsizetype>::max(), FrameStore::DefaultChunkFrames);
    bool loadResult = FrameFileIO::loadNativeCSVData(data, &staged);
    FrameFileIO::progressHook = nullptr;
    if (!loadResult)
    {
        if (staged.isEmpty()) return false;
        progress.cancel();
        if (QMessageBox::question(this, "Error Loading", "Do you want to salvage what could be loaded?",
                                  QMessageBox::Yes|QMessageBox::No) != QMessageBox::Yes)
            return true;
    }
    else
    {
        progress.setLabelText(tr("Adding %1 frames to the trace...").arg(staged.size()));
        progress.setValue(995);
    }

    disableAutoRowExpansion();
    ui->canFramesView->scrollToTop();
    model->clearFrames();
    model->insertFrames(staged);
    progress.cancel();
    loadedFileName = fileName;
    model->recalcOverwrite();
    ui->lbNumFrames->setText(QString::number(model->rowCount()));
    if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();

    updateFileStatus();
    emit framesUpdated(-1);
    return true;
}

void MainWindow::handleDroppedFile(const QString &filename)
{
    if (!Features::on(Features::Feature::LoadLog)) return; // web-savvycan: enabledfeatures.h
    QProgressDialog progress(qApp->activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.setLabelText("Loading file...");
    progress.setCancelButton(nullptr);
    progress.setRange(0,0);
    progress.setMinimumDuration(0);
    progress.show();
    
    QVector<CommFrame> loadedFrames;
    bool loadResult = FrameFileIO::autoDetectLoadFile(filename, &loadedFrames);
    
    progress.cancel();
    
    if (!loadResult)
    {
        if (loadedFrames.count() > 0) //only ask if at least one frame was decoded.
        {
            QMessageBox::StandardButton confirmDialog = QMessageBox::question(this, "Error Loading", "Do you want to salvage what could be loaded?",
                                      QMessageBox::Yes|QMessageBox::No);
            if (confirmDialog == QMessageBox::Yes)
            {
                loadResult = true;
            }
        }
    }

    if (loadResult)
    {
        disableAutoRowExpansion();
        ui->canFramesView->scrollToTop();
        model->clearFrames();
        model->insertFrames(loadedFrames);
        loadedFileName = filename;
        model->recalcOverwrite();
        ui->lbNumFrames->setText(QString::number(model->rowCount()));
        if (ui->cbAutoScroll->isChecked()) ui->canFramesView->scrollToBottom();

        updateFileStatus();
        emit framesUpdated(-1);
    }
}


void MainWindow::handleSaveFile()
{
    QString filename;

    const QVector<CommFrame> snapshot = model->getListReference()->toVector();
    if (FrameFileIO::saveFrameFile(filename, &snapshot))
    {
        loadedFileName = filename;
        updateFileStatus();
    }
}

void MainWindow::handleContinousLogging()
{
#ifdef Q_OS_WASM
    if (continuousLogging && !askWebLogName()) return; //Keep Logging
#endif
    // If already doing unfiltered logging, stop it
    if (continuousLogging && !continuousLoggingUseFilter)
    {
        continuousLogging = false;
        ui->actionSave_Continuous_Logfile->setText(tr("Start Continuous Logging"));
        ui->lblContMsg->setText("");
        FrameFileIO::closeContinuousNative();
        return;
    }

    // If filtered logging is running, stop it before switching
    if (continuousLogging)
    {
        FrameFileIO::closeContinuousNative();
        continuousLogging = false;
        ui->actionSave_Continuous_Filtered_Logfile->setText(tr("Start Continuous Logging (Filtered)"));
        ui->lblContMsg->setText("");
    }

    // Start unfiltered logging — open file first; if cancelled, do nothing
    if (!FrameFileIO::openContinuousNative())
        return;

    continuousLoggingUseFilter = false;
    continuousLogging = true;
    ui->actionSave_Continuous_Logfile->setText(tr("Cease Continuous Logging"));
}

void MainWindow::handleContinousLoggingFiltered()
{
#ifdef Q_OS_WASM
    if (continuousLogging && !askWebLogName()) return; //Keep Logging
#endif
    // If already doing filtered logging, stop it
    if (continuousLogging && continuousLoggingUseFilter)
    {
        continuousLogging = false;
        ui->actionSave_Continuous_Filtered_Logfile->setText(tr("Start Continuous Logging (Filtered)"));
        ui->lblContMsg->setText("");
        FrameFileIO::closeContinuousNative();
        return;
    }

    // If unfiltered logging is running, stop it before switching
    if (continuousLogging)
    {
        FrameFileIO::closeContinuousNative();
        continuousLogging = false;
        ui->actionSave_Continuous_Logfile->setText(tr("Start Continuous Logging"));
        ui->lblContMsg->setText("");
    }

    // Start filtered logging — open file first; if cancelled, do nothing
    if (!FrameFileIO::openContinuousNative())
        return;

    continuousLoggingUseFilter = true;
    continuousLogging = true;
    ui->actionSave_Continuous_Filtered_Logfile->setText(tr("Cease Continuous Logging (Filtered)"));
}

#ifdef Q_OS_WASM
//Continuous logging in the browser: the same menu items and FrameFileIO calls; the log is collected in the page
//and downloaded when it stops (WebDiskLog). The LOGGING label (it blinks, as on the desktop) shows its size so far.
QString MainWindow::webLogLabel(bool blinkOn)
{
    const qint64 bytes = WebDiskLog::bytesWritten();
    const QString size = bytes < 10 * 1024 ? tr("%1 bytes").arg(bytes)
                       : bytes < 10 * 1024 * 1024 ? tr("%1 kB").arg(bytes / 1024)
                       : tr("%1 MB").arg(bytes / (1024 * 1024));
    return (blinkOn ? QStringLiteral("LOGGING") : QStringLiteral(" ")) + QLatin1Char('\n')
           + tr("%1, %2 frames").arg(size).arg(WebDiskLog::framesWritten()) + QLatin1Char('\n')
           + tr("Not saved yet: downloaded on stop,\nlost if the tab crashes");
}

//With "Choose the Log's Name When Logging Stops..." on: the download's name. false: Keep Logging.
bool MainWindow::askWebLogName()
{
    if (!webLogAskNameAction->isChecked()) return true;
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Cease Continuous Logging"));
    dialog.setLabelText(tr("Name of the log file to download:"));
    dialog.setTextValue(WebDiskLog::fileName());
    dialog.setOkButtonText(tr("Download"));
    dialog.setCancelButtonText(tr("Keep Logging"));
    if (dialog.exec() != QDialog::Accepted || !continuousLogging) return false;
    WebDiskLog::setFileName(dialog.textValue().trimmed());
    return true;
}
#endif

void MainWindow::handleSaveFilteredFile()
{
    QString filename;

    const QVector<CommFrame> snapshot = model->getFilteredListReference()->toVector();
    if (FrameFileIO::saveFrameFile(filename, &snapshot))
    {
        loadedFileName = filename;
        updateFileStatus();
    }
}

void MainWindow::handleSaveFilters()
{
    if (!Features::on(Features::Feature::FilterFiles)) return; // web-savvycan: enabledfeatures.h
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Filter list (*.ftl)")));

    dialog.setDirectory(settings.value("Filters/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        if (!filename.contains('.')) filename += ".ftl";
        if (dialog.selectedNameFilter() == filters[0]) model->saveFilterFile(filename);
        settings.setValue("Filters/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::handleLoadFilters()
{
    if (!Features::on(Features::Feature::FilterFiles)) return; // web-savvycan: enabledfeatures.h
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Filter List (*.ftl)")));

    dialog.setDirectory(settings.value("Filters/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        //right now there is only one file type that can be loaded here so just do it.
        model->loadFilterFile(filename);
        settings.setValue("Filters/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::handleSaveDecoded()
{
    if (!Features::on(Features::Feature::DBC)) return; // web-savvycan: enabledfeatures.h
    handleSaveDecodedMethod(false);
}

void MainWindow::handleSaveDecodedCsv()
{
    if (!Features::on(Features::Feature::DBC)) return; // web-savvycan: enabledfeatures.h
    handleSaveDecodedMethod(true);
}

void MainWindow::handleSaveDecodedMethod(bool csv)
{
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    if (!csv) filters.append(QString(tr("Text File (*.txt *.TXT)")));
    else filters.append(QString(tr("CSV File (*.csv *.CSV)")));

    dialog.setDirectory(settings.value("FileIO/LoadSaveDirectory", dialog.directory().path()).toString());
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];
        if (!filename.contains('.'))
        {
            if (!csv) filename += ".txt";
            else filename += ".csv";
        }

        if(csv)
            saveDecodedTextFileAsColumns(filename);
        else
            saveDecodedTextFile(filename);

        settings.setValue("FileIO/LoadSaveDirectory", dialog.directory().path());
    }
}

void MainWindow::saveDecodedTextFileAsColumns(QString filename)
{
    QFile *outFile = new QFile(filename);
    const QVector<CommFrame> snapshot = model->getFilteredListReference()->toVector();
    const QVector<CommFrame> *frames = &snapshot;

    //const unsigned char *data;
    int dataLen;
    const CommFrame *frame;

    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
        return;
/*
Time: 205.173000   ID: 0x20E Std Bus: 0 Len: 8
Data Bytes: 88 10 00 13 BB 00 06 00
    SignalName	Value
*/
    QList<QPair<uint32_t, int>> msgsAndColumns;
    int columnsAdded = 0;
    int dataStartCol = 0;

    QString builderString;
    if (CSVAbsTime)
    {
        builderString += tr("Year") + "," + tr("Month") + "," + tr("Day") + "," + tr("Hour") + "," + tr("Minute") + "," + tr("Second") + "," + tr("Ms") + ",";
        dataStartCol += 7;
    }
    else
    {
        //time
        builderString += tr("Time") + ",";
        dataStartCol++;
    }
    //id
    builderString += tr("ID") + ",";
    dataStartCol++;
    //if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
    //else builderString += tr(" Std ");
    //bus
    builderString += tr("Bus") + ",";
    dataStartCol++;
    //len
    builderString += tr("DataLen") + ",";
    dataStartCol++;

    columnsAdded = dataStartCol;

    //loop through all the frames and the message data therein
    for (int c = 0; c < frames->count(); c++)
    {
        frame = &frames->at(c);
        //data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().length();

        //add all column names
        if (dbcHandler != nullptr)
        {            
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {                
                QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
                bool found = false;
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {
                    if(j==0)
                    {
                        for(int m = 0; m < msgsAndColumns.count(); m++)
                        {
                            if(msgsAndColumns[m].first == msg->ID)
                                found = true;
                        }
                        if(found == false)
                            msgsAndColumns.append(QPair<uint32_t,int>(msg->ID, columnsAdded));
                    }

                    if(found == false)
                    {
                        QString temp;
                        if (sigs[j]->processAsText(*frame, temp))
                        {
                            builderString.append(sigs[j]->name);
                            builderString.append(",");
                            columnsAdded++;
                        }
                    }
                }
            }
        }
    }

    //add EOL
    builderString += "\n";
    //write out the header row
    outFile->write(builderString.toUtf8());

        //builderString = tr("Data Bytes: ");
        //for (int temp = 0; temp < dataLen; temp++)
        //{
        //    builderString += Utility::formatNumber(data[temp]) + " ";
        //}
        //builderString += "\n";
        //outFile->write(builderString.toUtf8());

    int dataColumnsAdded = 0;
    builderString = "";
    for (int c = 0; c < frames->count(); c++)
    {
        dataColumnsAdded = 0;
        frame = &frames->at(c);
        //data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().length();

        QString builderString;
        if (CSVAbsTime)
        {
            QDateTime dt = QDateTime::fromMSecsSinceEpoch(frame->timeStamp().microSeconds() / 1000);
            builderString += QString::number(dt.date().year()) + "," + QString::number(dt.date().month()) + ",";
            builderString += QString::number(dt.date().day()) + "," + QString::number(dt.time().hour()) + ",";
            builderString += QString::number(dt.time().minute()) + "," + QString::number(dt.time().second()) + ",";
            builderString += QString::number(dt.time().msec()) + ",";
            dataColumnsAdded += 7;
        }
        else {
            builderString += QString::number((frame->timeStamp().microSeconds() / 1000000.0), 'f', 6) + ",";
            dataColumnsAdded++;
        }
        //id
        builderString += Utility::formatCANID(frame->frameId(), frame->hasExtendedFrameFormat()) + ",";
        dataColumnsAdded++;
        //if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
        //else builderString += tr(" Std ");
        //bus
        builderString += QString::number(frame->getBus()) + ",";
        dataColumnsAdded++;
        //len
        builderString += QString::number(dataLen) + ",";
        dataColumnsAdded++;

        if (dbcHandler != nullptr)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {
                QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {
                    if(j==0)
                    {
                        for(int i = 0; i<msgsAndColumns.count(); i++)
                        {
                            if(msgsAndColumns[i].first == msg->ID)
                            {
                                int startCol = msgsAndColumns[i].second;
                                while(dataColumnsAdded < startCol)
                                {
                                    builderString += ",";
                                    dataColumnsAdded++;
                                }
                            }
                        }
                    }

                    QString temp;
                    if (sigs[j]->processAsText(*frame, temp))
                    {
                        builderString.append(temp);
                        builderString.append(",");
                        dataColumnsAdded++;
                    }
                }
            }
            builderString.append("\n");
            outFile->write(builderString.toUtf8());
        }
    }
    outFile->close();
}

void MainWindow::saveDecodedTextFile(QString filename)
{
    QFile *outFile = new QFile(filename);
    const QVector<CommFrame> snapshot = model->getFilteredListReference()->toVector();
    const QVector<CommFrame> *frames = &snapshot;

    const unsigned char *data;
    int dataLen;
    const CommFrame *frame;

    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
        return;
/*
Time: 205.173000   ID: 0x20E Std Bus: 0 Len: 8
Data Bytes: 88 10 00 13 BB 00 06 00
    SignalName	Value
*/
    for (int c = 0; c < frames->count(); c++)
    {
        frame = &frames->at(c);
        data = reinterpret_cast<const unsigned char *>(frame->payload().constData());
        dataLen = frame->payload().length();

        QString builderString;
        builderString += tr("Time: ") + QString::number((frame->timeStamp().microSeconds() / 1000000.0), 'f', 6);
        builderString += tr("    ID: ") + Utility::formatCANID(frame->frameId(), frame->hasExtendedFrameFormat());
        if (frame->hasExtendedFrameFormat()) builderString += tr(" Ext ");
        else builderString += tr(" Std ");
        builderString += tr("Bus: ") + QString::number(frame->getBus());
        builderString += " Len: " + QString::number(dataLen) + "\n";
        outFile->write(builderString.toUtf8());

        builderString = tr("Data Bytes: ");
        for (int temp = 0; temp < dataLen; temp++)
        {
            builderString += Utility::formatNumber(data[temp]) + " ";
        }
        builderString += "\n";
        outFile->write(builderString.toUtf8());

        builderString = "";
        if (dbcHandler != nullptr)
        {
            DBC_MESSAGE *msg = dbcHandler->findMessage(*frame);
            if (msg != nullptr)
            {
                QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
                for (int j = 0; j < msg->sigHandler->getCount(); j++)
                {

                    QString temp;
                    if (sigs[j]->processAsText(*frame, temp))
                    {
                        builderString.append("\t" + temp);
                        builderString.append("\n");
                    }
                }
            }
            builderString.append("\n");
            outFile->write(builderString.toUtf8());
        }
    }
    outFile->close();
}

void MainWindow::toggleCapture()
{
    allowCapture = !allowCapture;
    if (allowCapture)
        ui->btnCaptureToggle->setText("Suspend Capturing");
    else
        ui->btnCaptureToggle->setText("Restart Capturing");

    emit suspendCapturing(!allowCapture);
}

void MainWindow::connectionStatusUpdated(int conns)
{
    lbStatusConnected.setText(tr("Connected to ") + QString::number(conns) + tr(" buses"));
}

void MainWindow::updateFileStatus()
{
    QString output;
    if (model->rowCount() == 0)
    {
        output = tr("No packets loaded");
    }
    else
    {
        if (loadedFileName.length() > 2)
        {
            output = loadedFileName + " loaded";
        }
        else
        {
            output = tr("No file loaded");
        }

        if (bDirty)
        {
            output += " (X)";
        }
    }
    lbStatusFilename.setText(output);
}

CommFrameModel* MainWindow::getCommFrameModel()
{
    return model;
}


/*
 * All functions past this point set up the various other windows that can be opened
*/

void MainWindow::showSettingsDialog()
{
    if (!settingsDialog)
    {
        settingsDialog = new MainSettingsDialog();
        connect (settingsDialog, SIGNAL(updatedSettings()), this, SLOT(readUpdateableSettings()));
    }
    settingsDialog->show();
}

//always gets unfiltered list. You ask for the graphs so there is no need to send filtered frames
//now always creates a new window. This allows for multiple independent graphing windows
void MainWindow::showGraphingWindow()
{
    if (!Features::on(Features::Feature::Graphs)) return; // web-savvycan: enabledfeatures.h
/* could only allow the latest window to have these centering signals.
   if (lastGraphingWindow)
    {
        disconnect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(int32_t,double)));
        disconnect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
        if (flowViewWindow)
        {
            disconnect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
            disconnect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
        }
    }
*/
    const FrameListView *frames;
    if (!useFiltered)
        frames = model->getListReference();
    else
        frames = model->getFilteredListReference();
    lastGraphingWindow = new GraphingWindow(frames);
    graphWindows.append(lastGraphingWindow);

    connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(uint32_t,double)));
    connect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(uint32_t,double)));

    if (flowViewWindow) //connect the two external windows together
    {
        connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(uint32_t,double)));
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(uint32_t,double)));
    }

    lastGraphingWindow->show();
}

void MainWindow::showTemporalGraphWindow()
{
    if (!Features::on(Features::Feature::TemporalGraph)) return; // web-savvycan: enabledfeatures.h
    //only create an instance of the object if we dont have one. Otherwise just display the existing one.
    if (!temporalGraphWindow)
    {
        const FrameListView *frames;
        if (!useFiltered)
            frames = model->getListReference();
        else
            frames = model->getFilteredListReference();

        //web-savvycan: no "Danger Will Robinson" question past 2000 frames: the map is bounded now (G1-G4) and is
        //built in slices, so a big capture neither crashes it nor stalls the page
        temporalGraphWindow = new TemporalGraphWindow(frames);
    }

    temporalGraphWindow->show();
}

void MainWindow::showFrameDataAnalysis()
{
    if (!Features::on(Features::Feature::FrameInfo)) return; // web-savvycan: enabledfeatures.h
    //only create an instance of the object if we dont have one. Otherwise just display the existing one.
    if (!frameInfoWindow)
    {
        if (!useFiltered)
            frameInfoWindow = new FrameInfoWindow(model->getListReference());
        else
            frameInfoWindow = new FrameInfoWindow(model->getFilteredListReference());
    }
    frameInfoWindow->show();
}

void MainWindow::showISOInterpreterWindow()
{
    if (!Features::on(Features::Feature::ISOTP)) return; // web-savvycan: enabledfeatures.h
    if (!isoWindow)
    {
        if (!useFiltered)
            isoWindow = new ISOTP_InterpreterWindow(model->getListReference());
        else
            isoWindow = new ISOTP_InterpreterWindow(model->getFilteredListReference());
    }
    isoWindow->show();
}

void MainWindow::showSnifferWindow()
{
    if (!Features::on(Features::Feature::Sniffer)) return; // web-savvycan: enabledfeatures.h
    if (!snifferWindow)
        snifferWindow = new SnifferWindow(this);
    snifferWindow->show();
}

void MainWindow::showBisectWindow()
{
    if (!Features::on(Features::Feature::Bisector)) return; // web-savvycan: enabledfeatures.h
    if (!bisectWindow)
    {
        bisectWindow = new BisectWindow(model->getListReference());
    }
    bisectWindow->show();
}

void MainWindow::showCANBridgeWindow()
{
    if (!Features::on(Features::Feature::CANBridge)) return; // web-savvycan: enabledfeatures.h
    if (!canBridgeWindow)
    {
        canBridgeWindow = new CANBridgeWindow(model->getListReference());
    }
    canBridgeWindow->show();
}

void MainWindow::showSearchWindow()
{
    if (!Features::on(Features::Feature::FrameSearch)) return; // web-savvycan: enabledfeatures.h
    if (!searchWindow)
    {
        searchWindow = new SearchWindow(model->getFilteredListReference(), dbcHandler);
        connect(searchWindow, &SearchWindow::jumpToFrameIndex, this, &MainWindow::jumpToSearchFrame);
        connect(this, &MainWindow::framesUpdated, searchWindow, &SearchWindow::updatedFrames);
        // Clear stale results whenever the filter changes (sendRefresh rebuilds filteredFrames)
        connect(model, &QAbstractItemModel::modelReset, searchWindow, [this]() {
            searchWindow->updatedFrames(0);
        });
    }
    searchWindow->show();
    searchWindow->raise();
}

void MainWindow::jumpToSearchFrame(int frameIndex)
{
    if (!model || frameIndex < 0 || frameIndex >= model->getFilteredListReference()->size())
        return;

    // frameIndex is a row in CommFrameModel (filteredFrames); map through the
    // sort proxy so the view scrolls to the correct visible row.
    QAbstractProxyModel *proxy = qobject_cast<QAbstractProxyModel *>(ui->canFramesView->model());
    QModelIndex srcIndex = model->index(frameIndex, 0);
    QModelIndex target = proxy ? proxy->mapFromSource(srcIndex) : srcIndex;
    if (target.isValid())
    {
        ui->canFramesView->scrollTo(target, QAbstractItemView::PositionAtCenter);
        ui->canFramesView->setCurrentIndex(target);
    }
}

void MainWindow::showFrameSenderWindow()
{
    if (!Features::on(Features::Feature::FrameSender)) return; // web-savvycan: enabledfeatures.h
    if (!frameSenderWindow)
    {
        if (!useFiltered)
            frameSenderWindow = new FrameSenderWindow(model->getListReference());
        else
            frameSenderWindow = new FrameSenderWindow(model->getFilteredListReference());
    }
    frameSenderWindow->show();
}

void MainWindow::showPlaybackWindow()
{
    if (!Features::on(Features::Feature::Playback)) return; // web-savvycan: enabledfeatures.h
    if (!playbackWindow)
    {
        if (!useFiltered)
            playbackWindow = new FramePlaybackWindow(model->getListReference());
        else
            playbackWindow = new FramePlaybackWindow(model->getFilteredListReference());
    }
    playbackWindow->show();
}

void MainWindow::showUDSFirmwareUploaderWindow()
{
    if (!Features::on(Features::Feature::UDSFirmwareUpdate)) return; // web-savvycan: enabledfeatures.h
    if (!udsFirmwareUploaderWindow)
    {
        udsFirmwareUploaderWindow = new UDSFirmwareUploaderWindow(model->getListReference());
    }
    udsFirmwareUploaderWindow->show();
}

void MainWindow::showComparisonWindow()
{
    if (!Features::on(Features::Feature::FileComparator)) return; // web-savvycan: enabledfeatures.h
    if (!comparatorWindow)
    {
        comparatorWindow = new FileComparatorWindow();
    }
    comparatorWindow->show();
}

void MainWindow::showDBCComparisonWindow()
{
    if (!Features::on(Features::Feature::DBCComparator)) return; // web-savvycan: enabledfeatures.h
    if (!dbcComparatorWindow)
    {
        dbcComparatorWindow = new DBCComparatorWindow();
    }
    dbcComparatorWindow->show();
}

void MainWindow::showSingleMultiWindow()
{
    if (!Features::on(Features::Feature::DiscreteState)) return; // web-savvycan: enabledfeatures.h
    if (!discreteStateWindow)
    {
        discreteStateWindow = new DiscreteStateWindow(model->getListReference());
    }
    discreteStateWindow->show();
}

void MainWindow::showFuzzingWindow()
{
    if (!Features::on(Features::Feature::Fuzzing)) return; // web-savvycan: enabledfeatures.h
    if (!fuzzingWindow)
    {
        fuzzingWindow = new FuzzingWindow(model->getListReference());
    }
    fuzzingWindow->show();
}

void MainWindow::showMCConfigWindow()
{
    if (!Features::on(Features::Feature::MotorControlConfig)) return; // web-savvycan: enabledfeatures.h
    if (!motorctrlConfigWindow)
    {
        motorctrlConfigWindow = new MotorControllerConfigWindow(model->getListReference());
        //connect(motorctrlConfigWindow, SIGNAL(sendCommFrame(const CommFrame*,int)), worker, SLOT(sendFrame(const CommFrame*,int)));
        //connect(motorctrlConfigWindow, SIGNAL(sendFrameBatch(const QList<CommFrame>*)), worker, SLOT(sendFrameBatch(const QList<CommFrame>*)));
    }
    motorctrlConfigWindow->show();
}

void MainWindow::showUDSScanWindow()
{
    if (!Features::on(Features::Feature::UDSScanner)) return; // web-savvycan: enabledfeatures.h
    if (!udsScanWindow)
    {
        udsScanWindow = new UDSScanWindow(model->getListReference());
    }
    udsScanWindow->show();
}

void MainWindow::showScriptingWindow()
{
    if (!Features::on(Features::Feature::Scripting)) return; // web-savvycan: enabledfeatures.h
#ifndef Q_OS_WASM //no scripting (Qt Qml) in the browser build
    if (!scriptingWindow)
    {
        scriptingWindow = new ScriptingWindow(model->getListReference());
    }
    scriptingWindow->show();
#endif
}

void MainWindow::showRangeWindow()
{
    if (!Features::on(Features::Feature::RangeState)) return; // web-savvycan: enabledfeatures.h
    if (!rangeWindow)
    {
        rangeWindow = new RangeStateWindow(model->getListReference());
    }
    rangeWindow->show();
}

void MainWindow::showFuzzyScopeWindow()
{
    //not done yet
}

void MainWindow::showFlowViewWindow()
{
    if (!Features::on(Features::Feature::FlowView)) return; // web-savvycan: enabledfeatures.h
    if (!flowViewWindow)
    {
        if (!useFiltered)
            flowViewWindow = new FlowViewWindow(model->getListReference());
        else
            flowViewWindow = new FlowViewWindow(model->getFilteredListReference());
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), this, SLOT(gotCenterTimeID(int32_t,double)));
        connect(this, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
    }

    if (lastGraphingWindow)
    {
        connect(lastGraphingWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), flowViewWindow, SLOT(gotCenterTimeID(int32_t,double)));
        connect(flowViewWindow, SIGNAL(sendCenterTimeID(uint32_t,double)), lastGraphingWindow, SLOT(gotCenterTimeID(int32_t,double)));
    }

    flowViewWindow->show();
}


void MainWindow::DBCSettingsUpdated()
    {
    updateFilterList();
    model->sendRefresh();
    }

void MainWindow::showDBCFileWindow()
{
    if (!Features::on(Features::Feature::DBC)) return; // web-savvycan: enabledfeatures.h
    if (!dbcFileWindow)
    {
        dbcFileWindow = new DBCLoadSaveWindow(model->getListReference());
        connect(dbcFileWindow, &DBCLoadSaveWindow::updatedDBCSettings, this, &MainWindow::DBCSettingsUpdated);
    }
    dbcFileWindow->show();
}

void MainWindow::showSignalViewer()
{
    if (!Features::on(Features::Feature::SignalViewer)) return; // web-savvycan: enabledfeatures.h
    if (!signalViewerWindow)
    {
        if (!useFiltered)
            signalViewerWindow = new SignalViewerWindow(model->getListReference());
        else
            signalViewerWindow = new SignalViewerWindow(model->getFilteredListReference());
    }
    signalViewerWindow->show();
}

void MainWindow::showConnectionSettingsWindow()
{
    if (!connectionWindow)
        connectionWindow = new ConnectionWindow();

    // Trigger a one-shot state probe on all SerialBus connections before showing the
    // window.  PCAN on macOS never emits stateChanged on USB unplug, so this is the
    // only time we can pay the cost of a brief disconnect+reconnect (user-visible action).
    for (CANConnection *conn : CANConManager::getInstance()->getConnections()) {
        if (conn->getType() == CANCon::SERIALBUS)
            QMetaObject::invokeMethod(conn, "probeConnectionState", Qt::QueuedConnection);
    }

    connectionWindow->show();
}
