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
#include "wasm/windowtabs.h"
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
// The windows are found through WindowTabs (in the browser they are tabs of the main window, not top-levels); "open"
// is a window with its tab (in front or not), or a visible top-level (a modal dialog).
// The newest graphing window (the tests open one at a time).
static GraphingWindow *graphWindow()
{
    GraphingWindow *found = nullptr;
    for (QWidget *w : WindowTabs::windows())
        if (auto *g = qobject_cast<GraphingWindow *>(w); g && WindowTabs::isOpen(g)) found = g;
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
    for (QWidget *w : WindowTabs::windows())
        if (auto *d = qobject_cast<NewGraphDialog *>(w); d && WindowTabs::isOpen(d)) return d;
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
static void installDbcHook();       // DBC (stage 3), at the end of this file
// ---- Temporal Graph (stage 4): what RE Tools > Temporal Graph shows. Its own block (and its own EM_JS below),
// so it merges apart from the other windows' hooks.
#include "re/temporalgraphwindow.h"
#include <cmath>

static TemporalGraphWindow *temporalWindow()
{
    for (QWidget *w : WindowTabs::windows())
        if (auto *t = qobject_cast<TemporalGraphWindow *>(w); t && WindowTabs::isOpen(t)) return t;
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

// ---- Tabs (wasm/windowtabs.h): webSavvyCAN.tabs(): the tab bar (titles, which is current, each tab's place, the
// row's height, the bar's width and its natural width, the page's reserve); selectTab(i) / closeTab(i) as a click on
// the tab or its close button.
extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_tabs() { return keep(QJsonDocument(WindowTabs::state()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE int wsc_select_tab(int i) { return WindowTabs::selectTab(i) ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int wsc_close_tab(int i) { return WindowTabs::closeTab(i) ? 1 : 0; }
// A window as its menu opens it (MainWindow's show... slot, by its index in kOpenSlots), from the event loop.
static const char *const kOpenSlots[] = {"showGraphingWindow", "showFrameDataAnalysis", "showTemporalGraphWindow",
                                         "showConnectionSettingsWindow", "showSettingsDialog"};
EMSCRIPTEN_KEEPALIVE int wsc_open_window(int i)
{
    if (i < 0 || i >= int(sizeof kOpenSlots / sizeof *kOpenSlots)) return 0;
    return QMetaObject::invokeMethod(MainWindow::getReference(), kOpenSlots[i], Qt::QueuedConnection) ? 1 : 0;
}
}

EM_JS(void, wsc_install_tabs_hook, (), {
    Object.assign(window.webSavvyCAN, {
        tabs: () => JSON.parse(UTF8ToString(_wsc_tabs())),
        selectTab: (i) => _wsc_select_tab(i | 0) === 1,
        closeTab: (i) => _wsc_close_tab(i | 0) === 1,
        // 'graph', 'frameInfo', 'temporal', 'connection', 'preferences'
        openWindow: (name) =>
            _wsc_open_window({ graph: 0, frameInfo: 1, temporal: 2, connection: 3, preferences: 4 }[name] ?? -1) === 1,
    });
});
// ---- end of tabs

void TestHook::install(CommFrameModel *model)
{
    g_model = model;
    wsc_install_hook();
    installFrameInfoHook();
    wsc_install_temporal_hook();
    wsc_install_memory_hook();
    wsc_install_tabs_hook();
    installDbcHook();
    TestHook::installFileTools();
    installReTools();
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
    for (QWidget *w : WindowTabs::windows())
        if (auto *f = qobject_cast<FrameInfoWindow *>(w); f && WindowTabs::isOpen(f)) return f;
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
    // its place on the page (in the browser a tab: below the tab row)
    const QPoint at = win->isWindow() ? win->pos() : win->mapTo(win->window(), QPoint(0, 0));
    o.insert("x", at.x());
    o.insert("y", at.y());
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

// ---- DBC (stage 3): File > DBC File Manager, Interpret Frames, Signal Viewer, the DBC editors, graphs by signal name.
// webSavvyCAN.dbc(): the loaded files with their messages and signals; .traceDecoded(n): the last n rows of the trace
// as shown (the Data column's text, with Interpret Frames its decoded lines, and the rows' colours); the others drive
// the windows where Qt's combo boxes, tables and trees aren't reachable through the accessibility tree.
#include "dbc/dbchandler.h"
#include "dbc/dbcloadsavewindow.h"
#include "dbc/dbcmaineditor.h"
#include "dbc/dbcmessageeditor.h"
#include "dbc/dbcsignaleditor.h"
#include "signalviewerwindow.h"
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QTableView>
#include <QTableWidget>
#include <QTreeWidget>

template <typename T> static T *visibleWindow()
{
    for (QWidget *w : WindowTabs::windows())   // tabs of the main window in the browser (wasm/windowtabs.h)
        if (auto *t = qobject_cast<T *>(w); t && WindowTabs::isOpen(t)) return t;
    return nullptr;
}

static QJsonObject dbcState()
{
    QJsonObject o;
    DBCHandler *h = DBCHandler::getReference();
    QJsonArray files;
    for (int i = 0; i < h->getFileCount(); i++) {
        DBCFile *f = h->getFileByIdx(i);
        QJsonObject fo;
        fo.insert("name", f->getFilename());
        fo.insert("path", f->getFullFilename());
        fo.insert("bus", f->getAssocBus());
        fo.insert("dirty", f->getDirtyFlag());
        QJsonArray msgs;
        for (DBC_MESSAGE *m : f->messageHandler->getMsgsAsList()) {
            QJsonObject mo;
            mo.insert("id", double(m->ID));
            mo.insert("ext", m->extendedID);
            mo.insert("name", m->name);
            mo.insert("len", int(m->len));
            mo.insert("fd", m->canFD);
            mo.insert("brs", m->bitRateSwitch);
            mo.insert("sender", m->sender ? m->sender->name : QString());
            mo.insert("bg", m->bgColor.isValid() ? m->bgColor.name() : QString());
            mo.insert("fg", m->fgColor.isValid() ? m->fgColor.name() : QString());
            QJsonArray sigs;
            for (DBC_SIGNAL *s : m->sigHandler->getSignalsAsList()) {
                QJsonObject so;
                so.insert("name", s->name);
                so.insert("startBit", s->startBit);
                so.insert("size", s->signalSize);
                so.insert("intel", s->intelByteOrder);
                so.insert("type", int(s->valType));
                so.insert("factor", s->factor);
                so.insert("offset", s->bias);
                so.insert("min", s->min);
                so.insert("max", s->max);
                so.insert("unit", s->unitName);
                so.insert("mux", s->isMultiplexor);
                so.insert("muxed", s->isMultiplexed);
                so.insert("muxValues", s->multiplexDbcString(DBC_SIGNAL::MuxStringFormat_UI));
                QJsonArray vals;
                for (const DBC_VAL_ENUM_ENTRY &v : s->valList) vals.append(QJsonArray{double(v.value), v.descript});
                so.insert("values", vals);
                sigs.append(so);
            }
            mo.insert("signals", sigs);
            msgs.append(mo);
        }
        fo.insert("messages", msgs);
        files.append(fo);
    }
    o.insert("files", files);
    o.insert("interpret", g_model->getInterpretMode());
    if (DBCLoadSaveWindow *w = visibleWindow<DBCLoadSaveWindow>())
        if (QTableWidget *t = w->findChild<QTableWidget *>("tableFiles")) {
            QJsonArray rows;
            for (int r = 0; r < t->rowCount(); r++)
                rows.append(t->item(r, 0) ? t->item(r, 0)->text() : QString());
            o.insert("managerRows", rows);
        }
    return o;
}

// The last n rows of the trace as it shows them: the Data column's text and the rows' colours
static QJsonArray traceDecoded(int n)
{
    QJsonArray rows;
    const int count = g_model->rowCount();
    for (int r = qMax(0, count - n); r < count; r++) {
        const QModelIndex data = g_model->index(r, int(Column::Data));
        QJsonObject row;
        row.insert("id", g_model->index(r, int(Column::FrameId)).data().toString());
        row.insert("text", data.data(Qt::DisplayRole).toString());
        const QVariant bg = data.data(Qt::BackgroundRole), fg = data.data(Qt::ForegroundRole);
        row.insert("bg", bg.canConvert<QColor>() ? bg.value<QColor>().name() : bg.value<QBrush>().color().name());
        row.insert("fg", fg.canConvert<QColor>() ? fg.value<QColor>().name() : fg.value<QBrush>().color().name());
        rows.append(row);
    }
    return rows;
}

static QComboBox *combo(QWidget *w, const char *name) { return w ? w->findChild<QComboBox *>(name) : nullptr; }
// a combo box's item picked by its text (exact, else the first that contains it), as a click in its list would
static bool pickText(QComboBox *box, const QString &text)
{
    if (!box) return false;
    int i = box->findText(text);
    if (i < 0) i = box->findText(text, Qt::MatchContains);
    if (i < 0) return false;
    box->setCurrentIndex(i);
    return true;
}

static QJsonObject signalViewerState()
{
    QJsonObject o;
    SignalViewerWindow *w = visibleWindow<SignalViewerWindow>();
    o.insert("open", w != nullptr);
    if (!w) return o;
    QTableWidget *t = w->findChild<QTableWidget *>("tableViewer");
    QJsonArray rows;
    for (int r = 0; t && r < t->rowCount(); r++) {
        QJsonObject row;
        row.insert("node", t->item(r, 0) ? t->item(r, 0)->text() : QString());
        row.insert("signal", t->item(r, 1) ? t->item(r, 1)->text() : QString());
        row.insert("value", t->item(r, 2) ? t->item(r, 2)->text() : QString());
        rows.append(row);
    }
    o.insert("rows", rows);
    QJsonArray nodes;
    if (QComboBox *b = combo(w, "cbNodes"))
        for (int i = 0; i < b->count(); i++) nodes.append(b->itemText(i));
    o.insert("nodes", nodes);
    return o;
}

// The DBC editors that are open: the message editor's and the signal editor's fields and bit grid
static QJsonObject dbcEditorState()
{
    QJsonObject o;
    o.insert("main", visibleWindow<DBCMainEditor>() != nullptr);
    if (DBCMainEditor *m = visibleWindow<DBCMainEditor>())
        if (QTreeWidget *t = m->findChild<QTreeWidget *>("treeDBC")) o.insert("treeTop", t->topLevelItemCount());
    if (DBCMessageEditor *e = visibleWindow<DBCMessageEditor>()) {
        QJsonObject me;
        for (const char *name : {"lineMsgName", "lineFrameID", "lineFrameLen", "lineComment"})
            if (QLineEdit *l = e->findChild<QLineEdit *>(name)) me.insert(name, l->text());
        for (const char *name : {"checkCanFD", "checkBRS", "checkExtended"})
            if (QCheckBox *c = e->findChild<QCheckBox *>(name)) me.insert(name, c->isChecked());
        o.insert("message", me);
    }
    if (DBCSignalEditor *e = visibleWindow<DBCSignalEditor>()) {
        QJsonObject se;
        for (const char *name : {"txtName", "txtBitLength", "txtFactor", "txtBias", "txtUnitName"})
            if (QLineEdit *l = e->findChild<QLineEdit *>(name)) se.insert(name, l->text());
        if (CANDataGrid *g = e->findChild<CANDataGrid *>("bitfield")) {
            se.insert("gridBytes", g->getBytesToDraw());
            QJsonArray used;   // the bits the grid gives a signal name (the message's signals)
            for (int b = 0; b < 512; b++) if (!g->getUsedSignalName(b).isEmpty()) used.append(b);
            se.insert("usedBits", used);
        }
        o.insert("signal", se);
    }
    return o;
}

// All points of graph i of the newest graphing window (at most max)
static QJsonObject graphPoints(int i, int max)
{
    QJsonObject o;
    GraphingWindow *win = graphWindow();
    if (!win || i < 0 || i >= win->graphs().count() || !win->graphs()[i].ref) return o;
    const GraphParams &g = win->graphs()[i];
    QJsonArray keys, values;
    int n = 0;
    for (auto it = g.ref->data()->constBegin(); it != g.ref->data()->constEnd() && n < max; ++it, ++n) {
        keys.append(it->key);
        values.append(it->value);
    }
    o.insert("name", g.graphName);
    o.insert("signal", g.associatedSignal ? g.associatedSignal->name : QString());
    o.insert("keys", keys);
    o.insert("values", values);
    return o;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_dbc_state() { return keep(QJsonDocument(dbcState()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_trace_decoded(int n) { return keep(QJsonDocument(traceDecoded(n)).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_signal_viewer() { return keep(QJsonDocument(signalViewerState()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_dbc_editors() { return keep(QJsonDocument(dbcEditorState()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_graph_points(int i, int max) { return keep(QJsonDocument(graphPoints(i, max)).toJson(QJsonDocument::Compact)); }
// The DBC File Manager's row i selected, as a click on it (Save, Edit, Remove act on the selected row)
EMSCRIPTEN_KEEPALIVE int wsc_dbc_select_file(int i)
{
    DBCLoadSaveWindow *w = visibleWindow<DBCLoadSaveWindow>();
    QTableWidget *t = w ? w->findChild<QTableWidget *>("tableFiles") : nullptr;
    if (!t || i < 0 || i >= t->rowCount()) return 0;
    t->setCurrentCell(i, 0);
    return 1;
}
// In the open DBC editor, the tree's message (by its name) or signal (msg + signal name) double-clicked: its editor opens
EMSCRIPTEN_KEEPALIVE int wsc_dbc_edit(const char *msgName, const char *sigName)
{
    DBCMainEditor *m = visibleWindow<DBCMainEditor>();
    QTreeWidget *tree = m ? m->findChild<QTreeWidget *>("treeDBC") : nullptr;
    if (!tree) return 0;
    const QString msg = QString::fromUtf8(msgName), sig = QString::fromUtf8(sigName);
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        QTreeWidgetItem *item = *it;
        const QStringList words = item->text(0).split(' ');
        bool isMsg = words.count() > 1 && words[1] == msg && item->parent();       // "0x1DB Name - comment"
        bool hit = sig.isEmpty() ? isMsg
                                 : (item->parent() && item->parent()->text(0).split(' ').value(1) == msg
                                    && (words[0] == sig || item->text(0).contains(") " + sig + " ")));
        if (!hit) continue;
        tree->setCurrentItem(item);
        emit tree->doubleClicked(tree->currentIndex());
        return 1;
    }
    return 0;
}
// Signal Viewer's Node / Message / Signal boxes set by their text (then "Add To Viewer" is clicked by the test)
EMSCRIPTEN_KEEPALIVE int wsc_signal_viewer_pick(const char *node, const char *msg, const char *sig)
{
    SignalViewerWindow *w = visibleWindow<SignalViewerWindow>();
    return w && pickText(combo(w, "cbNodes"), QString::fromUtf8(node)) && pickText(combo(w, "cbMessages"), QString::fromUtf8(msg))
           && pickText(combo(w, "cbSignals"), QString::fromUtf8(sig)) ? 1 : 0;
}
// Add New Graph's "Graph a DBC Signal" boxes set by their text (then "Copy Signal Parameters" is clicked by the test)
EMSCRIPTEN_KEEPALIVE int wsc_graph_dialog_pick(const char *node, const char *msg, const char *sig)
{
    NewGraphDialog *d = graphDialog();
    return d && pickText(combo(d, "cbNodes"), QString::fromUtf8(node)) && pickText(combo(d, "cbMessages"), QString::fromUtf8(msg))
           && pickText(combo(d, "cbSignals"), QString::fromUtf8(sig)) ? 1 : 0;
}
// The trace's context menu over line `line` of the Data cell of the row `fromEnd` rows before the newest (0 = the
// newest), as a right click there; the row is expanded first, as a click does. The test then picks its entry by name.
EMSCRIPTEN_KEEPALIVE int wsc_trace_context_menu(int fromEnd, int line)
{
    MainWindow *mw = MainWindow::getReference();
    QTableView *view = mw ? mw->findChild<QTableView *>("canFramesView") : nullptr;
    const int row = g_model->rowCount() - 1 - fromEnd;
    if (!view || row < 0) return 0;
    view->resizeRowToContents(row);
    view->scrollTo(g_model->index(row, int(Column::Data)));
    const int y = view->rowViewportPosition(row) + line * view->fontMetrics().height() + view->fontMetrics().height() / 2;
    const int x = view->columnViewportPosition(int(Column::Data)) + 10;
    emit view->customContextMenuRequested(QPoint(x, y));
    return 1;
}
}

EM_JS(void, wsc_install_dbc_hook, (), {
    const str = (s) => stringToNewUTF8(String(s));
    const call = (f, ...strs) => { const p = strs.map(str); try { return f(...p); } finally { p.forEach(_free); } };
    Object.assign(window.webSavvyCAN, {
        dbc: () => JSON.parse(UTF8ToString(_wsc_dbc_state())),
        traceDecoded: (n) => JSON.parse(UTF8ToString(_wsc_trace_decoded(n | 0))),
        signalViewer: () => JSON.parse(UTF8ToString(_wsc_signal_viewer())),
        dbcEditors: () => JSON.parse(UTF8ToString(_wsc_dbc_editors())),
        graphPoints: (i, max = 1000000) => JSON.parse(UTF8ToString(_wsc_graph_points(i | 0, max | 0))),
        dbcSelectFile: (i) => _wsc_dbc_select_file(i | 0) === 1,
        dbcEdit: (msg, sig = '') => call(_wsc_dbc_edit, msg, sig) === 1,
        signalViewerPick: (node, msg, sig) => call(_wsc_signal_viewer_pick, node, msg, sig) === 1,
        graphDialogPick: (node, msg, sig) => call(_wsc_graph_dialog_pick, node, msg, sig) === 1,
        traceContextMenu: (fromEnd, line) => _wsc_trace_context_menu(fromEnd | 0, line | 0) === 1,
    });
});

static void installDbcHook()
{
    wsc_install_dbc_hook();
}
// ---- end of DBC
