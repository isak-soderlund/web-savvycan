#include "flowviewwindow.h"
#include "ui_flowviewwindow.h"
#include "mainwindow.h"
#include "helpwindow.h"
#include "filterutility.h"
#include "qcpaxistickerhex.h"
#include "theme.h"
#include <QBuffer>

/*
 * web-savvycan (stage 4): RE Tools > Flow View with CAN FD, bounded, live on the ring-buffer trace.
 * - The selected ID's frames are kept compactly (ReTools::IdFrames: time, length, payload in rows of the ID's longest
 *   frame), at most kMaxFrames (the newest; was a QList<CommFrame> of every frame, without end on a busy bus).
 * - The graphs hold only the frames around the current one (the view is always the current frame +- the chart
 *   range, no drag or zoom), rebuilt when the view leaves them: was every frame of the ID in each of up to 64 graphs,
 *   twice (x/y vectors and the plot's own copy), and live frames all went to one x (x[k] was never appended).
 * - Live: new frames are found by the newest frame taken (ReTools::NewFrames), not by an index: the trace is a ring.
 * - CAN FD: trigger bits past byte 7 (the 64-bit chunks were indexed by byte * 8: bytes 8.. read past the frame);
 *   a frame longer than the ID's earlier ones gets its rows and graphs; a changed Ref cell sets the reference byte
 *   (it set the seek value).
 * - No crash with no frames (Stop, Live Mode, the timeline on an empty ID read frame 0 or -1).
 * - Theme: the byte colours per theme (light: SavvyCAN's), the "+" mark's border in the plot's text colour.
 * - In the browser a framed dialog; "Save image to file" downloads a PNG.
 */


FlowViewWindow::FlowViewWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::FlowViewWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the other windows: a Qt::Window is frameless and covers the main window's menus in the
    // browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    readSettings();

    modelFrames = frames;

    playbackTimer = new QTimer();

    currentPosition = 0;
    playbackActive = false;
    playbackForward = true;
    inhibitChangeCallback = false;

    memset(refBytes, 0, 64);
    memset(currBytes, 0, 64);
    memset(triggerValues, -1, sizeof(int) * 64);
    for (int i = 0; i < 64; i++) triggerBits[i] = 0;
    for (int i = 0; i < 64; i++) graphRef[i] = nullptr;

    //ui->graphView->setInteractions();

    ui->graphView->xAxis->setRange(0, 8);
    ui->graphView->yAxis->setRange(-10, 265); //run range a bit outside possible number so they aren't plotted in a hard to see place
    if (useHexTicker)
    {
        QSharedPointer<QCPAxisTickerHex> hexTicker(new QCPAxisTickerHex);
        ui->graphView->yAxis->setTicker(hexTicker);
    }
    ui->graphView->axisRect()->setupFullAxesBox();

    QCPItemText *textLabel = new QCPItemText(ui->graphView);
    //ui->graphView->addItem(textLabel);
    textLabel->setPositionAlignment(Qt::AlignTop|Qt::AlignHCenter);
    textLabel->position->setType(QCPItemPosition::ptAxisRectRatio);
    textLabel->position->setCoords(0.5, .5);
    textLabel->setText("+");
    textLabel->setFont(QFont(font().family(), 16)); // make font a bit larger
    textLabel->setPen(QPen(Theme::isDark() ? QColor(0xd0, 0xd0, 0xd0) : QColor(Qt::black))); // a border around the text
    centerMark = textLabel;

    ui->graphView->xAxis->setLabel("Time Axis");
    if (useHexTicker) ui->graphView->yAxis->setLabel("Value Axis (HEX)");
    else ui->graphView->yAxis->setLabel("Value Axis (dec)");

    QFont legendFont = font();
    legendFont.setPointSize(10);
    QFont legendSelectedFont = font();
    legendSelectedFont.setPointSize(12);
    legendSelectedFont.setBold(true);
    ui->graphView->legend->setFont(legendFont);
    ui->graphView->legend->setSelectedFont(legendSelectedFont);
    ui->graphView->legend->setSelectableParts(QCPLegend::spItems); // legend box shall not be selectable, only legend items
    //ui->graphView->xAxis->setAutoSubTicks(false);
    //ui->graphView->xAxis->setAutoTicks(false);
    QCPAxisTicker *xTicker = new QCPAxisTicker();
    xTicker->setTickCount(5);
    xTicker->setTickStepStrategy(QCPAxisTicker::tssReadability);
    ui->graphView->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(xTicker));
    //ui->graphView->xAxis->setAutoTickStep(false);
    //ui->graphView->xAxis->setAutoSubTicks(false);
    //ui->graphView->xAxis->setNumberFormat("gb");
    //ui->graphView->xAxis->setTickStep(5.0);
    //ui->graphView->xAxis->setSubTickCount(0);

    if (openGLMode)
    {
        ui->graphView->setAntialiasedElements(QCP::aeAll);
        ui->graphView->setOpenGl(true);
    }
    else
    {
        ui->graphView->setOpenGl(false);
        ui->graphView->setAntialiasedElements(QCP::aeNone);
    }

    ui->graphView->setBufferDevicePixelRatio(1);

    ui->tableBytes->setColumnWidth(0, 60);
    ui->tableBytes->setColumnWidth(1, 60);
    ui->tableBytes->setColumnWidth(2, 60);
    ui->tableBytes->setColumnWidth(3, 60);
    ui->tableBytes->setColumnWidth(4, 70);
    ui->tableBytes->setColumnWidth(5, 80);

    //make slider output constant updates while dragging the value around
    ui->timelineSlider->setTracking(true);

    connect(ui->btnBackOne, SIGNAL(clicked(bool)), this, SLOT(btnBackOneClick()));
    connect(ui->btnPause, SIGNAL(clicked(bool)), this, SLOT(btnPauseClick()));
    connect(ui->btnReverse, SIGNAL(clicked(bool)), this, SLOT(btnReverseClick()));
    connect(ui->btnStop, SIGNAL(clicked(bool)), this, SLOT(btnStopClick()));
    connect(ui->btnPlay, SIGNAL(clicked(bool)), this, SLOT(btnPlayClick()));
    connect(ui->btnForwardOne, SIGNAL(clicked(bool)), this, SLOT(btnFwdOneClick()));
    connect(ui->spinPlayback, SIGNAL(valueChanged(int)), this, SLOT(changePlaybackSpeed(int)));
    connect(ui->cbLoopPlayback, SIGNAL(clicked(bool)), this, SLOT(changeLooping(bool)));
    connect(playbackTimer, SIGNAL(timeout()), this, SLOT(timerTriggered()));
    connect(ui->graphView, SIGNAL(plottableDoubleClick(QCPAbstractPlottable*,QMouseEvent*)), this, SLOT(plottableDoubleClick(QCPAbstractPlottable*,QMouseEvent*)));
    connect(ui->tableBytes, SIGNAL(cellChanged(int,int)), this, SLOT(handleTableCellChange(int,int)));
    connect(ui->graphRangeSlider, &QSlider::valueChanged, this, &FlowViewWindow::graphRangeChanged);
    connect(ui->timelineSlider, &QSlider::sliderPressed, this, &FlowViewWindow::btnPauseClick);
    connect(ui->timelineSlider, &QSlider::valueChanged, this, &FlowViewWindow::gotoFrame);
    connect(ui->btnAllGraphs, SIGNAL(clicked(bool)), this, SLOT(selectAllGraphs()));
    connect(ui->btnNoGraphs, SIGNAL(clicked(bool)), this, SLOT(selectNoGraphs()));

    // Using lambda expression to strip away the possible filter label before passing the ID to updateDetailsWindow
    connect(ui->listFrameID, &QListWidget::currentTextChanged, 
        [this](QString itemText)
            {
            changeID(FilterUtility::getId(itemText));
            } );

    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));

    ui->graphView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->graphView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextMenuRequestGraph(QPoint)));
    ui->flowView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->flowView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextMenuRequestFlow(QPoint)));
    connect(ui->flowView, SIGNAL(gridClicked(int)), this, SLOT(gotCellClick(int)));

    // Prevent annoying accidental horizontal scrolling when filter list is populated with long interpreted message names
    ui->listFrameID->horizontalScrollBar()->setEnabled(false);

    playbackTimer->setInterval(ui->spinPlayback->value()); //set the timer to the default value of the control

    connect(ui->cbTimeGraph, &QCheckBox::toggled, this, [this]() { graphsDirty = true; updateGraphLocation(); });
    connect(Theme::instance(), &Theme::changed, this, &FlowViewWindow::themeChanged);
}

QCustomPlot *FlowViewWindow::plot() const
{
    return ui->graphView;
}

// The byte colours and the "+" border follow the theme (Theme re-themes axes and background itself).
void FlowViewWindow::themeChanged()
{
    if (centerMark) centerMark->setPen(QPen(Theme::isDark() ? QColor(0xd0, 0xd0, 0xd0) : QColor(Qt::black)));
    for (int i = 0; i < 64; i++)
        if (graphRef[i]) graphRef[i]->setPen(QPen(ReTools::byteColor(i), 1));
    inhibitChangeCallback = true;
    for (int i = 0; i < ui->tableBytes->rowCount(); i++)
        if (QTableWidgetItem *item = ui->tableBytes->item(i, TABLE_BYTE::GRAPH_COLOR))
            item->setBackground(QBrush(ReTools::byteColor(i)));
    inhibitChangeCallback = false;
    ui->graphView->replot();
}

void FlowViewWindow::selectId(quint32 id)
{
    for (int j = 0; j < ui->listFrameID->count(); j++)
    {
        if (FilterUtility::getIdAsInt(ui->listFrameID->item(j)) == id)
        {
            ui->listFrameID->setCurrentRow(j);
            return;
        }
    }
}

void FlowViewWindow::setTriggerValue(int byte, int value)
{
    if (byte < 0 || byte >= ui->tableBytes->rowCount()) return;
    ui->tableBytes->item(byte, TABLE_BYTE::TRIGGER)->setText(QString::number(value, 16)); // as typed: hex
}

void FlowViewWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    installEventFilter(this);

    readSettings();

    refreshIDList();
    if (ui->listFrameID->count() > 0)
    {
        changeID(FilterUtility::getId(ui->listFrameID->item(0)));
        ui->listFrameID->setCurrentRow(0);
    }
    updateFrameLabel();
    qDebug() << "FlowView show event was processed";
}

FlowViewWindow::~FlowViewWindow()
{
    removeEventFilter(this);

    delete ui;

    playbackTimer->stop();
    delete playbackTimer;
}

void FlowViewWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    writeSettings();
    emit rejected(); //can be picked up by main window if needed
}

void FlowViewWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("FlowView/WindowSize", QSize(1078, 621)).toSize());
        move(Utility::constrainedWindowPos(settings.value("FlowView/WindowPos", QPoint(50, 50)).toPoint()));
    }

    if (settings.value("FlowView/AutoRef", false).toBool())
    {
        ui->cbAutoRef->setChecked(true);
    }

    if (settings.value("FlowView/UseTimestamp", false).toBool())
    {
        ui->cbTimeGraph->setChecked(true);
    }

    if (settings.value("Main/FilterLabeling", false).toBool())
        ui->listFrameID->setMaximumWidth(250);
    else
        ui->listFrameID->setMaximumWidth(120);    

    secondsMode = settings.value("Main/TimeSeconds", false).toBool();
    openGLMode = settings.value("Main/UseOpenGL", false).toBool();
    useHexTicker = settings.value("FlowView/GraphHex", false).toBool();
}

void FlowViewWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("FlowView/WindowSize", size());
        settings.setValue("FlowView/WindowPos", pos());
    }
}

/*
 * Keyboard shortcuts to allow for quick work without needing to move around a mouse.
 * R = resume or pause playback
 * T = go back one frame
 * Y = go forward one frame
*/
bool FlowViewWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_R:
            if (playbackActive) btnPauseClick();
            else btnPlayClick();
            break;
        case Qt::Key_T:
            btnBackOneClick();
            break;
        case Qt::Key_Y:
            btnFwdOneClick();
            break;
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("flowview.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
}

void FlowViewWindow::handleTableCellChange(int row, int col)
{
    if (inhibitChangeCallback) return;
    switch (col)
    {
    case TABLE_BYTE::BKEY:
    case TABLE_BYTE::CURR:
        break;
    case TABLE_BYTE::REF:
        // web-savvycan: the reference byte (SavvyCAN set the seek value here too)
        refBytes[row] = static_cast<unsigned char>(ui->tableBytes->item(row, col)->text().toInt(nullptr, 16)); //always interpreted as hex
        ui->flowView->setReference(refBytes, true);
        break;
    case TABLE_BYTE::TRIGGER:
        triggerValues[row] = ui->tableBytes->item(row, col)->text().toInt(nullptr, 16); //always interpreted as hex
        break;
    case TABLE_BYTE::GRAPH_EN:
        if (ui->tableBytes->item(row, col)->checkState() == Qt::Checked)
        {
            if (!graphRef[row]) createGraph(row);
            updateGraphLocation();
        }
        else
        {
            if (graphRef[row]) ui->graphView->removeGraph(graphRef[row]);
            graphRef[row] = nullptr;
            qDebug() << "Removed graph #" << row;
        }
        //if (!graphRef[row]) return;
        //graphRef[row]->setVisible( (ui->tableBytes->item(row, col)->checkState()==Qt::Checked)?true:false );
        ui->graphView->replot();
        break;
    case TABLE_BYTE::GRAPH_COLOR:
        break;
    }
}

void FlowViewWindow::selectAllGraphs()
{
    int rows = ui->tableBytes->rowCount();
    for (int i = 0; i < rows; i++)
    {
        if (!graphRef[i])
        {
            if (i < ui->tableBytes->rowCount())
            {
                createGraph(i);

            }
        }
        graphRef[i]->setVisible(true);
        inhibitChangeCallback = true;
        ui->tableBytes->item(i, TABLE_BYTE::GRAPH_EN)->setCheckState(Qt::Checked);
        inhibitChangeCallback = false;
    }
    updateGraphLocation();
}

void FlowViewWindow::selectNoGraphs()
{
    for (int i = 0; i < 64; i++)
    {
        if (graphRef[i])
        {
            ui->graphView->removeGraph(graphRef[i]);
            graphRef[i] = nullptr;
            //graphRef[i]->setVisible(false);
            inhibitChangeCallback = true;
            if (i < ui->tableBytes->rowCount()) ui->tableBytes->item(i, TABLE_BYTE::GRAPH_EN)->setCheckState(Qt::Unchecked);
            inhibitChangeCallback = false;
        }
    }
    ui->graphView->replot();
}

void FlowViewWindow::setupByteTable(int bytes)
{
    inhibitChangeCallback = true;
    ui->tableBytes->setRowCount(bytes);
    for (int i = 0; i < bytes; i++)
    {
        QTableWidgetItem *ni = new QTableWidgetItem(QString::number(i));
        ni->setFlags(Qt::ItemIsEnabled); //none, allow nothing.
        ui->tableBytes->setItem(i, 0, ni);
        ni = new QTableWidgetItem(QString::number(0));
        ni->setFlags(Qt::ItemIsEnabled);
        ui->tableBytes->setItem(i, 1, ni);
        ni = new QTableWidgetItem(QString::number(0));
        ui->tableBytes->setItem(i, 2, ni);
        ni = new QTableWidgetItem(QString::number(-1));
        ui->tableBytes->setItem(i, 3, ni);
        ni = new QTableWidgetItem("");
        ni->setCheckState(Qt::Checked);
        ui->tableBytes->setItem(i, 4, ni);
        ni = new QTableWidgetItem("");
        ni->setBackground(QBrush(ReTools::byteColor(i)));
        ui->tableBytes->setItem(i, 5, ni);
    }
    inhibitChangeCallback = false;
}

void FlowViewWindow::gotCellClick(int bitPosition)
{
    if (bitPosition < 0 || bitPosition >= 512) return;
    int chunk = bitPosition / 64;
    int idx = bitPosition & 63;
    triggerBits[chunk] = triggerBits[chunk] ^ (1ull << idx);
    if (triggerBits[chunk] & (1ull << idx)) ui->flowView->setCellTextState(bitPosition, GridTextState::BOLD_BLUE);
    else ui->flowView->setCellTextState(bitPosition, GridTextState::NORMAL);
    qDebug() << "Bit Num: " << bitPosition << "   Hex of trigger bits: " << QString::number(triggerBits[chunk], 16);
}

void FlowViewWindow::graphRangeChanged(int range) {
    ui->rangeValue->setText(QString::number(range));
    updateGraphLocation();
}

void FlowViewWindow::plottableDoubleClick(QCPAbstractPlottable* plottable, QMouseEvent* event)
{
    int id = 0;
    //apply transforms to get the X axis value where we double clicked
    double coord = plottable->keyAxis()->pixelToCoord(event->position().x());
    if (haveCacheId) id = cacheIdValue;
    if (secondsMode) emit sendCenterTimeID(id, coord);
    else emit sendCenterTimeID(id, coord / 1000000.0);
}

void FlowViewWindow::gotCenterTimeID(uint32_t ID, double timestamp)
{
    int64_t t_stamp;

    t_stamp = timestamp * 1000000l;

    qDebug() << "timestamp: " << t_stamp;

    selectId(ID); //to be sure we're focused on the proper ID (selecting it loads its frames)

    int bestIdx = -1;
    for (int i = 0; i < frameCache.size(); i++)
    {
        if (frameCache.timeUs(i) > t_stamp)
        {
            bestIdx = i - 1;
            break;
        }
    }
    qDebug() << "Best index " << bestIdx;
    if (bestIdx > -1)
    {
        currentPosition = bestIdx;
        if (ui->cbAutoRef->isChecked())
        {
            memcpy(refBytes, currBytes, 64);
        }

        loadCurrentBytes();

        updateDataView();
    }
}

void FlowViewWindow::contextMenuRequestFlow(QPoint pos)
{
  QMenu *menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);

  menu->addAction(tr("Save image to file"), this, SLOT(saveFileFlow()));

  menu->popup(ui->flowView->mapToGlobal(pos));
}

void FlowViewWindow::contextMenuRequestGraph(QPoint pos)
{
  QMenu *menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);

  menu->addAction(tr("Save image to file"), this, SLOT(saveFileGraph()));

  menu->popup(ui->graphView->mapToGlobal(pos));
}

void FlowViewWindow::saveFileGraph()
{
#ifdef Q_OS_WASM
    // web-savvycan: in the browser the picture goes to the browser as a download (Qt's save dialog would only see
    // the page's in-memory file system)
    {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        ui->graphView->toPixmap(1024, 768).save(&buffer, "PNG");
        QFileDialog::saveFileContent(png, QStringLiteral("FlowViewGraph.png"));
        return;
    }
#endif
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("PDF Files (*.pdf)")));
    filters.append(QString(tr("PNG Files (*.png)")));
    filters.append(QString(tr("JPEG Files (*.jpg)")));

    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(settings.value("FlowView/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];

        if (dialog.selectedNameFilter() == filters[0])
        {
            if (!filename.contains('.')) filename += ".pdf";
            ui->graphView->savePdf(filename, 0, 0);
        }
        if (dialog.selectedNameFilter() == filters[1])
        {
            if (!filename.contains('.')) filename += ".png";
            ui->graphView->savePng(filename, 1024, 768);
        }
        if (dialog.selectedNameFilter() == filters[2])
        {
            if (!filename.contains('.')) filename += ".jpg";
            ui->graphView->saveJpg(filename, 1024, 768);
        }
        settings.setValue("FlowView/LoadSaveDirectory", dialog.directory().path());
    }
}

void FlowViewWindow::saveFileFlow()
{
#ifdef Q_OS_WASM
    {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        ui->flowView->grab().save(&buffer, "PNG");
        QFileDialog::saveFileContent(png, QStringLiteral("FlowViewGrid.png"));
        return;
    }
#endif
    QString filename;
    QFileDialog dialog(this);
    QSettings settings;

    QStringList filters;
    filters.append(QString(tr("PNG Files (*.png)")));
    filters.append(QString(tr("JPEG Files (*.jpg)")));

    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(filters);
    dialog.setViewMode(QFileDialog::Detail);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(settings.value("FlowView/LoadSaveDirectory", dialog.directory().path()).toString());

    if (dialog.exec() == QDialog::Accepted)
    {
        filename = dialog.selectedFiles()[0];

        if (dialog.selectedNameFilter() == filters[0])
        {
            if (!filename.contains('.')) filename += ".png";
            ui->flowView->saveImage(filename, 1024, 768);
        }
        if (dialog.selectedNameFilter() == filters[1])
        {
            if (!filename.contains('.')) filename += ".jpg";
            ui->flowView->saveImage(filename, 1024, 768);
        }
        settings.setValue("FlowView/LoadSaveDirectory", dialog.directory().path());
    }
}

void FlowViewWindow::updatedFrames(int numFrames)
{
    if (numFrames == -1) //all frames deleted (or a file loaded in their place). Kill the display
    {
        ui->listFrameID->clear();
        foundID.clear();
        frameCache.clear();
        haveCacheId = false;
        newFrames.reset();
        currentPosition = 0;
        refreshIDList();
        removeAllGraphs();
        setupByteTable(0);
        memset(refBytes, 0, 64);
        memset(currBytes, 0, 64);
        updateDataView();
        updateFrameLabel();
    }
    else if (numFrames == -2) //all new set of frames. Reset (the selected ID again, if it is still there)
    {
        const bool had = haveCacheId;
        const quint32 id = cacheIdValue;
        ui->listFrameID->clear();
        foundID.clear();
        frameCache.clear();
        haveCacheId = false;
        newFrames.reset();
        currentPosition = 0;
        refreshIDList();
        if (had) selectId(id);
        updateFrameLabel();
    }
    else if (numFrames > 0) //just got some new frames. See if they are relevant.
    {
        // web-savvycan: the trace is a ring: the new frames are those after the newest one taken, not rows
        // count - numFrames.. (wrong once the ring drops its oldest frames, and numFrames may be more than it keeps)
        const QVector<CommFrame> fresh = newFrames.take(modelFrames, numFrames);
        const qint64 droppedBefore = frameCache.dropped();
        const int before = ui->listFrameID->count();
        int added = 0;
        for (const CommFrame &f : fresh)
        {
            if (f.frameType() == CommFrame::ErrorFrame || f.frameType() == CommFrame::InvalidFrame) continue;
            const quint32 id = f.frameId();
            if (!foundID.contains(id))
            {
                foundID.insert(id);
                FilterUtility::createFilterItem(id, ui->listFrameID);
            }
            if (haveCacheId && id == cacheIdValue)
            {
                if (f.payload().size() > frameCache.maxLength() && f.payload().size() > ui->tableBytes->rowCount())
                {
                    // a longer frame of this ID (CAN FD): a row and a graph for each new byte
                    const int oldRows = ui->tableBytes->rowCount();
                    frameCache.append(f);
                    growByteTable(oldRows, frameCache.maxLength());
                }
                else frameCache.append(f);
                added++;
            }
        }
        if (ui->listFrameID->count() != before) ui->listFrameID->sortItems();
        followDrops(droppedBefore);
        if (added > 0)
        {
            if (ui->cbLiveMode->checkState() == Qt::Checked)
            {
                currentPosition = frameCache.size() - 1;
                loadCurrentBytes();
                memcpy(refBytes, currBytes, 64);
            }
            updateDataView();
            if (ui->cbSync->checkState() == Qt::Checked && currentPosition < frameCache.size())
                emit sendCenterTimeID(cacheIdValue, frameCache.timeUs(currentPosition) / 1000000.0);
        }
    }
    updateFrameLabel();
}

// Frames went from the front of the ring: the current frame keeps its place among the frames (or is the oldest
// left), the graphs are rebuilt.
void FlowViewWindow::followDrops(qint64 droppedBefore)
{
    const qint64 gone = frameCache.dropped() - droppedBefore;
    if (gone <= 0) return;
    currentPosition = int(qMax<qint64>(0, currentPosition - gone));
    graphsDirty = true;
}

// The current frame's bytes (zeros past its length).
void FlowViewWindow::loadCurrentBytes()
{
    memset(currBytes, 0, 64);
    if (currentPosition < 0 || currentPosition >= frameCache.size()) return;
    memcpy(currBytes, frameCache.data(currentPosition), size_t(frameCache.length(currentPosition)));
}

void FlowViewWindow::removeAllGraphs()
{
    ui->graphView->clearGraphs();
    ui->graphView->replot();
    for (int i = 0; i < 64; i++) graphRef[i] = nullptr;
    graphFirst = 0;
    graphLast = -1;
    graphsDirty = true;
}

// A graph for one byte, empty: rebuildGraphs() gives it the frames around the current one.
void FlowViewWindow::createGraph(int byteNum)
{
    if (byteNum < 0 || byteNum >= 64) return;
    graphRef[byteNum] = ui->graphView->addGraph();
    ui->graphView->graph()->setName(QString("Graph %1").arg(ui->graphView->graphCount()-1));
    ui->graphView->graph()->setLineStyle(QCPGraph::lsLine); //connect points with lines
    QPen graphPen;
    graphPen.setColor(ReTools::byteColor(byteNum));
    graphPen.setWidth(1);
    ui->graphView->graph()->setPen(graphPen);
    ui->graphView->axisRect()->setupFullAxesBox();
    graphsDirty = true;
}

// A frame's place on the x axis: its time (s or µs as the app's time setting) or its number since the ID was
// selected (stays the same as old frames leave the ring).
double FlowViewWindow::graphKey(int i) const
{
    if (ui->cbTimeGraph->isChecked())
        return secondsMode ? frameCache.timeUs(i) / 1000000.0 : double(frameCache.timeUs(i));
    return double(frameCache.dropped() + i);
}

void FlowViewWindow::rebuildGraphs(int from, int to)
{
    graphFirst = from;
    graphLast = to;
    graphDropped = frameCache.dropped();
    graphsDirty = false;
    const int n = to >= from ? to - from + 1 : 0;
    QVector<double> keys(n);
    for (int j = 0; j < n; j++) keys[j] = graphKey(from + j);
    for (int k = 0; k < 64; k++)
    {
        if (!graphRef[k]) continue;
        QVector<QCPGraphData> points(n);
        for (int j = 0; j < n; j++)
        {
            const int i = from + j;
            points[j].key = keys[j];
            points[j].value = k < frameCache.length(i) ? frameCache.data(i)[k] : 0;
        }
        graphRef[k]->data()->set(points, true);
    }
}

void FlowViewWindow::refreshIDList()
{
    // one pass in slices (the view makes a copy of each frame)
    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += 4096)
    {
        const QVector<CommFrame> chunk = modelFrames->mid(i, 4096);
        for (const CommFrame &f : chunk)
        {
            if (f.frameType() == CommFrame::ErrorFrame || f.frameType() == CommFrame::InvalidFrame) continue;
            const quint32 id = f.frameId();
            if (!foundID.contains(id))
            {
                foundID.insert(id);
                FilterUtility::createFilterItem(id, ui->listFrameID);
            }
        }
    }
    newFrames.markTaken(modelFrames);
    //default is to sort in ascending order
    ui->listFrameID->sortItems();
}

void FlowViewWindow::updateFrameLabel()
{
    if (frameCache.size() > 0)
        ui->lblNumFrames->setText(QString::number(currentPosition + 1) + tr(" of ") + QString::number(frameCache.size()));
    else ui->lblNumFrames->setText("0 of 0");
}

void FlowViewWindow::changeID(QString newID)
{
    qDebug() << "change id " << newID;
    if (newID.isEmpty()) return;
    //parse the ID and then load up the frame cache with just messages with that ID.
    uint32_t id = (uint32_t)Utility::ParseStringToNum(newID);
    frameCache.clear();
    haveCacheId = true;
    cacheIdValue = id;

    playbackTimer->stop();
    playbackActive = false;
    // one pass over the trace in slices; frames that come later are found by the newest frame read
    const qsizetype count = modelFrames->count();
    for (qsizetype x = 0; x < count; x += 4096)
    {
        const QVector<CommFrame> chunk = modelFrames->mid(x, 4096);
        for (const CommFrame &f : chunk)
            if (f.frameId() == id && f.frameType() != CommFrame::ErrorFrame && f.frameType() != CommFrame::InvalidFrame)
                frameCache.append(f);
    }
    newFrames.markTaken(modelFrames);
    const int maxBytes = frameCache.maxLength();
    ui->flowView->setBytesToDraw(maxBytes);
    setupByteTable(maxBytes);

    currentPosition = 0;
    removeAllGraphs();

    if (frameCache.size() == 0)
    {
        memset(currBytes, 0, 64);
        memset(refBytes, 0, 64);
        updateDataView();
        return;
    }

    //for (uint32_t c = 0; c < frameCache.at(0).len; c++)
    for (int c = 0; c < maxBytes; c++)
    {
        createGraph(c);
    }

    loadCurrentBytes();
    memcpy(refBytes, currBytes, 64);

    updateDataView();
}

// A longer frame than before (CAN FD): table rows, grid and graphs for bytes oldRows..newRows-1.
void FlowViewWindow::growByteTable(int oldRows, int newRows)
{
    if (newRows <= oldRows) return;
    inhibitChangeCallback = true;
    ui->tableBytes->setRowCount(newRows);
    for (int i = oldRows; i < newRows; i++)
    {
        QTableWidgetItem *ni = new QTableWidgetItem(QString::number(i));
        ni->setFlags(Qt::ItemIsEnabled);
        ui->tableBytes->setItem(i, 0, ni);
        ni = new QTableWidgetItem(QString::number(0));
        ni->setFlags(Qt::ItemIsEnabled);
        ui->tableBytes->setItem(i, 1, ni);
        ui->tableBytes->setItem(i, 2, new QTableWidgetItem(QString::number(0)));
        ui->tableBytes->setItem(i, 3, new QTableWidgetItem(QString::number(-1)));
        ni = new QTableWidgetItem("");
        ni->setCheckState(Qt::Checked);
        ui->tableBytes->setItem(i, 4, ni);
        ni = new QTableWidgetItem("");
        ni->setBackground(QBrush(ReTools::byteColor(i)));
        ui->tableBytes->setItem(i, 5, ni);
        createGraph(i);
    }
    inhibitChangeCallback = false;
    ui->flowView->setBytesToDraw(newRows);
}

void FlowViewWindow::btnBackOneClick()
{
    ui->cbLiveMode->setChecked(false);
    playbackTimer->stop(); //pushing this button halts automatic playback
    playbackActive = false;

    updatePosition(false);
    updateDataView();
}

void FlowViewWindow::btnPauseClick()
{
    ui->cbLiveMode->setChecked(false);
    playbackActive = false;
    playbackTimer->stop();
    updateDataView();
}

void FlowViewWindow::btnReverseClick()
{
    ui->cbLiveMode->setChecked(false);
    if (frameCache.isEmpty()) return;
    playbackActive = true;
    playbackForward = false;
    playbackTimer->start();
}

void FlowViewWindow::btnStopClick()
{
    ui->cbLiveMode->setChecked(false);
    playbackTimer->stop(); //pushing this button halts automatic playback
    playbackActive = false;
    currentPosition = 0;

    loadCurrentBytes();
    memcpy(refBytes, currBytes, 64);

    updateFrameLabel();
    updateDataView();
}

void FlowViewWindow::btnPlayClick()
{
    ui->cbLiveMode->setChecked(false);
    if (frameCache.isEmpty()) return;
    playbackActive = true;
    playbackForward = true;
    playbackTimer->start();
}

void FlowViewWindow::btnFwdOneClick()
{
    ui->cbLiveMode->setChecked(false);
    playbackTimer->stop();
    playbackActive = false;
    updatePosition(true);
    updateDataView();
}

void FlowViewWindow::changePlaybackSpeed(int newSpeed)
{
    playbackTimer->setInterval(newSpeed);
}

void FlowViewWindow::changeLooping(bool check)
{
    Q_UNUSED(check);
}

void FlowViewWindow::timerTriggered()
{
    if (!playbackActive || (ui->cbLiveMode->checkState() == Qt::Checked) || frameCache.isEmpty())
    {
        playbackTimer->stop();
        return;
    }
    if (playbackForward)
    {
        updatePosition(true);
    }
    else
    {
        updatePosition(false);
    }

    updateDataView();

    if (!ui->cbLoopPlayback->isChecked())
    {
        if (currentPosition == 0) playbackActive = false;
        if (currentPosition == (frameCache.size() - 1)) playbackActive = false;
    }
}

void FlowViewWindow::updateDataView()
{
    inhibitChangeCallback = true;
    int maxbyte = ui->tableBytes->rowCount();

    for (int i = 0; i < maxbyte; i++)
    {
        ui->tableBytes->item(i, 1)->setText(Utility::formatByteAsHex(currBytes[i]));
        ui->tableBytes->item(i, 2)->setText(Utility::formatByteAsHex(refBytes[i]));
    }

    ui->flowView->setReference(refBytes, false);
    ui->flowView->updateData(currBytes, true);

    // web-savvycan: never a negative timeline (no frames: 0..0), so it can't send position -1
    const QSignalBlocker blockSlider(ui->timelineSlider);
    ui->timelineSlider->setMaximum(qMax(0, frameCache.size() - 1));
    ui->timelineSlider->setValue(currentPosition);

    for (int i = 0; i < maxbyte; i++)
    {
        if (currBytes[i] == triggerValues[i])
        {
            playbackActive = false;
            playbackTimer->stop();
            break; //once any trigger is hit we can stop looking
        }
    }

    updateGraphLocation();

    updateFrameLabel();
    inhibitChangeCallback = false;
}

void FlowViewWindow::gotoFrame(int frame) {
    if (frameCache.isEmpty()) return;
    if (frame >= 0 && frame < frameCache.size()) currentPosition = frame;
    else currentPosition = 0;

    processNewPosition();

    updateDataView();
}

void FlowViewWindow::updatePosition(bool forward)
{
    if (frameCache.isEmpty()) return;

    if (forward)
    {
        if (currentPosition < (frameCache.size() - 1)) currentPosition++;
        else if (ui->cbLoopPlayback->isChecked()) currentPosition = 0;
    }
    else
    {
        if (currentPosition > 0) currentPosition--;
        else if (ui->cbLoopPlayback->isChecked()) currentPosition = frameCache.size() - 1;
    }

    processNewPosition();

    const QSignalBlocker blockSlider(ui->timelineSlider);
    ui->timelineSlider->setValue(currentPosition);
}

void FlowViewWindow::processNewPosition()
{
    if (currentPosition < 0 || currentPosition >= frameCache.size()) return;
    if (ui->cbAutoRef->isChecked())
    {
        memcpy(refBytes, currBytes, 64);
    }

    //figure out which bits changed since the previous frame and then AND that with the trigger bits. If any bits
    //get through that then they're changed and a trigger so we stop playback at this frame.
    //This is complicated by the fact that CAN-FD frames might have far more than 64 bits. It is necessary
    //to thus process them 64 bits at a time and just move chunk to chunk until done.
    //web-savvycan: chunk is the first byte of 8 (it was used as byte * 8: bytes 8.. read past the frame).
    const unsigned char *data = frameCache.data(currentPosition);
    const int len = frameCache.length(currentPosition);
    for (int chunk = 0; chunk < len; chunk += 8)
    {
        uint64_t changedBits = 0;
        const int maxVal = qMin(chunk + 8, len);
        for (int i = chunk; i < maxVal; i++)
        {
            const uint8_t cngByte = currBytes[i] ^ data[i];
            changedBits |= (uint64_t)cngByte << (8ull * (i & 7));
        }
        changedBits &= triggerBits[chunk / 8];
        if (changedBits)
        {
            playbackActive = false;
            playbackTimer->stop();
        }
    }
    loadCurrentBytes();

    if (ui->cbSync->checkState() == Qt::Checked) emit sendCenterTimeID(cacheIdValue, frameCache.timeUs(currentPosition) / 1000000.0);
}

void FlowViewWindow::updateGraphLocation()
{
    if (frameCache.size() == 0) return;
    const int range = ui->graphRangeSlider->value();
    int start = currentPosition - range;
    if (start < 0) start = 0;
    int end = currentPosition + range;
    if (end >= frameCache.size()) end = frameCache.size() - 1;
    // the graphs hold the frames around the current one: rebuilt when the view leaves them (or new frames should
    // show, or old ones left the ring)
    if (graphsDirty || graphDropped != frameCache.dropped() || start < graphFirst || end > graphLast)
        rebuildGraphs(qMax(0, currentPosition - 2 * range), qMin(frameCache.size() - 1, currentPosition + 2 * range));
    ui->graphView->xAxis->setRange(graphKey(start), graphKey(end));
    if (!ui->cbTimeGraph->isChecked()) ui->graphView->xAxis->setNumberFormat("gb");

    ui->graphView->replot();
}
