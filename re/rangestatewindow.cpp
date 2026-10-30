#include "rangestatewindow.h"
#include "ui_rangestatewindow.h"
#include "mainwindow.h"
#include "utility.h"
#include "helpwindow.h"
#include "filterutility.h"
#include "theme.h"

#include <QElapsedTimer>

/*
 * web-savvycan (stage 4): RE Tools > Range State, with CAN FD, on the ring-buffer trace.
 * - The search is SavvyCAN's (ReTools::rangeSearch, its processSignal() line for line) on the frames of each ID,
 *   gathered in one pass over the trace into compact rows (was a copy of the trace's CommFrames per ID: one full scan
 *   of the trace per ID, and a reserve() of the whole trace's size per ID); 64-bit arithmetic (1 << 32 and the
 *   values overflowed int).
 * - CAN FD: a candidate's start bit up to 511 (its packed id/start/length kept the start bit in 8 bits: start bits
 *   past 255 ran into the length).
 * - No crash on an ID whose frames have left the trace (it read frame 0 of an empty list).
 * - The page keeps running while it searches (the event loop every 100 ms, not after every signal size).
 * - Live: new IDs join the filter list from the trace's newest frames (the trace is a ring: rows from count - n
 *   were wrong once it dropped old frames).
 * - The graph line in the theme's line colour (was black); in the browser a framed dialog.
 */

RangeStateWindow::RangeStateWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::RangeStateWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: a framed dialog in the browser (a Qt::Window is frameless over the main window's menus)
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    modelFrames = frames;

    ui->graphSignal->xAxis->setRange(0, 8);
    ui->graphSignal->yAxis->setRange(-10, 265); //run range a bit outside possible number so they aren't plotted in a hard to see place
    ui->graphSignal->xAxis->setVisible(false);
    ui->graphSignal->yAxis->setVisible(false);

    //ui->graphSignal->axisRect()->setupFullAxesBox();

    ui->cbSignalMode->addItem(tr("Big Endian"));
    ui->cbSignalMode->addItem(tr("Little Endian"));
    ui->cbSignalMode->addItem(tr("Try Both"));

    ui->cbSignedMode->addItem(tr("Signed Value"));
    ui->cbSignedMode->addItem(tr("Unsigned Value"));
    ui->cbSignedMode->addItem(tr("Try Both"));

//lambda expressions used here because these are tiny functions that don't have any reason to be full named functions
//unfortunately the fact that valueChanged is overloaded makes the syntax here HORRIBLE. That sucks.
    connect(ui->spinMaxSigSize, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            [=](int newVal)
            {
                if (newVal < ui->spinMinSigSize->value()) ui->spinMinSigSize->setValue(newVal);
            });

    connect(ui->spinMinSigSize, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            [=](int newVal)
            {
                if (newVal > ui->spinMaxSigSize->value()) ui->spinMaxSigSize->setValue(newVal);
            });

    connect(ui->btnAllFilter, &QAbstractButton::clicked,
            [=]()
            {
                for (int i = 0; i < ui->listFilter->count(); i++)
                {
                    QListWidgetItem *item = ui->listFilter->item(i);
                    item->setCheckState(Qt::Checked);
                    idFilters[Utility::ParseStringToNum(item->text())] = true;
                }
            });

    connect(ui->btnNoneFilter, &QAbstractButton::clicked,
            [=]()
            {
                for (int i = 0; i < ui->listFilter->count(); i++)
                {
                    QListWidgetItem *item = ui->listFilter->item(i);
                    item->setCheckState(Qt::Unchecked);
                    idFilters[Utility::ParseStringToNum(item->text())] = false;
                }
            });

    connect(ui->listFilter, &QListWidget::itemChanged,
            [=](QListWidgetItem *item)
            {
                bool isChecked = false;
                int id = FilterUtility::getIdAsInt(item);
                if (item->checkState() == Qt::Checked) isChecked = true;
                idFilters[id] = isChecked;
            });

    connect(ui->btnRecalc, &QAbstractButton::clicked, this, &RangeStateWindow::recalcButton);
    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));
    connect(ui->listCandidates, &QListWidget::currentRowChanged, this, &RangeStateWindow::clickedSignalList);
}

RangeStateWindow::~RangeStateWindow()
{
    delete ui;
}

void RangeStateWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    readSettings();

    refreshFilterList();

    installEventFilter(this);
}

void RangeStateWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    removeEventFilter(this);
    writeSettings();
}

bool RangeStateWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("rangestate.md");
            break;
        }
        return true;
    } else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }
    return false;
}

void RangeStateWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("RangeStateView/WindowSize", QSize(765, 615)).toSize());
        move(Utility::constrainedWindowPos(settings.value("RangeStateView/WindowPos", QPoint(50, 50)).toPoint()));
    }
}

void RangeStateWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("RangeStateView/WindowSize", size());
        settings.setValue("RangeStateView/WindowPos", pos());
    }
}

QCustomPlot *RangeStateWindow::plot() const
{
    return ui->graphSignal;
}

void RangeStateWindow::addFilterId(quint32 id)
{
    if (idFilters.contains(int(id))) return;
    idFilters.insert(int(id), true);
    FilterUtility::createCheckableFilterItem(id, true, ui->listFilter);
}

void RangeStateWindow::updatedFrames(int numFrames)
{
    if (numFrames == -1) //all frames deleted (or a file loaded in their place)
    {
        ui->listFilter->clear();
        idFilters.clear();
        refreshFilterList();
    }
    else if (numFrames == -2) //all new set of frames. Reset
    {
        refreshFilterList();
    }
    else if (numFrames > 0) //just got some new frames. See if we need to update the filters list. Otherwise nothing to do - no recalc happens until the button is pressed
    {
        // web-savvycan: the newest numFrames frames are the trace's tail (fewer if the ring kept fewer)
        const int before = ui->listFilter->count();
        for (const CommFrame &f : modelFrames->tail(numFrames))
            if (ReTools::hasData(f)) addFilterId(f.frameId());
        if (ui->listFilter->count() != before) ui->listFilter->sortItems();
    }
}

void RangeStateWindow::refreshFilterList()
{
    idFilters.clear();
    ui->listFilter->clear();

    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += 4096)
        for (const CommFrame &f : modelFrames->mid(i, 4096))
            if (ReTools::hasData(f)) addFilterId(f.frameId());

    ui->listFilter->sortItems();
}

// One ID's frames from the trace, compact.
ReTools::IdFrames RangeStateWindow::framesOf(quint32 id) const
{
    ReTools::IdFrames frames;
    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += 4096)
        for (const CommFrame &f : modelFrames->mid(i, 4096))
            if (f.frameId() == id && ReTools::hasData(f)) frames.append(f);
    return frames;
}

void RangeStateWindow::recalcButton()
{
    if (running) return;
    running = true;
    ui->btnRecalc->setEnabled(false);
    QElapsedTimer clock;
    clock.start();

    ui->listCandidates->clear();
    foundSignals.clear();
    ui->graphSignal->clearGraphs();
    ui->graphSignal->replot();

    QProgressDialog progress(qApp->activeWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.setLabelText("Calculating");
    progress.setCancelButton(0);
    progress.setRange(0,0);
    progress.setMinimumDuration(0);
    progress.show();

    // The frames of every ID to search, in one pass over the trace, with no event loop in between (the ring keeps
    // its rows meanwhile). Then the search, letting the event loop run every 100 ms.
    QMap<quint32, ReTools::IdFrames> perId;
    for (auto it = idFilters.cbegin(); it != idFilters.cend(); ++it)
        if (it.value()) perId.insert(quint32(it.key()), ReTools::IdFrames());
    runFrames = 0;
    const qsizetype count = modelFrames->count();
    for (qsizetype i = 0; i < count; i += 4096)
    {
        for (const CommFrame &f : modelFrames->mid(i, 4096))
        {
            if (!ReTools::hasData(f)) continue;
            auto it = perId.find(f.frameId());
            if (it == perId.end()) continue;
            it.value().append(f);
            runFrames++;
        }
    }

    ReTools::RangeParams p;
    p.minBits = ui->spinMinSigSize->value();
    p.maxBits = ui->spinMaxSigSize->value();
    p.granularity = ui->spinGranularity->value();
    p.sigType = ui->cbSignalMode->currentIndex() + 1;
    p.signedType = ui->cbSignedMode->currentIndex() + 1;
    p.sensitivity = ui->slideSensitivity->value();

    QElapsedTimer sinceYield;
    sinceYield.start();
    const auto keepAlive = [&sinceYield]() {
        if (sinceYield.elapsed() < 100) return;
        qApp->processEvents();
        sinceYield.restart();
    };
    for (auto it = perId.begin(); it != perId.end(); ++it)
    {
        qDebug() << "Processing for ID: " << it.key();
        const size_t first = foundSignals.size();
        ReTools::rangeSearch(it.key(), it.value(), p, foundSignals, keepAlive);
        it.value().clear(); // done with this ID's frames
        for (size_t c = first; c < foundSignals.size(); c++)
        {
            const ReTools::RangeCandidate &r = foundSignals[c];
            ui->listCandidates->addItem("ID: " + QString::number(r.id, 16) + " startBit: " + QString::number(r.startBit)
                                        + "  len: " + QString::number(r.bits) + (r.isSigned ? " Signed" : " Unsigned")
                                        + (r.bigEndian ? " BigEndian" : " LittleEndian"));
        }
        keepAlive();
    }

    progress.cancel();
    runMs = clock.elapsed();
    runCount++;
    running = false;
    ui->btnRecalc->setEnabled(true);
    qDebug() << "Found " << foundSignals.size() << " signals total.";
}

//graphs the vector such that the X axis is just the index into the vector and Y is perfectly graphed within the window
void RangeStateWindow::createGraph(const QVector<double> &values)
{
    double minval = 1000000, maxval = -100000;

    const int numEntries = values.count();

    ui->graphSignal->clearGraphs();

    QVector<double> x(numEntries);

    for (int j = 0; j < numEntries; j++)
    {
        x[j] = j;
        if (values[j] < minval) minval = values[j];
        if (values[j] > maxval) maxval = values[j];
    }

    qDebug() << "YMin: " << minval << " YMax: " << maxval;

    double ymin, ymax;

    ymin = minval;

    if (ymin < 0) ymin *= 1.2;
    else ymin *= 0.8;

    ymax = maxval;
    if (ymax < 0) ymax *= 0.8;
    else ymax *= 1.2;

    if (fabs(ymin) < 0.01) ymin -= (ymax / 60.0);
    if (fabs(ymax) < 0.01) ymax -= (ymin / 60.0);

    qDebug() << "YFm: " << ymin << " YFM: " << ymax;

    ui->graphSignal->addGraph();
    ui->graphSignal->graph()->setName("Graph");
    ui->graphSignal->graph()->setData(x, values, true);
    ui->graphSignal->graph()->setLineStyle(QCPGraph::lsLine); //connect points with lines
    QPen graphPen;
    // web-savvycan: the theme's default line colour (Theme::applyToPlot swaps it on a theme switch); was black
    graphPen.setColor(Theme::isDark() ? QColor(0xe0, 0xe0, 0xe0) : QColor(Qt::black));
    graphPen.setWidth(1);
    ui->graphSignal->graph()->setPen(graphPen);
    ui->graphSignal->xAxis->setRange(0, numEntries);
    ui->graphSignal->yAxis->setRange(ymin, ymax);
    ui->graphSignal->replot();
}

void RangeStateWindow::clickedSignalList(int idx)
{
    if (idx < 0 || idx >= int(foundSignals.size())) return; //just in case...

    const ReTools::RangeCandidate &c = foundSignals[size_t(idx)];
    qDebug() << "I:" << c.id << " sb:" << c.startBit << " len:" << c.bits << " signed:" << c.isSigned << " big:" << c.bigEndian;

    const ReTools::IdFrames frames = framesOf(c.id);
    createGraph(ReTools::signalValues(frames, c.startBit, c.bits, c.bigEndian, c.isSigned));
}
