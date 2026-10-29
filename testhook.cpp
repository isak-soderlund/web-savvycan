#include "testhook.h"
#include "theme.h"
#include "canframemodel.h"
#include "aboutdialog.h"
#include "connections/canconmanager.h"
#include "connections/canconnection.h"
#include "wasm/disklog/webdisklog.h"
#include "mainwindow.h"
#include "re/graphingwindow.h"
#include "re/newgraphdialog.h"
#include "candatagrid.h"
#include "qcustomplot.h"
#include <QApplication>
#include <QLineEdit>
#include <QSettings>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <emscripten.h>
#include <emscripten/heap.h>

// The C library's mallinfo() (emscripten's dlmalloc and emmalloc have it; its malloc.h doesn't declare it): ten
// size_t fields, uordblks = bytes allocated now.
struct WscMallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
extern "C" WscMallinfo wsc_libc_mallinfo(void) __asm__("mallinfo");

static CommFrameModel *g_model = nullptr;

static QJsonObject stats()
{
    CANConManager *conns = CANConManager::getInstance();
    bool connected = false;
    for (CANConnection *c : conns->getConnections())
        if (c && c->getStatus() == CANCon::CONNECTED) connected = true;
    QJsonObject o;
    o.insert("connected", connected);
    o.insert("framesReceived", double(g_model->framesIngested()));   // taken in by the trace since Clear
    o.insert("framesInTrace", double(g_model->getFilteredListReference()->count()));
    o.insert("framesEvicted", double(g_model->framesWrapped()));     // scrolled out of the ring since Clear
    o.insert("framesDropped", double(conns->getDroppedFrames()));    // the status bar's "Dropped"
    o.insert("queued", double(conns->getQueuedFrames()));            // received, not yet in the trace
    o.insert("logging", WebDiskLog::isActive());
    o.insert("logFile", WebDiskLog::fileName());
    o.insert("logFramesWritten", double(WebDiskLog::framesWritten()));
    return o;
}

static QJsonArray traceTail(int n)
{
    QJsonArray rows;
    if (n <= 0) return rows;
    for (const CommFrame &f : g_model->getFilteredListReference()->tail(n)) {
        QJsonArray data;
        for (char c : f.payload()) data.append(int(static_cast<unsigned char>(c)));
        QJsonObject row;
        row.insert("id", double(f.frameId()));
        row.insert("ext", f.hasExtendedFrameFormat());
        row.insert("fd", f.frameType() == CommFrame::CANFDDataFrame);
        row.insert("brs", f.hasBitrateSwitch());
        row.insert("esi", f.hasErrorStateIndicator());
        row.insert("bus", f.getBus());
        // the trace's time in µs (exact in a double up to 2^53 µs, 285 years)
        row.insert("time", double(f.timeStamp().seconds() * 1000000LL + f.timeStamp().microSeconds()));
        row.insert("data", data);
        rows.append(row);
    }
    return rows;
}

// Plain C exports (not embind: in an Asyncify build embind calls come back as Promises; these stay
// synchronous, so the tests can read webSavvyCAN.stats().framesReceived directly).
static const char *keep(const QByteArray &utf8)
{
    static QByteArray buffer;
    buffer = utf8;
    return buffer.constData();
}
// The newest graphing window (the tests open one at a time).
static GraphingWindow *graphWindow()
{
    GraphingWindow *found = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *g = qobject_cast<GraphingWindow *>(w); g && g->isVisible()) found = g;
    return found;
}

// What the newest graphing window shows: graphs, points in all of them, the visible x range and the last
// point's time (the test of Follow End of Graph); per graph what it stores (the bound of RESOLVE.md stage 2); the
// wasm heap (in use by malloc, and the size of the memory).
static QJsonObject graphState()
{
    QJsonObject o;
    const WscMallinfo mi = wsc_libc_mallinfo();
    o.insert("heapUsed", double(mi.uordblks));
    o.insert("heapSize", double(emscripten_get_heap_size()));
    o.insert("cap", GraphingWindow::kMaxPoints);
    GraphingWindow *win = graphWindow();
    QCustomPlot *plot = win ? win->findChild<QCustomPlot *>() : nullptr;
    o.insert("open", plot != nullptr);
    if (!plot) return o;
    double points = 0, lastKey = 0;
    for (int i = 0; i < plot->graphCount(); i++) {
        const auto data = plot->graph(i)->data();
        points += data->size();
        if (!data->isEmpty()) lastKey = qMax(lastKey, (data->constEnd() - 1)->key);
    }
    QJsonArray series;
    for (const GraphParams &g : win->graphs()) {
        if (!g.ref) continue;
        const auto data = g.ref->data();
        QJsonObject s;
        s.insert("name", g.graphName);
        s.insert("startBit", g.startBit);
        s.insert("numBits", g.numBits);
        s.insert("stored", data->size());
        s.insert("bytes", double(data->size()) * double(sizeof(QCPGraphData)));
        s.insert("envelope", g.envelopePoints);
        s.insert("bucket", g.bucketSamples);
        s.insert("samples", g.samples);
        if (!data->isEmpty()) {
            bool found = false;
            const QCPRange values = data->valueRange(found);
            s.insert("firstKey", data->constBegin()->key);
            s.insert("lastKey", (data->constEnd() - 1)->key);
            s.insert("firstValue", data->constBegin()->value);
            s.insert("lastValue", (data->constEnd() - 1)->value);
            s.insert("minValue", values.lower);
            s.insert("maxValue", values.upper);
        }
        series.append(s);
    }
    o.insert("series", series);
    o.insert("graphs", plot->graphCount());
    o.insert("points", points);
    o.insert("lastKey", lastKey);
    o.insert("xLower", plot->xAxis->range().lower);
    o.insert("xUpper", plot->xAxis->range().upper);
    o.insert("yLower", plot->yAxis->range().lower);
    o.insert("yUpper", plot->yAxis->range().upper);
    return o;
}

// The Add New Graph dialog that is open (Graph > Add New Graph runs it with exec()).
static NewGraphDialog *graphDialog()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *d = qobject_cast<NewGraphDialog *>(w); d && d->isVisible()) return d;
    return nullptr;
}

static QJsonObject graphDialogState()
{
    QJsonObject o;
    NewGraphDialog *d = graphDialog();
    o.insert("open", d != nullptr);
    if (!d) return o;
    if (CANDataGrid *grid = d->findChild<CANDataGrid *>("gridData")) {
        o.insert("gridBytes", grid->getBytesToDraw());
        o.insert("gridWidth", grid->width());
        o.insert("gridHeight", grid->height());
    }
    for (const char *name : {"txtID", "txtStartBit", "txtDataLen", "editFrameLen"})
        if (QLineEdit *e = d->findChild<QLineEdit *>(name)) o.insert(name, e->text());
    return o;
}

extern "C" {
// A new graphing window with one graph, as RE Tools > New Graphing Window and its "Add New Graph"
// dialog make it: frame ID, start bit, bits, Intel byte order; with sameWindow, one more graph in the newest window.
EMSCRIPTEN_KEEPALIVE int wsc_add_graph(int id, int startBit, int numBits, int intel, int sameWindow)
{
    if (!sameWindow || !graphWindow())
        QMetaObject::invokeMethod(MainWindow::getReference(), "showGraphingWindow", Qt::DirectConnection);
    GraphingWindow *win = graphWindow();
    if (!win) return 0;
    GraphParams params;
    params.ID = uint32_t(id);
    params.startBit = startBit;
    params.numBits = numBits;
    params.intelFormat = intel != 0;
    params.graphName = QStringLiteral("test");
    win->createGraph(params);
    return 1;
}
// A text field of the open Add New Graph dialog set as typing would (its textChanged slots run): 0 the ID (as hex),
// 1 the start bit, 2 the bit length, 3 the frame length.
EMSCRIPTEN_KEEPALIVE int wsc_graph_dialog_set(int field, int value)
{
    static const char *const names[] = {"txtID", "txtStartBit", "txtDataLen", "editFrameLen"};
    NewGraphDialog *d = graphDialog();
    QLineEdit *e = d && field >= 0 && field < 4 ? d->findChild<QLineEdit *>(names[field]) : nullptr;
    if (!e) return 0;
    e->setText(field == 0 ? QStringLiteral("0x") + QString::number(uint32_t(value), 16).toUpper() : QString::number(value));
    return 1;
}
// Graph > Add New Graph in the newest graphing window, from the event loop (the dialog runs with exec()).
EMSCRIPTEN_KEEPALIVE int wsc_open_graph_dialog()
{
    GraphingWindow *win = graphWindow();
    return win && QMetaObject::invokeMethod(win, "addNewGraph", Qt::QueuedConnection) ? 1 : 0;
}
// The visible ranges of the newest graphing window, as dragging and zooming set them.
EMSCRIPTEN_KEEPALIVE int wsc_set_graph_view(double xLower, double xUpper, double yLower, double yUpper)
{
    GraphingWindow *win = graphWindow();
    QCustomPlot *plot = win ? win->findChild<QCustomPlot *>() : nullptr;
    if (!plot) return 0;
    plot->xAxis->setRange(xLower, xUpper);
    plot->yAxis->setRange(yLower, yUpper);
    plot->replot();
    return 1;
}
EMSCRIPTEN_KEEPALIVE const char *wsc_graph_dialog() { return keep(QJsonDocument(graphDialogState()).toJson(QJsonDocument::Compact)); }
// The trace's size (frames kept) as File > Preferences stores it (QSettings Main/MaximumFrames, the page's
// localStorage); read at start: reload after.
EMSCRIPTEN_KEEPALIVE void wsc_set_max_frames(int frames)
{
    QSettings settings;
    settings.setValue(QStringLiteral("Main/MaximumFrames"), frames);
    settings.sync();
}
EMSCRIPTEN_KEEPALIVE const char *wsc_graph_state() { return keep(QJsonDocument(graphState()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_stats() { return keep(QJsonDocument(stats()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_theme() { return Theme::isDark() ? "dark" : "light"; }
EMSCRIPTEN_KEEPALIVE const char *wsc_trace_tail(int n) { return keep(QJsonDocument(traceTail(n)).toJson(QJsonDocument::Compact)); }
// For the tests: the theme as File > Preferences > Theme sets it (0 follow system, 1 light, 2 dark).
EMSCRIPTEN_KEEPALIVE void wsc_set_theme(int mode)
{
    Theme::instance()->setMode(mode == 2 ? Theme::Mode::Dark : mode == 1 ? Theme::Mode::Light : Theme::Mode::System);
}
EMSCRIPTEN_KEEPALIVE const char *wsc_version() { return keep(AboutDialog::gitCommit().toUtf8()); }
}

// EM_JS: its body may contain commas, EM_ASM's may not
EM_JS(void, wsc_install_hook, (), {
    window.webSavvyCAN = {
        version: UTF8ToString(_wsc_version()),
        stats: () => JSON.parse(UTF8ToString(_wsc_stats())),
        theme: () => UTF8ToString(_wsc_theme()),
        addGraph: (id, startBit, numBits, intel = true, sameWindow = false) =>
            _wsc_add_graph(id, startBit, numBits, intel ? 1 : 0, sameWindow ? 1 : 0) === 1,
        graphState: () => JSON.parse(UTF8ToString(_wsc_graph_state())),
        graphDialog: () => JSON.parse(UTF8ToString(_wsc_graph_dialog())),
        openGraphDialog: () => _wsc_open_graph_dialog() === 1,
        setGraphView: (xLower, xUpper, yLower, yUpper) => _wsc_set_graph_view(xLower, xUpper, yLower, yUpper) === 1,
        setGraphDialogField: (name, value) =>
            _wsc_graph_dialog_set({ id: 0, startBit: 1, bits: 2, frameLen: 3 }[name] ?? -1, value | 0) === 1,
        setMaxFrames: (frames) => _wsc_set_max_frames(frames | 0),
        setTheme: (mode) => _wsc_set_theme({ system: 0, light: 1, dark: 2 }[mode] ?? 0),
        traceTail: (n) => JSON.parse(UTF8ToString(_wsc_trace_tail(n | 0))),
    };
});

static void installFrameInfoHook(); // Frame Data Analysis, at the end of this file
// ---- Temporal Graph (stage 4): what RE Tools > Temporal Graph shows. Its own block (and its own EM_JS below),
// so it merges apart from the other windows' hooks.
#include "re/temporalgraphwindow.h"
#include <cmath>

static TemporalGraphWindow *temporalWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *t = qobject_cast<TemporalGraphWindow *>(w); t && t->isVisible()) return t;
    return nullptr;
}

// The IDs with their frame counts, the time span, the column width, the colour map's size and what it holds, the
// plot's items, the memory of the counts and the map, the visible ranges.
static QJsonObject temporalState()
{
    QJsonObject o;
    TemporalGraphWindow *win = temporalWindow();
    o.insert("open", win != nullptr);
    if (!win) return o;
    const TemporalDensity &d = win->density();
    QCustomPlot *plot = win->plot();
    QJsonArray ids;
    const std::vector<quint64> &keys = d.sortedKeys();
    for (int r = 0; r < int(keys.size()); r++) {
        double n = 0;
        int from = -1, to = -1;   // its first and last column with frames
        const std::vector<quint32> &counts = d.countsOfRank(r);
        for (int c = 0; c < int(counts.size()); c++) {
            if (!counts[size_t(c)]) continue;
            n += counts[size_t(c)];
            if (from < 0) from = c;
            to = c;
        }
        QJsonObject id;
        id.insert("id", double(quint32(keys[size_t(r)])));
        id.insert("ext", (keys[size_t(r)] >> 32) != 0);
        id.insert("frames", n);
        id.insert("fromUs", double(d.originUs() + qint64(from) * d.binUs()));     // start of its first column
        id.insert("toUs", double(d.originUs() + qint64(to + 1) * d.binUs()));     // end of its last column
        ids.append(id);
    }
    double plotted = 0, nonEmpty = 0;
    int keySize = 0, valueSize = 0;
    for (int i = 0; i < plot->plottableCount(); i++) {
        if (auto *map = qobject_cast<QCPColorMap *>(plot->plottable(i))) {
            QCPColorMapData *m = map->data();
            keySize = m->keySize();
            valueSize = m->valueSize();
            for (int r = 0; r < valueSize; r++)
                for (int c = 0; c < keySize; c++)
                    if (const double v = m->cell(c, r); !std::isnan(v) && v > 0) { plotted += v; nonEmpty++; }
        }
    }
    o.insert("building", win->building());
    o.insert("frames", double(d.frames()));
    o.insert("hiddenFrames", double(d.hiddenFrames()));
    o.insert("ids", ids);
    o.insert("firstUs", double(d.firstUs()));
    o.insert("lastUs", double(d.lastUs()));
    o.insert("binUs", double(d.binUs()));
    o.insert("columns", d.usedColumns());
    o.insert("maxColumns", TemporalDensity::Columns);
    o.insert("maxIds", TemporalDensity::MaxIds);
    o.insert("mapKeySize", keySize);
    o.insert("mapValueSize", valueSize);
    o.insert("plotted", plotted);           // frames in the map's cells (= frames - hiddenFrames)
    o.insert("nonEmptyCells", nonEmpty);
    o.insert("plottables", plot->plottableCount());
    o.insert("graphs", plot->graphCount());
    o.insert("items", plot->itemCount());
    o.insert("memoryBytes", double(d.memoryBytes() + win->mapBytes()));
    o.insert("xLower", plot->xAxis->range().lower);
    o.insert("xUpper", plot->xAxis->range().upper);
    o.insert("yLower", plot->yAxis->range().lower);
    o.insert("yUpper", plot->yAxis->range().upper);
    return o;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_temporal_state() { return keep(QJsonDocument(temporalState()).toJson(QJsonDocument::Compact)); }
}

EM_JS(void, wsc_install_temporal_hook, (), {
    window.webSavvyCAN.temporalState = () => JSON.parse(UTF8ToString(_wsc_temporal_state()));
});
// ---- end of Temporal Graph

// ---- The page's memory (the leaner log loader, loader.spec): webSavvyCAN.memory(): heapUsed (bytes malloc has
// handed out now, mallinfo), maxFootprint (the most malloc ever had from the system: the peak, with its free gaps),
// heapSize (the wasm memory: it never shrinks), storeBytes (the trace's frames in the store), frames (in the store).
static QJsonObject memoryState()
{
    const WscMallinfo mi = wsc_libc_mallinfo();
    QJsonObject o;
    o.insert("heapUsed", double(mi.uordblks));
    o.insert("maxFootprint", double(mi.usmblks));
    o.insert("heapSize", double(emscripten_get_heap_size()));
    o.insert("storeBytes", double(g_model->storeMemoryBytes()));
    o.insert("frames", double(g_model->getListReference()->count()));
    return o;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_memory() { return keep(QJsonDocument(memoryState()).toJson(QJsonDocument::Compact)); }
}

EM_JS(void, wsc_install_memory_hook, (), {
    window.webSavvyCAN.memory = () => JSON.parse(UTF8ToString(_wsc_memory()));
});
// ---- end of memory

void TestHook::install(CommFrameModel *model)
{
    g_model = model;
    wsc_install_hook();
    installFrameInfoHook();
    wsc_install_temporal_hook();
    wsc_install_memory_hook();
    TestHook::installFileTools();
}

// ---- Frame Data Analysis (RE Tools > Frame Data Analysis, the heatmap tool) -------------------------------------
// webSavvyCAN.frameInfo(): what its window shows: the IDs in its list, the ID analysed, its frames and lengths, per
// bit (512, CAN FD) the changes from frame to frame (the heatmap's counts), the frames with the bit set (Bitfield
// Histogram) and the heatmap's level, the heatmap grid's size, the byte graphs shown and their points.
// webSavvyCAN.frameInfoSelect(id): selects the ID in its list, as a click on it (the analysis runs at once).
#include "re/frameinfowindow.h"
#include "filterutility.h"
#include <QListWidget>
#include <QTreeWidget>

static FrameInfoWindow *frameInfoWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *f = qobject_cast<FrameInfoWindow *>(w); f && f->isVisible()) return f;
    return nullptr;
}

static QJsonObject frameInfoState()
{
    QJsonObject o;
    FrameInfoWindow *win = frameInfoWindow();
    o.insert("open", win != nullptr);
    if (!win) return o;
    QJsonArray ids;
    if (QListWidget *list = win->findChild<QListWidget *>("listFrameID")) {
        for (int i = 0; i < list->count(); i++) ids.append(double(FilterUtility::getIdAsInt(list->item(i))));
        if (list->currentItem()) o.insert("current", double(FilterUtility::getIdAsInt(list->currentItem())));
    }
    o.insert("ids", ids);
    const FrameInfoWindow::Analysis &a = win->analysis();
    o.insert("valid", a.valid);
    o.insert("id", double(a.id));
    o.insert("frames", a.frames);
    o.insert("minLen", a.minLen);
    o.insert("maxLen", a.maxLen);
    o.insert("byteGraphs", a.byteGraphs);
    o.insert("bytePoints", a.bytePoints);
    QJsonArray flips, sets, heat;
    for (int i = 0; i < FrameInfoWindow::kMaxBits; i++) {
        flips.append(a.flips[i]);
        sets.append(a.sets[i]);
        heat.append(int(a.heat[i]));
    }
    o.insert("flips", flips);
    o.insert("sets", sets);
    o.insert("heat", heat);
    if (CANDataGrid *grid = win->findChild<CANDataGrid *>()) {
        o.insert("gridBytes", grid->getBytesToDraw());
        o.insert("gridWidth", grid->width());
        o.insert("gridHeight", grid->height());
    }
    if (QTreeWidget *tree = win->findChild<QTreeWidget *>("treeDetails")) {
        QJsonArray lines; // the details' first level: "# of frames: ...", "Data Length: ...", ...
        if (QTreeWidgetItem *top = tree->topLevelItem(0))
            for (int i = 0; i < top->childCount(); i++) lines.append(top->child(i)->text(0));
        o.insert("details", lines);
    }
    int shown = 0;
    for (QCustomPlot *plot : win->findChildren<QCustomPlot *>())
        if (plot->isVisible()) shown++;
    o.insert("plotsVisible", shown); // the byte graphs and the two histograms
    o.insert("x", win->x());
    o.insert("y", win->y());
    o.insert("width", win->width());
    o.insert("height", win->height());
    return o;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_frameinfo_state() { return keep(QJsonDocument(frameInfoState()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE int wsc_frameinfo_select(int id)
{
    FrameInfoWindow *win = frameInfoWindow();
    QListWidget *list = win ? win->findChild<QListWidget *>("listFrameID") : nullptr;
    if (!list) return 0;
    for (int i = 0; i < list->count(); i++) {
        if (FilterUtility::getIdAsInt(list->item(i)) == uint32_t(id)) {
            list->setCurrentRow(i);
            return 1;
        }
    }
    return 0;
}
}

EM_JS(void, wsc_install_frameinfo_hook, (), {
    Object.assign(window.webSavvyCAN, {
        frameInfo: () => JSON.parse(UTF8ToString(_wsc_frameinfo_state())),
        frameInfoSelect: (id) => _wsc_frameinfo_select(id | 0) === 1,
    });
});

static void installFrameInfoHook()
{
    wsc_install_frameinfo_hook();
}
