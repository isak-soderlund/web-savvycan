#include "frameinfowindow.h"
#include "ui_frameinfowindow.h"
#include "mainwindow.h"
#include "helpwindow.h"
#include <QtDebug>
#include <vector>
#include "filterutility.h"
#include "qcpaxistickerhex.h"
#include "theme.h"
#include <QBuffer>
#include <QFileDialog>
#include <QScreen>
#include <QScrollArea>
#include <algorithm>
#include <cmath>

// SavvyCAN's eight byte graph colours, byte by byte (they repeat every 8 bytes: CAN FD has up to 64). web-savvycan:
// in dark, lighter ones, light grey for black (the plot background is dark).
QColor FrameInfoWindow::byteGraphColor(int byte)
{
    static const QColor light[8] = {
        Qt::blue, Qt::green,  Qt::black, Qt::red, //0 1 2 3
        Qt::gray, Qt::darkYellow, Qt::cyan,  Qt::darkMagenta, //4 5 6 7
    };
    static const QColor dark[8] = {
        QColor(0x6c, 0x9c, 0xff), QColor(0x40, 0xe0, 0x40), QColor(0xd0, 0xd0, 0xd0), QColor(0xff, 0x60, 0x60),
        QColor(0xa0, 0xa0, 0xa0), QColor(0xe0, 0xc8, 0x40), QColor(Qt::cyan), QColor(0xe0, 0x70, 0xe0),
    };
    return (Theme::isDark() ? dark : light)[byte & 7];
}

const int numIntervalHistBars = 20;

FrameInfoWindow::FrameInfoWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::FrameInfoWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
    // the .ui's size (the dialog opened at its smaller size hint, too narrow for a 64-byte heatmap), within the page
    if (QScreen *s = QGuiApplication::primaryScreen())
        resize(QSize(1142, 651).boundedTo(s->availableGeometry().size() - QSize(20, 40)));
#else
    setWindowFlags(Qt::Window);
#endif

    readSettings();

    modelFrames = frames;

    // Using lambda expression to strip away the possible filter label before passing the ID to updateDetailsWindow
    connect(ui->listFrameID, &QListWidget::currentTextChanged, 
        [this](QString itemText)
            {
            FrameInfoWindow::updateDetailsWindow(FilterUtility::getId(itemText));
            } );

    connect(MainWindow::getReference(), &MainWindow::framesUpdated, this, &FrameInfoWindow::updatedFrames);
    connect(ui->btnSave, &QAbstractButton::clicked, this, &FrameInfoWindow::saveDetails);

    ui->splitter->setStretchFactor(0, 1); //idx, stretch factor
    ui->splitter->setStretchFactor(1, 4); //goal is to make right hand side larger by default

    // web-savvycan: one graph per byte, 4 a row, in a scroll area: 8 for classic CAN (2 rows, as SavvyCAN), up to 64
    // (16 rows) for CAN FD; the graphs of bytes past the longest frame are hidden.
    QScrollArea *byteArea = new QScrollArea(this);
    byteArea->setWidgetResizable(true);
    byteArea->setFrameShape(QFrame::NoFrame);
    QWidget *byteWidget = new QWidget(byteArea);
    QGridLayout *byteGrid = new QGridLayout(byteWidget);
    byteGrid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < kMaxBytes; i++)
    {
        graphByte[i] = new QCustomPlot(byteWidget);
        graphByte[i]->setMinimumHeight(110);
        graphRef[i] = nullptr;
        setupByteGraph(graphByte[i], i);
        byteGrid->addWidget(graphByte[i], i / 4, i & 3);
        if (i >= shownByteGraphs) graphByte[i]->hide();
    }
    byteArea->setWidget(byteWidget);
    ui->gridLower->addWidget(byteArea, 0, 0);

    ui->gridUpper->addWidget(new QLabel("Heatmap"), 0, 0);
    heatmap = new CANDataGrid();
    heatmap->setMode(GridMode::HEAT_VIEW);
    ui->gridUpper->addWidget(heatmap, 1, 0);

    ui->gridUpper->addWidget(new QLabel("Bit Histogram"), 0, 1);
    graphHistogram = new QCustomPlot();
    ui->gridUpper->addWidget(graphHistogram, 1, 1);

    ui->gridUpper->setRowMinimumHeight(0, 20);
    ui->gridUpper->setRowStretch(0, 1);
    ui->gridUpper->setRowStretch(1, 10);

    graphHistogram->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectAxes |
                                    QCP::iSelectLegend);

    graphHistogram->xAxis->setRange(0, 63);
    graphHistogram->yAxis->setRange(0, 100);
    QSharedPointer<QCPAxisTickerLog> graphHistoLogTicker(new QCPAxisTickerLog);
    graphHistogram->yAxis->setTicker(graphHistoLogTicker);
    //graphHistogram->yAxis2->setTicker(graphHistoLogTicker);
    graphHistogram->yAxis->setNumberFormat("eb"); // e = exponential, b = beautiful decimal powers
    graphHistogram->yAxis->setNumberPrecision(0); //log ticker always picks powers of 10 so no need or use for precision

    //graphHistogram->axisRect()->setupFullAxesBox();
    graphHistogram->setBufferDevicePixelRatio(1);

    graphHistogram->xAxis->setLabel("Bits");
    graphHistogram->yAxis->setLabel("Instances");

    graphHistogram->legend->setVisible(false);

    ui->timeHistogram->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectAxes |
                                        QCP::iSelectLegend);

    ui->timeHistogram->xAxis->setRange(0, numIntervalHistBars);
    ui->timeHistogram->yAxis->setRange(0, 100);
    QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
    ui->timeHistogram->yAxis->setTicker(logTicker);
    //ui->timeHistogram->yAxis2->setTicker(logTicker);
    ui->timeHistogram->yAxis->setScaleType(QCPAxis::stLogarithmic);
    ui->timeHistogram->yAxis->setNumberFormat("eb"); // e = exponential, b = beautiful decimal powers
    ui->timeHistogram->yAxis->setNumberPrecision(0); //log ticker always picks powers of 10 so no need or use for precision
    //ui->timeHistogram->axisRect()->setupFullAxesBox();

    ui->timeHistogram->xAxis->setLabel("Interval (ms)");
    ui->timeHistogram->yAxis->setLabel("Occurrences");

    ui->timeHistogram->legend->setVisible(false);
    ui->timeHistogram->setBufferDevicePixelRatio(1);

    if (useOpenGL)
    {
        graphHistogram->setAntialiasedElements(QCP::aeAll);
        graphHistogram->setOpenGl(true);
        ui->timeHistogram->setAntialiasedElements(QCP::aeAll);
        ui->timeHistogram->setOpenGl(true);
    }
    else
    {
        graphHistogram->setOpenGl(false);
        graphHistogram->setAntialiasedElements(QCP::aeNone);
        ui->timeHistogram->setOpenGl(false);
        ui->timeHistogram->setAntialiasedElements(QCP::aeNone);
    }

    // Prevent annoying accidental horizontal scrolling when filter list is populated with long interpreted message names
    ui->listFrameID->horizontalScrollBar()->setEnabled(false);

    installEventFilter(this);

    connect(graphHistogram, SIGNAL(mousePress(QMouseEvent*)), this, SLOT(mousePress()));
    connect(graphHistogram, SIGNAL(mouseWheel(QWheelEvent*)), this, SLOT(mouseWheel()));
    connect(ui->timeHistogram, SIGNAL(mousePress(QMouseEvent*)), this, SLOT(mousePress()));
    connect(ui->timeHistogram, SIGNAL(mouseWheel(QWheelEvent*)), this, SLOT(mouseWheel()));

    dbcHandler = DBCHandler::getReference();

    connect(Theme::instance(), &Theme::changed, this, &FrameInfoWindow::themeChanged);
}

// web-savvycan: the byte graphs' colours follow the theme (Theme re-themes the plots' axes and backgrounds itself).
void FrameInfoWindow::themeChanged()
{
    for (int i = 0; i < kMaxBytes; i++)
    {
        if (!graphRef[i]) continue;
        graphRef[i]->setPen(QPen(byteGraphColor(i), 1));
        graphByte[i]->replot();
    }
    heatmap->update();
}

void FrameInfoWindow::setupByteGraph(QCustomPlot *plot, int num)
{
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectAxes |
                                    QCP::iSelectLegend);

    plot->xAxis->setRange(0, 63);
    plot->yAxis->setRange(0, 265);
    if (useHexTicker)
    {
        QSharedPointer<QCPAxisTickerHex> hexTicker(new QCPAxisTickerHex);
        plot->yAxis->setTicker(hexTicker);
    }
    //plot->axisRect()->setupFullAxesBox();

    plot->xAxis->setLabel("Time [" + QString::number(num) + "]");
    //if (useHexTicker) plot->yAxis->setLabel("Value (HEX)");
    //else plot->yAxis->setLabel("Value (Dec)");
    plot->yAxis->setLabel("");

    plot->legend->setVisible(false);
    plot->setBufferDevicePixelRatio(1);

    if (useOpenGL)
    {
        plot->setAntialiasedElements(QCP::aeAll);
        plot->setOpenGl(true);
    }
    else
    {
        plot->setOpenGl(false);
        plot->setAntialiasedElements(QCP::aeNone);
    }

    connect(plot, SIGNAL(mousePress(QMouseEvent*)), this, SLOT(mousePress()));
    connect(plot, SIGNAL(mouseWheel(QWheelEvent*)), this, SLOT(mouseWheel()));
    connect(plot, &QCustomPlot::mouseDoubleClick, this, &FrameInfoWindow::mouseDoubleClick);
}

void FrameInfoWindow::mousePress()
{
    QCustomPlot *plot = qobject_cast<QCustomPlot *>(sender());
    if (!plot) return;
    // if an axis is selected, only allow the direction of that axis to be dragged
    // if no axis is selected, both directions may be dragged

    if (plot->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
        plot->axisRect()->setRangeDrag(plot->xAxis->orientation());
    else if (plot->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
        plot->axisRect()->setRangeDrag(plot->yAxis->orientation());
    else
        plot->axisRect()->setRangeDrag(Qt::Horizontal|Qt::Vertical);
}

void FrameInfoWindow::mouseWheel()
{
    QCustomPlot *plot = qobject_cast<QCustomPlot *>(sender());
    if (!plot) return;

    // if an axis is selected, only allow the direction of that axis to be zoomed
    // if no axis is selected, both directions may be zoomed

    if (plot->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
        plot->axisRect()->setRangeZoom(plot->xAxis->orientation());
    else if (plot->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
        plot->axisRect()->setRangeZoom(plot->yAxis->orientation());
    else
        plot->axisRect()->setRangeZoom(Qt::Horizontal|Qt::Vertical);
}


//two modes here. If none of the shown sub graphs are hidden then hide all except the one the user
//just double clicked on. Otherwise unhide the hidden ones (web-savvycan: as many as the frame has bytes, up to 64)
void FrameInfoWindow::mouseDoubleClick()
{
    QCustomPlot *plot = qobject_cast<QCustomPlot *>(sender());
    bool hideMode = true;

    for (int i = 0; i < shownByteGraphs; i++)
    {
        if (graphByte[i]->isHidden()) hideMode = false;
    }

    for (int i = 0; i < shownByteGraphs; i++)
    {
        graphByte[i]->setHidden(hideMode && graphByte[i] != plot);
    }
}

void FrameInfoWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    readSettings();
    refreshIDList();
    selectFirstID();
}

// The first ID analysed and selected: once (selecting a row analyses it through currentTextChanged; SavvyCAN analysed
// it a second time before that, a second pass over a large capture).
void FrameInfoWindow::selectFirstID()
{
    if (ui->listFrameID->count() == 0) return;
    if (ui->listFrameID->currentRow() == 0)
        updateDetailsWindow(FilterUtility::getId(ui->listFrameID->item(0)));
    else
        ui->listFrameID->setCurrentRow(0);
}

bool FrameInfoWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("framedetails.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
}

FrameInfoWindow::~FrameInfoWindow()
{
    removeEventFilter(this);
    delete ui;
}

void FrameInfoWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    writeSettings();
    emit rejected();
}

void FrameInfoWindow::readSettings()
{
    QSettings settings;
 
    if (settings.value("Main/FilterLabeling", false).toBool())
    {
        ui->listFrameID->setMinimumWidth(250);
    }
    else
    {
        ui->listFrameID->setMinimumWidth(120);    
    }

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("FrameInfo/WindowSize", QSize(794, 694)).toSize());
        move(Utility::constrainedWindowPos(settings.value("FrameInfo/WindowPos", QPoint(50, 50)).toPoint()));
    }
    useOpenGL = settings.value("Main/UseOpenGL", false).toBool();
    useHexTicker = settings.value("InfoCompare/GraphHex", false).toBool();
}

void FrameInfoWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("FrameInfo/WindowSize", size());
        settings.setValue("FrameInfo/WindowPos", pos());
    }
}

//remember, negative numbers are special -1 = all frames deleted, -2 = totally new set of frames.
void FrameInfoWindow::updatedFrames(int numFrames)
{
    if (numFrames == -1) //all frames deleted. Kill the display
    {
        //qDebug() << "Delete all frames in Info Window";
        ui->listFrameID->clear();
        foundID.clear();
        clearDisplay();
        refreshIDList();
    }
    else if (numFrames == -2) //all new set of frames. Reset
    {
        //qDebug() << "All new set of frames in Info Window";
        ui->listFrameID->clear();
        foundID.clear();
        clearDisplay();
        refreshIDList();
        selectFirstID();
    }
    else if (numFrames > 0) //just got some new frames. See if they are relevant.
    {
        // web-savvycan: the trace is a ring (framestore.h): the newest numFrames frames are its tail (fewer when more
        // came in one tick than it keeps), and a row number from the start doesn't stay valid as old frames go, so
        // only the tail is read. New IDs join the list; the details of the selected ID stay as they were analysed
        // (as in SavvyCAN: select the ID again to analyse the newer frames).
        const int before = ui->listFrameID->count();
        const QVector<CommFrame> newest = modelFrames->tail(numFrames);
        for (const CommFrame &frame : newest) addFoundID(frame.frameId());
        if (ui->listFrameID->count() != before)
        {
            //default is to sort in ascending order
            ui->listFrameID->sortItems();
            ui->lblUniqueID->setText("(" + QString::number(ui->listFrameID->count()) + tr(" unique ids)"));
        }
    }
}

void FrameInfoWindow::addFoundID(uint32_t id)
{
    if (foundID.contains(id)) return;
    foundID.insert(id);
    FilterUtility::createFilterItem(id, ui->listFrameID);
}

// Nothing analysed: an empty tree, heatmap and graphs (after Clear, a new capture, an ID whose frames are gone).
void FrameInfoWindow::clearDisplay()
{
    ui->treeDetails->clear();
    lastAnalysis = Analysis();
    heatmap->setHeat(lastAnalysis.heat);
    graphHistogram->clearGraphs();
    graphHistogram->replot();
    for (int i = 0; i < kMaxBytes; i++)
    {
        graphByte[i]->clearGraphs();
        graphRef[i] = nullptr;
        if (graphByte[i]->isVisible()) graphByte[i]->replot();
    }
    ui->timeHistogram->clearGraphs();
    ui->timeHistogram->replot();
}

void FrameInfoWindow::updateDetailsWindow(QString newID)
{
    int targettedID;
    int minLen, maxLen, thisLen;
    int64_t avgInterval;
    int64_t minInterval;
    int64_t maxInterval;
    int64_t thisInterval;
    // web-savvycan: CAN FD: every per-byte and per-bit table holds 64 bytes / 512 bits (SavvyCAN's held 8 / 64 and
    // overflowed on frames longer than 8 bytes); the 256 x 64 value histogram is on the heap (wasm's stack is small).
    int minData[kMaxBytes];
    int maxData[kMaxBytes];
    std::vector<int> dataHistogram(256 * kMaxBytes, 0); // [value * kMaxBytes + byte]
    int bitfieldHistogram[kMaxBits];
    QVector<double> histGraphX, histGraphY;
    QVector<double> byteGraphX[kMaxBytes], byteGraphY[kMaxBytes];
    QVector<double> timeGraphX, timeGraphY;
    QHash<QString, QHash<QString, int>> signalInstances;
    double maxY = -1000.0;
    uint8_t changedBits[kMaxBytes];
    uint8_t referenceBits[kMaxBytes];
    bool referenceSet[kMaxBytes]; // a byte's reference is the first frame of the ID that has the byte

    //these two used by bitflip heatmap functionality
    uint8_t refByte[kMaxBytes];
    int bitFlips[kMaxBits];
    double bitFlipHeat[kMaxBits];

    QTreeWidgetItem *baseNode, *dataBase, *histBase, *tempItem;

    if (newID.isEmpty()) return; // the list was cleared: nothing selected
    if (modelFrames->count() == 0) return;

    targettedID = static_cast<int>(Utility::ParseStringToNum(newID));

    qDebug() << "Started update details window with id " << targettedID;

    avgInterval = 0;

    if (targettedID > -1)
    {
        // web-savvycan: the rows of this ID's frames, not a copy of the frames (a capture can hold a million of them);
        // read once more below. Nothing changes the trace in between (it is changed on this thread only).
        std::vector<qsizetype> rows;
        const qsizetype total = modelFrames->count();
        for (qsizetype i = 0; i < total; i++)
        {
            if (modelFrames->at(i).frameId() == static_cast<uint32_t>(targettedID)) rows.push_back(i);
        }

        if (rows.empty()) //nothing to do if there are no frames!
        {
            clearDisplay();
            return;
        }
        const int numFrames = static_cast<int>(rows.size());
        const CommFrame firstFrame = modelFrames->at(rows[0]);

        ui->treeDetails->clear();
        lastAnalysis = Analysis();

        baseNode = new QTreeWidgetItem();
        baseNode->setText(0, QString("ID: ") + newID );

        if (firstFrame.hasExtendedFrameFormat()) //if these frames seem to be extended then try for J1939 decoding
        {
            // ------- J1939 decoding ----------
            J1939ID jid;
            jid.src = targettedID & 0xFF;
            jid.priority = targettedID >> 26;
            jid.edp = (targettedID >> 25) & 1;
            jid.dp = (targettedID >> 24) & 1;
            jid.pgn = (targettedID >> 8) & 0x3FFFF; //18 bits. Includes EDP and DP
            jid.pf = (targettedID >> 16) & 0xFF;
            jid.ps = (targettedID >> 8) & 0xFF;

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("J1939 decoding"));
            baseNode->addChild(tempItem);

            if (jid.pf > 0xEF)
            {
                jid.isBroadcast = true;
                jid.dest = 0xFFFF;
                tempItem = new QTreeWidgetItem();
                tempItem->setText(0, tr("   Broadcast Frame"));
                baseNode->addChild(tempItem);
            }
            else
            {
                jid.dest = jid.ps;
                jid.pgn &= 0xFFFF00; //targetted messages use PGN with 00 in low nibbles
                tempItem = new QTreeWidgetItem();
                if (jid.dest < 0xFF)
                    tempItem->setText(0, tr("   Destination ID: ") + Utility::formatNumber(static_cast<uint64_t>(jid.dest)));
                else
                    tempItem->setText(0, tr("   Destination ID: Global (ALL)"));
                baseNode->addChild(tempItem);
            }

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   Priority: ") + QString::number(jid.priority) + " (0-7)") ;
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   SRC: ") + Utility::formatNumber(static_cast<uint64_t>(jid.src)));
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   EDP: ") + QString::number(jid.edp));
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   DP: ") + QString::number(jid.dp));
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   PGN: ") + Utility::formatNumber(static_cast<uint64_t>(jid.pgn)) + "(" + QString::number(jid.pgn) + ")");
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   PF: ") + Utility::formatNumber(static_cast<uint64_t>(jid.pf)));
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   PS: ") + Utility::formatNumber(static_cast<uint64_t>(jid.ps)));
            baseNode->addChild(tempItem);

            // ------- GMLAN 29bit decoding ----------
            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("GMLAN 29bit decoding"));
            baseNode->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   Priority bits: ") + Utility::formatNumber( (uint64_t)FilterUtility::getGMLanPriorityBits(targettedID)));
            baseNode->addChild(tempItem);
            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   Arbitration Id: ") + Utility::formatNumber( (uint64_t)FilterUtility::getGMLanArbitrationId(targettedID)));
            baseNode->addChild(tempItem);
            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("   Sender Id: ") + Utility::formatNumber( (uint64_t)FilterUtility::getGMLanSenderId(targettedID)));
            baseNode->addChild(tempItem);

        }

        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("# of frames: ") + QString::number(numFrames,10));
        baseNode->addChild(tempItem);

        //clear out all the counters and accumulators
        minLen = kMaxBytes; // web-savvycan: was 8 (an FD ID of 64-byte frames showed "8 to 64")
        maxLen = 0;
        minInterval = 0x7FFFFFFF;
        maxInterval = 0;
        for (int i = 0; i < kMaxBytes; i++)
        {
            minData[i] = 256;
            maxData[i] = -1;
            changedBits[i] = 0;
            referenceBits[i] = 0;
            refByte[i] = 0;
            referenceSet[i] = false;
        }
        for (int j = 0; j < kMaxBits; j++)
        {
            bitfieldHistogram[j] = 0;
            bitFlips[j] = 0;
            bitFlipHeat[j] = 0;
        }
        signalInstances.clear();

        std::vector<int64_t> sortedIntervals;
        sortedIntervals.reserve(rows.size());
        int64_t intervalSum = 0;
        int64_t prevStamp = 0;

        // web-savvycan: byte graphs of at most kMaxBytePoints points: with more frames than half of that, a point pair
        // per run of `bucket` frames, the run's lowest value at its first frame and its highest at its last.
        const int halfPoints = kMaxBytePoints / 2;
        const int bucket = numFrames > halfPoints ? (numFrames + halfPoints - 1) / halfPoints : 1;
        int bucketMin[kMaxBytes], bucketMax[kMaxBytes];
        int bucketFirst = 0, bucketBytes = 0;

        DBC_MESSAGE *msg = dbcHandler->findMessageForFilter(targettedID, nullptr);

        //then find all data points
        for (int j = 0; j < numFrames; j++)
        {
            const CommFrame thisFrame = modelFrames->at(rows[j]);
            const QByteArray payload = thisFrame.payload();
            const unsigned char *data = reinterpret_cast<const unsigned char *>(payload.constData());
            const int dataLen = qMin(static_cast<int>(payload.length()), static_cast<int>(kMaxBytes));
            // whole time in µs (frames stamped from the system clock keep the seconds in seconds())
            const int64_t stamp = thisFrame.timeStamp().seconds() * 1000000LL + thisFrame.timeStamp().microSeconds();

            if (j % bucket == 0)
            {
                bucketFirst = j;
                bucketBytes = 0;
            }
            for (int bytcnt = 0; bytcnt < dataLen; bytcnt++)
            {
                if (bytcnt >= bucketBytes) bucketMin[bytcnt] = bucketMax[bytcnt] = data[bytcnt];
                else
                {
                    bucketMin[bytcnt] = qMin(bucketMin[bytcnt], int(data[bytcnt]));
                    bucketMax[bytcnt] = qMax(bucketMax[bytcnt], int(data[bytcnt]));
                }
            }
            // bytes this frame doesn't have are left out of this run's points (a shorter frame of the same ID)
            bucketBytes = qMax(bucketBytes, dataLen);
            if (j % bucket == bucket - 1 || j == numFrames - 1)
            {
                for (int bytcnt = 0; bytcnt < bucketBytes; bytcnt++)
                {
                    byteGraphX[bytcnt].append(bucketFirst);
                    byteGraphY[bytcnt].append(bucketMin[bytcnt]);
                    if (j != bucketFirst)
                    {
                        byteGraphX[bytcnt].append(j);
                        byteGraphY[bytcnt].append(bucketMax[bytcnt]);
                    }
                }
            }

            if (j != 0)
            {
                //TODO - we try the interval whichever way doesn't go negative. But, we should probably sort the frame list before
                //starting so that the intervals are all correct.
                if (stamp > prevStamp)
                    thisInterval = stamp - prevStamp;
                else
                    thisInterval = prevStamp - stamp;

                sortedIntervals.push_back(thisInterval);
                intervalSum += thisInterval;
                if (thisInterval > maxInterval) maxInterval = thisInterval;
                if (thisInterval < minInterval) minInterval = thisInterval;
                avgInterval += thisInterval;
            }
            prevStamp = stamp;
            thisLen = dataLen;
            if (thisLen > maxLen) maxLen = thisLen;
            if (thisLen < minLen) minLen = thisLen;
            for (int c = 0; c < thisLen; c++)
            {
                unsigned char dat = data[c];
                if (!referenceSet[c])
                {
                    referenceBits[c] = dat;
                    refByte[c] = dat;
                    referenceSet[c] = true;
                }
                if (minData[c] > dat) minData[c] = dat;
                if (maxData[c] < dat) maxData[c] = dat;
                dataHistogram[dat * kMaxBytes + c]++; //add one to count for this
                for (int l = 0; l < 8; l++)
                {
                    int bit = dat & (1 << l);
                    if (bit == (1 << l))
                    {
                        bitfieldHistogram[c * 8 + l]++;
                    }
                }
                changedBits[c] |= referenceBits[c] ^ dat;

                if (refByte[c] != dat) //if this byte doesn't match the value it last had
                {
                    uint8_t newBits = refByte[c] ^ dat; //get changed bits since last ref
                    for (int l = 0; l < 8; l++)
                    {
                        if (newBits & (1 << l)) bitFlips[(c * 8) + l]++;
                    }
                    refByte[c] = dat; //set the reference to this current byte now that processing is done
                }
            }

            //Search every signal in the selected message and give output of the range the signal took and
            //how many messages contained each discrete value.
            if (msg)
            {
                int numSignals = msg->sigHandler->getCount();
                QList<DBC_SIGNAL *> sigs = msg->sigHandler->getSignalsAsList();
                for (int i = 0; i < numSignals; i++)
                {
                    DBC_SIGNAL *sig = sigs[i];
                    if (sig)
                    {
                        if (sig->isSignalInMessage(thisFrame))
                        {
                            QString sigVal;
                            if (sig->processAsText(thisFrame, sigVal, false))
                            {
                                signalInstances[sig->name][sigVal] = signalInstances[sig->name][sigVal] + 1;
                            }
                        }
                    }
                }
            }
        }
        if (maxLen == 0) minLen = 0; //only frames without data (remote frames)

        //Divide all the bit flip heat values by the number of frames to get a ratio
        for (int j = 0; j < kMaxBits; j++) bitFlipHeat[j] = bitFlips[j] / (double)numFrames;

        std::sort(sortedIntervals.begin(), sortedIntervals.end());
        int64_t intervalStdDiv = 0, intervalPctl5 = 0, intervalPctl95 = 0, intervalMean = 0, intervalVariance = 0;

        int maxTimeCounter = -1;
        if (sortedIntervals.size() > 0)
        {
            intervalMean = intervalSum / sortedIntervals.size();

            for(int l = 0; l < static_cast<int>(sortedIntervals.size()); l++) {
                intervalVariance += ((sortedIntervals[l] - intervalMean) * (sortedIntervals[l] - intervalMean));
            }

            intervalVariance /= sortedIntervals.size();
            intervalStdDiv = static_cast<int>(sqrt(intervalVariance));

            intervalPctl5 = sortedIntervals[static_cast<unsigned int>(floor(0.05 * sortedIntervals.size()))];
            intervalPctl95 = sortedIntervals[static_cast<unsigned int>(floor(0.95 * sortedIntervals.size()))];

            uint64_t step = static_cast<unsigned int>(ceil((maxInterval - minInterval) / numIntervalHistBars));
            unsigned int index = 0;
            int counter = 0;
            for(int l = 0; l <= numIntervalHistBars; l++) {
                int64_t currentMax = maxInterval - ((numIntervalHistBars - l) * step);	// avoid missing the biggest value due to rounding errors
                while(index < sortedIntervals.size()) {
                    if(sortedIntervals[index] <= currentMax) {
                        counter++;
                        index++;
                    }
                    else {
                        break;
                    }
                }
                timeGraphX.append(currentMax / 1000.0);
                timeGraphY.append(counter);
                if(counter > maxTimeCounter) maxTimeCounter = counter;
                counter = 0;
            }
        }
        else minInterval = 0; //one frame: no interval

        if (numFrames > 1)
            avgInterval = avgInterval / (numFrames - 1);
        else avgInterval = 0;

        //now that data processing is done, create all of our output

        tempItem = new QTreeWidgetItem();

        if (minLen < maxLen)
            tempItem->setText(0, tr("Data Length: ") + QString::number(minLen) + tr(" to ") + QString::number(maxLen));
        else
            tempItem->setText(0, tr("Data Length: ") + QString::number(minLen));

        baseNode->addChild(tempItem);

        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Average inter-frame interval: ") + QString::number(avgInterval / 1000.0) + "ms");
        baseNode->addChild(tempItem);
        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Minimum inter-frame interval: ") + QString::number(minInterval / 1000.0) + "ms");
        baseNode->addChild(tempItem);
        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Maximum inter-frame interval: ") + QString::number(maxInterval / 1000.0) + "ms");
        baseNode->addChild(tempItem);
        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Inter-frame interval variation: ") + QString::number((maxInterval - minInterval) / 1000.0) + "ms");
        baseNode->addChild(tempItem);
        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Interval standard deviation: ") + QString::number(intervalStdDiv / 1000.0) + "ms");
        baseNode->addChild(tempItem);
        tempItem = new QTreeWidgetItem();
        tempItem->setText(0, tr("Minimum range to fit 90% of inter-frame intervals: ") + QString::number((intervalPctl95 - intervalPctl5) / 1000.0) + "ms");
        baseNode->addChild(tempItem);

        //display accumulated data for all the bytes in the message
        for (int c = 0; c < maxLen; c++)
        {
            dataBase = new QTreeWidgetItem();
            histBase = new QTreeWidgetItem();

            dataBase->setText(0, tr("Data Byte ") + QString::number(c));
            baseNode->addChild(dataBase);

            tempItem = new QTreeWidgetItem();
            QString builder;
            builder = tr("Changed bits: 0x") + QString::number(changedBits[c], 16) + "  (" + Utility::formatByteAsBinary(changedBits[c]) + ")";
            tempItem->setText(0, builder);
            dataBase->addChild(tempItem);

            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, tr("Range: ") + Utility::formatNumber((unsigned int)minData[c]) + tr(" to ") + Utility::formatNumber((unsigned int)maxData[c]));
            dataBase->addChild(tempItem);
            histBase->setText(0, tr("Histogram"));
            dataBase->addChild(histBase);

            for (int d = 0; d < 256; d++)
            {
                if (dataHistogram[d * kMaxBytes + c] > 0)
                {
                    tempItem = new QTreeWidgetItem();
                    tempItem->setText(0, QString::number(d) + "/0x" + QString::number(d, 16) +" (" + Utility::formatByteAsBinary(static_cast<uint8_t>(d)) +") -> " + QString::number(dataHistogram[d * kMaxBytes + c]));
                    histBase->addChild(tempItem);
                }
            }
        }

        dataBase = new QTreeWidgetItem();
        dataBase->setText(0, tr("Bitfield Histogram"));
        for (int c = 0; c < 8 * maxLen; c++)
        {
            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, QString::number(c) + " (Byte " + QString::number(c / 8) + " Bit "
                            + QString::number(c % 8) + ") : " + QString::number(bitfieldHistogram[c]));

            dataBase->addChild(tempItem);
            histGraphX.append(c);
            histGraphY.append(bitfieldHistogram[c]);
            if (bitfieldHistogram[c] > maxY) maxY = bitfieldHistogram[c];
        }
        baseNode->addChild(dataBase);

        //heat map output
        dataBase = new QTreeWidgetItem();
        dataBase->setText(0, tr("Bitchange Heatmap"));
        for (int c = 0; c < 8 * maxLen; c++)
        {
            tempItem = new QTreeWidgetItem();
            tempItem->setText(0, QString::number(c) + " (Byte " + QString::number(c / 8) + " Bit "
                            + QString::number(c % 8) + ") : " + QString::number(bitFlipHeat[c] * 100.0, 'f', 2));

            dataBase->addChild(tempItem);
            // web-savvycan: SavvyCAN added the bitfield histogram's points a second time here (the bit histogram
            // drew every bit twice)
            uint8_t heat = bitFlipHeat[c] * 255;
            if ((heat < 1) && (bitFlipHeat[c] > 0.0001)) heat = 1; //make sure any little bit of heat causes at least some output
            //qDebug() << "Heat for bit " << c <<  " is " << heat;
            lastAnalysis.heat[c] = heat;
        }
        baseNode->addChild(dataBase);
        // web-savvycan: the grid as large as the longest frame (8 bytes: 8x8; 12/16: 16x8; 20-32: 16x16; 48/64: 32x16)
        heatmap->setBytesToDraw(qMax(8, maxLen));
        ui->gridUpper->setColumnStretch(0, maxLen > 16 ? 3 : 1); // a 32 or 16 bit wide grid: more room than the histogram
        ui->gridUpper->setColumnStretch(1, maxLen > 16 ? 2 : 1);
        heatmap->setHeat(lastAnalysis.heat);

        QHash<QString, QHash<QString, int>>::const_iterator it = signalInstances.constBegin();
        while (it != signalInstances.constEnd()) {
            dataBase = new QTreeWidgetItem();
            dataBase->setText(0, it.key());
            QHash<QString,int>::const_iterator itVal = signalInstances[it.key()].constBegin();
            while (itVal != signalInstances[it.key()].constEnd())
            {
                tempItem = new QTreeWidgetItem();
                tempItem->setText(0, itVal.key() + ": " + QString::number(itVal.value()));
                dataBase->addChild(tempItem);
                ++itVal;
            }
            baseNode->addChild(dataBase);
            ++it;
        }

        ui->treeDetails->insertTopLevelItem(0, baseNode);

        graphHistogram->clearGraphs();
        graphHistogram->addGraph();
        graphHistogram->graph()->setData(histGraphX, histGraphY);
        graphHistogram->graph()->setLineStyle(QCPGraph::lsStepLeft); //connect points with lines
        QBrush graphBrush;
        graphBrush.setColor(Qt::red);
        graphBrush.setStyle(Qt::SolidPattern);
        graphHistogram->graph()->setPen(Qt::NoPen);
        graphHistogram->graph()->setBrush(graphBrush);
        graphHistogram->xAxis->setRange(0, 8 * qMax(8, maxLen)); // web-savvycan: every bit of the frame (was 0..63)
        graphHistogram->yAxis->setRange(0.8, maxY * 1.2);
        graphHistogram->yAxis->setScaleType(QCPAxis::stLogarithmic);
        graphHistogram->axisRect()->setupFullAxesBox();
        Theme::applyToPlot(graphHistogram);
        graphHistogram->replot();

        // web-savvycan: a graph for every byte of the longest frame (SavvyCAN drew bytes 0-7 only); at least 8 as
        // SavvyCAN, the rest hidden
        shownByteGraphs = qMax(8, maxLen);
        for (int graphs = 0; graphs < kMaxBytes; graphs++)
        {
            graphByte[graphs]->clearGraphs();
            graphRef[graphs] = nullptr;
            if (graphs >= shownByteGraphs)
            {
                graphByte[graphs]->hide();
                continue;
            }
            graphRef[graphs] = graphByte[graphs]->addGraph();
            graphByte[graphs]->graph()->setData(byteGraphX[graphs], byteGraphY[graphs], true);
            graphByte[graphs]->graph()->setPen(QPen(byteGraphColor(graphs), 1));
            graphByte[graphs]->xAxis->setRange(0, numFrames);
            graphByte[graphs]->show();
            Theme::applyToPlot(graphByte[graphs]);
            graphByte[graphs]->replot();
            lastAnalysis.bytePoints = qMax(lastAnalysis.bytePoints, static_cast<int>(byteGraphX[graphs].size()));
        }

        ui->timeHistogram->clearGraphs();
        ui->timeHistogram->addGraph();
        ui->timeHistogram->graph()->setData(timeGraphX, timeGraphY);
        ui->timeHistogram->graph()->setLineStyle(QCPGraph::lsStepLeft); //connect points with lines
        //QBrush graphBrush;
        graphBrush.setColor(Qt::red);
        graphBrush.setStyle(Qt::SolidPattern);
        ui->timeHistogram->graph()->setPen(Qt::NoPen);
        ui->timeHistogram->graph()->setBrush(graphBrush);
        //ui->timeHistogram->yAxis->setRange(0, maxTimeCounter * 1.1);
        //ui->timeHistogram->xAxis->setRange(minInterval / 1000.0, maxInterval / 1000.0); //graph is in ms while intervals are in us
        ui->timeHistogram->axisRect()->setupFullAxesBox();
        ui->timeHistogram->rescaleAxes();
        Theme::applyToPlot(ui->timeHistogram);
        ui->timeHistogram->replot();

        lastAnalysis.valid = true;
        lastAnalysis.id = static_cast<uint32_t>(targettedID);
        lastAnalysis.frames = numFrames;
        lastAnalysis.minLen = minLen;
        lastAnalysis.maxLen = maxLen;
        lastAnalysis.byteGraphs = shownByteGraphs;
        std::copy(bitFlips, bitFlips + kMaxBits, lastAnalysis.flips);
        std::copy(bitfieldHistogram, bitfieldHistogram + kMaxBits, lastAnalysis.sets);
    }
    else
    {
    }

    QSettings settings;
    if (settings.value("InfoCompare/AutoExpand", false).toBool())
    {
        ui->treeDetails->expandAll();
    }
}

void FrameInfoWindow::refreshIDList()
{
    const qsizetype total = modelFrames->count();
    for (qsizetype i = 0; i < total; i++)
    {
        addFoundID(modelFrames->at(i).frameId());
    }
    //default is to sort in ascending order
    ui->listFrameID->sortItems();
    ui->lblUniqueID->setText("(" + QString::number(ui->listFrameID->count()) + tr(" unique ids)"));
}

//go through all IDs, recalculate the data, and then write it out
void FrameInfoWindow::writeAllDetails(QIODevice *out)
{
    for (int i = 0; i < ui->listFrameID->count(); i++)
    {
        updateDetailsWindow(FilterUtility::getId(ui->listFrameID->item(i)));
        dumpNode(ui->treeDetails->invisibleRootItem(), out, 0);
        out->write("\n\n");
    }
}

void FrameInfoWindow::saveDetails()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the text is made in memory and handed to the browser as a download (Qt's save
    // dialog would only see the page's in-memory file system).
    {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly | QIODevice::Text);
        writeAllDetails(&buffer);
        buffer.close();
        QFileDialog::saveFileContent(buffer.data(), QStringLiteral("FrameDetails.txt"));
        return;
    }
#endif
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("Text File (*.txt)")));

    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(settings.value("FrameInfo/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        settings.setValue("FrameInfo/LoadSaveDirectory", dialog.directory().path());
        filename = dialog.selectedFiles()[0];
        if (!filename.contains('.')) filename += ".txt";
        if (dialog.selectedNameFilter() == filters[0])
        {
            QFile *outFile = new QFile(filename);

            if (!outFile->open(QIODevice::WriteOnly | QIODevice::Text))
            {
                delete outFile;
                return;
            }

            writeAllDetails(outFile);

            outFile->close();
            delete outFile;
        }
    }
}

void FrameInfoWindow::dumpNode(QTreeWidgetItem* item, QIODevice *file, int indent)
{
    for (int i = 0; i < indent; i++) file->write("\t");
    file->write(item->text(0).toUtf8());
    file->write("\n");
    for( int i = 0; i < item->childCount(); ++i )
        dumpNode( item->child(i), file, indent + 1 );
}
