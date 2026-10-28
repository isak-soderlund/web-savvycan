#include "testhook.h"
#include "theme.h"
#include "canframemodel.h"
#include "aboutdialog.h"
#include "connections/canconmanager.h"
#include "connections/canconnection.h"
#include "wasm/disklog/webdisklog.h"

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
extern "C" {
EMSCRIPTEN_KEEPALIVE const char *wsc_stats() { return keep(QJsonDocument(stats()).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_theme() { return Theme::isDark() ? "dark" : "light"; }
EMSCRIPTEN_KEEPALIVE const char *wsc_trace_tail(int n) { return keep(QJsonDocument(traceTail(n)).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_version() { return keep(AboutDialog::gitCommit().toUtf8()); }
}

// EM_JS: its body may contain commas, EM_ASM's may not
EM_JS(void, wsc_install_hook, (), {
    window.webSavvyCAN = {
        version: UTF8ToString(_wsc_version()),
        stats: () => JSON.parse(UTF8ToString(_wsc_stats())),
        theme: () => UTF8ToString(_wsc_theme()),
        traceTail: (n) => JSON.parse(UTF8ToString(_wsc_trace_tail(n | 0))),
    };
});

void TestHook::install(CommFrameModel *model)
{
    g_model = model;
    wsc_install_hook();
}
