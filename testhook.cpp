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

void TestHook::install(CommFrameModel *model)
{
    g_model = model;
    wsc_install_hook();
}
