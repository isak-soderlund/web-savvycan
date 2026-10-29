#include "testhook.h"
#include "theme.h"
#include "canframemodel.h"
#include "aboutdialog.h"
#include "connections/canconmanager.h"
#include "connections/canconnection.h"
#include "wasm/disklog/webdisklog.h"
#include "mainwindow.h"
#include "re/graphingwindow.h"
#include "qcustomplot.h"
#include <QApplication>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <emscripten.h>

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
// point's time (the test of Follow End of Graph).
static QJsonObject graphState()
{
    QJsonObject o;
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
    o.insert("graphs", plot->graphCount());
    o.insert("points", points);
    o.insert("lastKey", lastKey);
    o.insert("xLower", plot->xAxis->range().lower);
    o.insert("xUpper", plot->xAxis->range().upper);
    o.insert("yLower", plot->yAxis->range().lower);
    o.insert("yUpper", plot->yAxis->range().upper);
    return o;
}

extern "C" {
// A new graphing window with one graph, as RE Tools > New Graphing Window and its "Add New Graph"
// dialog make it: frame ID, start bit, bits, Intel byte order.
EMSCRIPTEN_KEEPALIVE int wsc_add_graph(int id, int startBit, int numBits, int intel)
{
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
        addGraph: (id, startBit, numBits, intel = true) => _wsc_add_graph(id, startBit, numBits, intel ? 1 : 0) === 1,
        graphState: () => JSON.parse(UTF8ToString(_wsc_graph_state())),
        setTheme: (mode) => _wsc_set_theme({ system: 0, light: 1, dark: 2 }[mode] ?? 0),
        traceTail: (n) => JSON.parse(UTF8ToString(_wsc_trace_tail(n | 0))),
    };
});

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

void TestHook::install(CommFrameModel *model)
{
    g_model = model;
    wsc_install_hook();
    wsc_install_temporal_hook();
}
