#include "temporalgraphwindow.h"
#include "ui_temporalgraphwindow.h"
#include "helpwindow.h"
#include "mainwindow.h"
#include "utility.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

/*
 * web-savvycan: the temporal graph (RE Tools > Temporal Graph), rebuilt (G1-G4, resolve-can
 * docs/research/savvycan-improve.md):
 * - G1/G2: SavvyCAN sized its colour map by the ID range / 30 x the duration x 4 (29-bit IDs or long captures:
 *   billions of cells, an int overflow in QCustomPlot's setSize and writes through its null array). Here the map
 *   is TemporalDensity: at most Columns time columns x one row per distinct ID (at most MaxIds).
 * - G3: no point per frame (a scatter of every frame, redrawn on every pan and zoom): one colour map of
 *   frame counts, its size bounded as above.
 * - G4: live. MainWindow's framesUpdated(n) adds the new frames' counts (O(new frames) per GUI tick); -1/-2 (list
 *   cleared, loaded or replaced) rebuild from the frame list, in slices of ~25 ms so the page stays responsive
 *   on a big capture. No debug output per frame.
 * The trace is a ring of the newest frames: the counts keep the whole session (the ring may have dropped the
 * oldest frames since), and new frames are found by the newest frame counted, not by an index into the list.
 */

namespace {

qint64 floorDiv(qint64 a, qint64 b) // b > 0
{
    qint64 q = a / b;
    if ((a % b) != 0 && a < 0) q--;
    return q;
}

bool sameFrame(const CommFrame &a, const CommFrame &b)
{
    return a.timeStamp().microSeconds() == b.timeStamp().microSeconds() && a.frameId() == b.frameId()
           && a.hasExtendedFrameFormat() == b.hasExtendedFrameFormat() && a.getBus() == b.getBus()
           && a.frameType() == b.frameType() && a.payload() == b.payload();
}

QString idLabel(quint64 key)
{
    const bool ext = (key >> 32) != 0;
    return "0x" + QString::number(quint32(key), 16).toUpper().rightJustified(ext ? 8 : 3, '0');
}

QString durationLabel(qint64 us)
{
    if (us < 1000) return QString::fromUtf8("%1 µs").arg(us);
    if (us < 1000000) return QString("%1 ms").arg(us / 1000.0, 0, 'g', 3);
    if (us < 60000000) return QString("%1 s").arg(us / 1000000.0, 0, 'g', 3);
    if (us < 3600000000LL) return QString("%1 min").arg(us / 60000000.0, 0, 'g', 3);
    return QString("%1 h").arg(us / 3600000000.0, 0, 'g', 3);
}

constexpr int BuildChunk = 4096;     // frames copied out of the list at a time
constexpr int BuildSliceMs = 25;     // then the event loop (and the browser) gets a turn

} // namespace

// ---------------------------------------------------------------- TemporalDensity

void TemporalDensity::clear()
{
    mFrames = mHiddenFrames = 0;
    mFirstUs = mLastUs = mOriginUs = 0;
    mBinUs = 1;
    mSlot.clear();
    mKeys.clear();
    mCounts.clear();
    mSorted.clear();
    mSortedSlot.clear();
    mSortedValid = false;
}

void TemporalDensity::add(qint64 timeUs, quint32 id, bool extended)
{
    const quint64 k = key(id, extended);
    int slot = -1;
    auto it = mSlot.constFind(k);
    if (it != mSlot.constEnd())
        slot = it.value();
    else if (int(mKeys.size()) < MaxIds)
    {
        slot = int(mKeys.size());
        mSlot.insert(k, slot);
        mKeys.push_back(k);
        mCounts.emplace_back(Columns, 0u);
        mSortedValid = false;
    }

    if (mFrames == 0)
    {
        mFirstUs = mLastUs = mOriginUs = timeUs;
        mBinUs = 1;
    }
    else
    {
        if (timeUs < mFirstUs) mFirstUs = timeUs;
        if (timeUs > mLastUs) mLastUs = timeUs;
        if (timeUs < mOriginUs || (timeUs - mOriginUs) / mBinUs >= Columns) fit(mFirstUs, mLastUs);
    }
    mFrames++;

    if (slot < 0)
    {
        mHiddenFrames++;
        return;
    }
    const qint64 col = qBound(qint64(0), (timeUs - mOriginUs) / mBinUs, qint64(Columns - 1));
    quint32 &c = mCounts[size_t(slot)][size_t(col)];
    if (c != std::numeric_limits<quint32>::max()) c++;
}

// Widen the columns (doubling, columns added in pairs) and move column 0 so that [lo, hi] fits. Every origin is a
// multiple of the column width, so an old column always falls into exactly one new one.
void TemporalDensity::fit(qint64 lo, qint64 hi)
{
    qint64 bin = mBinUs, origin = 0;
    for (;;)
    {
        origin = floorDiv(lo, bin) * bin;
        if ((hi - origin) / bin < Columns) break;
        if (bin >= (qint64(1) << 55)) break;   // ~1000 years a column: timestamps that far apart are not a capture
        bin *= 2;
    }
    if (bin == mBinUs && origin == mOriginUs) return;

    std::vector<quint32> merged(Columns);
    for (std::vector<quint32> &row : mCounts)
    {
        std::fill(merged.begin(), merged.end(), 0u);
        for (int i = 0; i < Columns; i++)
        {
            const quint32 n = row[size_t(i)];
            if (!n) continue;
            const qint64 start = mOriginUs + qint64(i) * mBinUs;
            const qint64 j = qBound(qint64(0), floorDiv(start - origin, bin), qint64(Columns - 1));
            quint32 &c = merged[size_t(j)];
            c = (std::numeric_limits<quint32>::max() - c < n) ? std::numeric_limits<quint32>::max() : c + n;
        }
        row.swap(merged);
    }
    mBinUs = bin;
    mOriginUs = origin;
}

int TemporalDensity::usedColumns() const
{
    if (mFrames == 0) return 0;
    return int(qBound(qint64(1), (mLastUs - mOriginUs) / mBinUs + 1, qint64(Columns)));
}

quint32 TemporalDensity::maxCount() const
{
    quint32 m = 0;
    for (const std::vector<quint32> &row : mCounts)
        for (quint32 c : row) m = qMax(m, c);
    return m;
}

std::size_t TemporalDensity::memoryBytes() const
{
    const std::size_t rows = mCounts.capacity();
    return rows * (sizeof(std::vector<quint32>) + Columns * sizeof(quint32))
           + mKeys.capacity() * sizeof(quint64) + mSorted.capacity() * sizeof(quint64)
           + mSortedSlot.capacity() * sizeof(int) + std::size_t(mSlot.capacity()) * (sizeof(quint64) + 2 * sizeof(int));
}

void TemporalDensity::sortKeys() const
{
    if (mSortedValid) return;
    mSortedSlot.resize(mKeys.size());
    std::iota(mSortedSlot.begin(), mSortedSlot.end(), 0);
    std::sort(mSortedSlot.begin(), mSortedSlot.end(), [this](int a, int b) { return mKeys[size_t(a)] < mKeys[size_t(b)]; });
    mSorted.resize(mKeys.size());
    for (size_t i = 0; i < mSortedSlot.size(); i++) mSorted[i] = mKeys[size_t(mSortedSlot[i])];
    mSortedValid = true;
}

const std::vector<quint64> &TemporalDensity::sortedKeys() const
{
    sortKeys();
    return mSorted;
}

const std::vector<quint32> &TemporalDensity::countsOfRank(int rank) const
{
    sortKeys();
    return mCounts[size_t(mSortedSlot[size_t(rank)])];
}

// ---------------------------------------------------------------- TemporalIdTicker

double TemporalIdTicker::getTickStep(const QCPRange &range)
{
    // whole rows only: 1, 2, 5, 10, 20, 50, ... rows per tick, about tickCount() ticks
    const double raw = range.size() / double(qMax(1, tickCount()));
    double step = 1;
    for (double mag = 1; step < raw; mag *= 10)
    {
        if (raw <= mag) { step = mag; break; }
        if (raw <= 2 * mag) { step = 2 * mag; break; }
        if (raw <= 5 * mag) { step = 5 * mag; break; }
        step = 10 * mag;
    }
    return step;
}

int TemporalIdTicker::getSubTickCount(double tickStep)
{
    Q_UNUSED(tickStep);
    return 0;
}

QString TemporalIdTicker::getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision)
{
    Q_UNUSED(locale);
    Q_UNUSED(formatChar);
    Q_UNUSED(precision);
    const std::vector<quint64> &keys = mDensity->sortedKeys();
    const double r = std::round(tick);
    if (std::abs(tick - r) > 0.01 || r < 0 || r >= double(keys.size())) return QString();
    return idLabel(keys[size_t(r)]);
}

// ---------------------------------------------------------------- TemporalGraphWindow

TemporalGraphWindow::TemporalGraphWindow(const FrameListView *frames, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::TemporalGraphWindow)
{
    ui->setupUi(this);
#ifdef Q_OS_WASM
    // web-savvycan: as the connection and graphing windows: a Qt::Window is frameless and covers the main window's
    // menus in the browser; as a dialog it has Qt's title bar and close button and can be moved aside.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
#else
    setWindowFlags(Qt::Window);
#endif

    readSettings();

    modelFrames = frames;
    QCustomPlot *p = ui->graphingView;

    p->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectAxes);

    // Time in the app's time style (File > Preferences), as the graphing window: µs, or seconds, or the clock.
    if (Utility::timeStyle == TS_SECONDS || Utility::timeStyle == TS_CLOCK) keyScale = 1e-6;
    p->xAxis->setNumberFormat("f");
    p->xAxis->setNumberPrecision(Utility::timeStyle == TS_SECONDS ? 3 : 0);
    if (Utility::timeStyle == TS_CLOCK)
    {
        QSharedPointer<QCPAxisTickerDateTime> clock(new QCPAxisTickerDateTime);
        clock->setDateTimeFormat("hh:mm:ss.zzz");
        p->xAxis->setTicker(clock);
        p->xAxis->setLabel("Time");
    }
    else
        p->xAxis->setLabel(Utility::timeStyle == TS_SECONDS ? QString("Time (s)") : QString::fromUtf8("Time (µs)"));

    p->yAxis->setLabel("ID");
    p->yAxis->setNumberFormat("g"); // no "b": with it QCustomPlot draws a label's "E" as a power of ten (0x7E8 as 0x7·10⁸)
    idTicker = QSharedPointer<TemporalIdTicker>(new TemporalIdTicker(&mDensity));
    idTicker->setTickCount(10);
    p->yAxis->setTicker(idTicker);
    p->axisRect()->setupFullAxesBox();
    p->xAxis->setRange(0, 1);
    p->yAxis->setRange(-0.5, 0.5);

    // A line above the plot: IDs, frames, the width of a column (the map's resolution).
    p->plotLayout()->insertRow(0);
    QFont small = font();
    if (small.pointSizeF() > 8) small.setPointSizeF(small.pointSizeF() - 1);
    summary = new QCPTextElement(p, QString(), small);
    p->plotLayout()->addElement(0, 0, summary);

    // One colour map of frame counts per (time column, ID): empty cells transparent, the count on a log scale.
    // One gradient for light and dark (its darkest blue still reads on the dark plot background).
    colorMap = new QCPColorMap(p->xAxis, p->yAxis);
    colorMap->setName("Frames");
    colorMap->setInterpolate(false);
    colorMap->setTightBoundary(false);
    colorMap->setDataScaleType(QCPAxis::stLogarithmic);
    QCPColorGradient gradient;
    gradient.setColorStopAt(0.0, QColor(40, 100, 235));
    gradient.setColorStopAt(0.3, QColor(0, 175, 215));
    gradient.setColorStopAt(0.55, QColor(60, 190, 70));
    gradient.setColorStopAt(0.78, QColor(240, 200, 0));
    gradient.setColorStopAt(1.0, QColor(230, 45, 35));
    gradient.setNanHandling(QCPColorGradient::nhTransparent);
    colorMap->setGradient(gradient);
    colorMap->setDataRange(QCPRange(1, 2));

    // connect slot that ties some axis selections together (especially opposite axes):
    connect(p, SIGNAL(selectionChangedByUser()), this, SLOT(selectionChanged()));
    connect(p, SIGNAL(mousePress(QMouseEvent*)), this, SLOT(mousePress()));
    connect(p, SIGNAL(mouseWheel(QWheelEvent*)), this, SLOT(mouseWheel()));
    connect(MainWindow::getReference(), SIGNAL(framesUpdated(int)), this, SLOT(updatedFrames(int)));
    // make bottom and left axes transfer their ranges to top and right axes:
    connect(p->xAxis, SIGNAL(rangeChanged(QCPRange)), p->xAxis2, SLOT(setRange(QCPRange)));
    connect(p->yAxis, SIGNAL(rangeChanged(QCPRange)), p->yAxis2, SLOT(setRange(QCPRange)));
    // the user zoomed or panned: stop fitting the view to the whole capture (R puts it back)
    connect(p->xAxis, SIGNAL(rangeChanged(QCPRange)), this, SLOT(xRangeChanged(QCPRange)));
    connect(p->yAxis, SIGNAL(rangeChanged(QCPRange)), this, SLOT(xRangeChanged(QCPRange)));

    // about one ID label per 16 pixels of height and one time label per 160 pixels of width (µs labels are long)
    connect(p, &QCustomPlot::beforeReplot, this, [this]() {
        QCustomPlot *plot = ui->graphingView;
        idTicker->setTickCount(qBound(2, qMax(plot->axisRect()->height(), plot->height() - 100) / 16, 40));
        plot->xAxis->ticker()->setTickCount(qBound(2, qMax(plot->axisRect()->width(), plot->width() - 150) / 160, 10));
    });

    buildTimer.setSingleShot(true);
    buildTimer.setInterval(1); // a real timer: the browser gets a turn between slices
    connect(&buildTimer, &QTimer::timeout, this, &TemporalGraphWindow::buildStep);

    if (useOpenGL)
    {
        p->setAntialiasedElements(QCP::aeAll);
        //ui->graphingView->setNoAntialiasingOnDrag(true);
        p->setOpenGl(true);
    }
    else
    {
        p->setOpenGl(false);
        p->setAntialiasedElements(QCP::aeNone);
    }

    generateGraph();
}

TemporalGraphWindow::~TemporalGraphWindow()
{
    delete ui;
}

QCustomPlot *TemporalGraphWindow::plot() const
{
    return ui->graphingView;
}

std::size_t TemporalGraphWindow::mapBytes() const
{
    const std::size_t cells = std::size_t(colorMap->data()->keySize()) * std::size_t(colorMap->data()->valueSize());
    return cells * (sizeof(double) + sizeof(QRgb)); // the cells and the map's image (more if oversampled, < 100 cells)
}

void TemporalGraphWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    installEventFilter(this);
    readSettings();
    updatePlot(); // the counts went on while the window was closed
}

void TemporalGraphWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    removeEventFilter(this);
    writeSettings();
}

void TemporalGraphWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("Temporal/WindowSize", QSize(800, 600)).toSize());
        move(Utility::constrainedWindowPos(settings.value("Temporal/WindowPos", QPoint(50, 50)).toPoint()));
    }
    useOpenGL = settings.value("Main/UseOpenGL", false).toBool();
}

void TemporalGraphWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("Temporal/WindowSize", size());
        settings.setValue("Temporal/WindowPos", pos());
    }
}

void TemporalGraphWindow::countFrame(const CommFrame &frame)
{
    const CommFrame::FrameType type = frame.frameType();
    if (type == CommFrame::ErrorFrame || type == CommFrame::InvalidFrame) return; // no ID on the bus
    mDensity.add(frame.timeStamp().microSeconds(), frame.frameId(), frame.hasExtendedFrameFormat());
}

void TemporalGraphWindow::updatedFrames(int numFrames)
{
    if (numFrames == -1 || numFrames == -2)
    {
        // -1: cleared, or a file loaded in its place; -2: an all new set. Count the list as it is now.
        generateGraph();
        return;
    }
    if (numFrames <= 0 || mBuilding) return; // a build reads up to the end of the list anyway

    // The n new frames are the newest rows of the list (fewer if the ring dropped some). Rows already counted
    // (a filtered list's tail, or frames a build has read) are told apart by the newest frame counted.
    const qsizetype count = modelFrames->count();
    if (count == 0) return;
    qsizetype start = qMax<qsizetype>(0, count - numFrames);
    if (haveLast)
    {
        const qsizetype before = count - numFrames - 1;
        if (before >= 0 && sameFrame(modelFrames->at(before), lastFrame))
            start = before + 1;
        else
        {
            for (qsizetype i = count - 1; i >= qMax<qsizetype>(0, before); i--)
            {
                if (sameFrame(modelFrames->at(i), lastFrame))
                {
                    start = i + 1;
                    break;
                }
            }
        }
    }
    if (start >= count) return;
    const QVector<CommFrame> fresh = modelFrames->mid(start, count - start);
    for (const CommFrame &f : fresh) countFrame(f);
    if (!fresh.isEmpty())
    {
        lastFrame = fresh.last();
        haveLast = true;
    }
    updatePlot();
}

// Count every frame of the list again, in slices (buildStep).
void TemporalGraphWindow::generateGraph()
{
    buildTimer.stop();
    mDensity.clear();
    haveLast = false;
    buildCursor = 0;
    mBuilding = true;
    autoFit = true;
    buildReplotClock.start();
    buildStep();
}

void TemporalGraphWindow::buildStep()
{
    QElapsedTimer slice;
    slice.start();
    while (mBuilding)
    {
        const qsizetype count = modelFrames->count();
        // The ring may have dropped its oldest chunk since the last slice: find where the list moved to.
        if (haveLast && buildCursor > 0 && !sameFrame(modelFrames->at(buildCursor - 1), lastFrame))
        {
            qsizetype i = qMin(buildCursor - 1, count - 1);
            const qsizetype stop = qMax<qsizetype>(0, i - 2 * FrameStore::DefaultChunkFrames);
            for (; i >= stop; i--)
                if (sameFrame(modelFrames->at(i), lastFrame)) break;
            buildCursor = i >= stop ? i + 1 : qMin(buildCursor, count);
        }
        if (buildCursor >= count)
        {
            mBuilding = false;
            break;
        }
        const QVector<CommFrame> chunk = modelFrames->mid(buildCursor, BuildChunk);
        if (chunk.isEmpty())
        {
            mBuilding = false;
            break;
        }
        for (const CommFrame &f : chunk) countFrame(f);
        buildCursor += chunk.size();
        lastFrame = chunk.last();
        haveLast = true;
        if (slice.elapsed() >= BuildSliceMs) break;
    }

    if (mBuilding)
    {
        if (buildReplotClock.elapsed() >= 500) // show the progress now and then
        {
            updatePlot();
            buildReplotClock.restart();
        }
        buildTimer.start();
    }
    else
        updatePlot();
}

void TemporalGraphWindow::updatePlot()
{
    if (!isVisible())
    {
        plotDirty = true;
        return;
    }
    plotDirty = false;
    QCustomPlot *p = ui->graphingView;
    QCPColorMapData *d = colorMap->data();
    const int rows = mDensity.ids();

    QString text;
    if (mDensity.isEmpty() || rows == 0)
    {
        d->setSize(0, 0);
        text = mBuilding ? tr("Reading frames...") : tr("No frames");
    }
    else
    {
        // Cells are centred on the map's key/value range; at least 2 x 2 (QCustomPlot draws a 1-cell side as
        // nothing), the spare row or column empty.
        const int cols = qMax(2, mDensity.usedColumns());
        const int valueRows = qMax(2, rows);
        d->setSize(cols, valueRows);
        const double bin = double(mDensity.binUs());
        const double origin = double(mDensity.originUs());
        d->setRange(QCPRange((origin + bin / 2) * keyScale, (origin + (cols - 1) * bin + bin / 2) * keyScale),
                    QCPRange(0, valueRows - 1));
        const double nan = std::numeric_limits<double>::quiet_NaN();
        quint32 maxCount = 1;
        for (int r = 0; r < valueRows; r++)
        {
            if (r >= rows)
            {
                for (int c = 0; c < cols; c++) d->setCell(c, r, nan);
                continue;
            }
            const std::vector<quint32> &counts = mDensity.countsOfRank(r);
            for (int c = 0; c < cols; c++)
            {
                const quint32 n = counts[size_t(c)];
                d->setCell(c, r, n ? double(n) : nan);
                if (n > maxCount) maxCount = n;
            }
        }
        colorMap->setDataRange(QCPRange(1, qMax(2.0, double(maxCount))));

        text = tr("%1 IDs, %2 frames over %3; a column is %4; colour: frames per cell (1 to %5, log)")
                   .arg(rows).arg(mDensity.frames())
                   .arg(durationLabel(mDensity.lastUs() - mDensity.firstUs()))
                   .arg(durationLabel(mDensity.binUs())).arg(maxCount);
        if (mDensity.hiddenFrames())
            text += tr("; %1 frames of IDs past the first %2 not shown").arg(mDensity.hiddenFrames()).arg(TemporalDensity::MaxIds);
        if (mBuilding)
            text += tr(" (reading frames: %1 of %2)").arg(buildCursor).arg(modelFrames->count());
    }
    summary->setText(text);

    if (autoFit) fitView();
    p->replot(QCustomPlot::rpQueuedReplot);
}

// The whole capture: from the first frame's column to the end of the last frame's, every ID row.
void TemporalGraphWindow::fitView()
{
    QCustomPlot *p = ui->graphingView;
    settingRange = true;
    if (!mDensity.isEmpty())
    {
        const qint64 bin = mDensity.binUs(), origin = mDensity.originUs();
        const qint64 lower = origin + floorDiv(mDensity.firstUs() - origin, bin) * bin;
        const qint64 upper = origin + floorDiv(mDensity.lastUs() - origin, bin) * bin + bin;
        p->xAxis->setRange(double(lower) * keyScale, double(upper) * keyScale);
        p->yAxis->setRange(-0.5, qMax(1, mDensity.ids()) - 0.5);
    }
    settingRange = false;
}

void TemporalGraphWindow::xRangeChanged(const QCPRange &range)
{
    Q_UNUSED(range);
    if (!settingRange) autoFit = false;
}

void TemporalGraphWindow::selectionChanged()
{
  /*
   normally, axis base line, axis tick labels and axis labels are selectable separately, but we want
   the user only to be able to select the axis as a whole, so we tie the selected states of the tick labels
   and the axis base line together. However, the axis label shall be selectable individually.

   The selection state of the left and right axes shall be synchronized as well as the state of the
   bottom and top axes.
  */

  // make top and bottom axes be selected synchronously, and handle axis and tick labels as one selectable object:
  if (ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spAxis) || ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spTickLabels) ||
      ui->graphingView->xAxis2->selectedParts().testFlag(QCPAxis::spAxis) || ui->graphingView->xAxis2->selectedParts().testFlag(QCPAxis::spTickLabels))
  {
    ui->graphingView->xAxis2->setSelectedParts(QCPAxis::spAxis|QCPAxis::spTickLabels);
    ui->graphingView->xAxis->setSelectedParts(QCPAxis::spAxis|QCPAxis::spTickLabels);
  }
  // make left and right axes be selected synchronously, and handle axis and tick labels as one selectable object:
  if (ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spAxis) || ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spTickLabels) ||
      ui->graphingView->yAxis2->selectedParts().testFlag(QCPAxis::spAxis) || ui->graphingView->yAxis2->selectedParts().testFlag(QCPAxis::spTickLabels))
  {
    ui->graphingView->yAxis2->setSelectedParts(QCPAxis::spAxis|QCPAxis::spTickLabels);
    ui->graphingView->yAxis->setSelectedParts(QCPAxis::spAxis|QCPAxis::spTickLabels);
  }
}

void TemporalGraphWindow::mousePress()
{
  // if an axis is selected, only allow the direction of that axis to be dragged
  // if no axis is selected, both directions may be dragged

  if (ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
  {
    ui->graphingView->axisRect()->setRangeDrag(ui->graphingView->xAxis->orientation());
  }
  else if (ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
  {
    ui->graphingView->axisRect()->setRangeDrag(ui->graphingView->yAxis->orientation());
  }
  else
  {
    ui->graphingView->axisRect()->setRangeDrag(Qt::Horizontal|Qt::Vertical);
  }
}

void TemporalGraphWindow::mouseWheel()
{
  // if an axis is selected, only allow the direction of that axis to be zoomed
  // if no axis is selected, both directions may be zoomed

  if (ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
    ui->graphingView->axisRect()->setRangeZoom(ui->graphingView->xAxis->orientation());
  else if (ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
    ui->graphingView->axisRect()->setRangeZoom(ui->graphingView->yAxis->orientation());
  else
    ui->graphingView->axisRect()->setRangeZoom(Qt::Horizontal|Qt::Vertical);
}

bool TemporalGraphWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key())
        {
        case Qt::Key_Plus:
            zoomIn();
            break;
        case Qt::Key_Minus:
            zoomOut();
            break;
        case Qt::Key_R:
            resetView();
            break;
        case Qt::Key_F1:
            HelpWindow::getRef()->showHelp("temporalwindow.md");
            break;
        }
        return true;
    }  else {
        // standard event processing
        return QObject::eventFilter(obj, event);
    }

    return false;
}

// R: the whole capture again, and follow it as it grows.
void TemporalGraphWindow::resetView()
{
    autoFit = true;
    fitView();
    ui->graphingView->replot();
}

void TemporalGraphWindow::zoomIn()
{
    QCPRange xrange = ui->graphingView->xAxis->range();
    QCPRange yrange = ui->graphingView->yAxis->range();
    if (ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
    {
        ui->graphingView->xAxis->scaleRange(0.666, xrange.center());
    }

    else if (ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
    {
        ui->graphingView->yAxis->scaleRange(0.666, yrange.center());
    }
    else
    {
        ui->graphingView->xAxis->scaleRange(0.666, xrange.center());
        ui->graphingView->yAxis->scaleRange(0.666, yrange.center());
    }
    ui->graphingView->replot();
}

void TemporalGraphWindow::zoomOut()
{
    QCPRange xrange = ui->graphingView->xAxis->range();
    QCPRange yrange = ui->graphingView->yAxis->range();
    if (ui->graphingView->xAxis->selectedParts().testFlag(QCPAxis::spAxis))
    {
        ui->graphingView->xAxis->scaleRange(1.5, xrange.center());
    }

    else if (ui->graphingView->yAxis->selectedParts().testFlag(QCPAxis::spAxis))
    {
        ui->graphingView->yAxis->scaleRange(1.5, yrange.center());
    }
    else
    {
        ui->graphingView->xAxis->scaleRange(1.5, xrange.center());
        ui->graphingView->yAxis->scaleRange(1.5, yrange.center());
    }
    ui->graphingView->replot();
}
