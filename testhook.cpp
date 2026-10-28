#include "testhook.h"
#include "theme.h"
#include "canframemodel.h"
#include "aboutdialog.h"
#include "connections/canconmanager.h"
#include "connections/canconnection.h"

#include <QJsonDocument>
#include <QMap>
#include <QPointer>

#ifdef Q_OS_WASM
#include <emscripten.h>
#include "wasm/disklog/webdisklog.h"
#endif

namespace {
QMap<QString, TestHook::StatFn> &registry()
{
    static QMap<QString, TestHook::StatFn> map;
    return map;
}
QPointer<CommFrameModel> g_model;
}

namespace TestHook {

void registerStat(const QString &name, StatFn fn)
{
    registry().insert(name, std::move(fn));
}

QJsonObject stats()
{
    QJsonObject o;
    for (auto it = registry().cbegin(); it != registry().cend(); ++it)
        o.insert(it.key(), it.value()());
    return o;
}

QString theme()
{
    return Theme::isDark() ? QStringLiteral("dark") : QStringLiteral("light");
}

QJsonArray traceTail(int n)
{
    QJsonArray rows;
    if (!g_model || n <= 0) return rows;
    const QVector<CommFrame> tail = g_model->getFilteredListReference()->tail(n); // one locked snapshot
    for (const CommFrame &f : tail) {
        QJsonArray data;
        const QByteArray payload = f.payload();
        for (char c : payload) data.append(int(static_cast<unsigned char>(c)));
        QJsonObject row;
        row.insert("id", double(f.frameId()));
        row.insert("ext", f.hasExtendedFrameFormat());
        row.insert("fd", f.frameType() == CommFrame::FrameType::CANFDDataFrame);
        row.insert("brs", f.hasBitrateSwitch());
        row.insert("esi", f.hasErrorStateIndicator());
        row.insert("bus", f.getBus());
        row.insert("data", data);
        rows.append(row);
    }
    return rows;
}

#ifdef Q_OS_WASM
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
EMSCRIPTEN_KEEPALIVE const char *wsc_theme() { return keep(theme().toUtf8()); }
EMSCRIPTEN_KEEPALIVE const char *wsc_trace_tail(int n) { return keep(QJsonDocument(traceTail(n)).toJson(QJsonDocument::Compact)); }
EMSCRIPTEN_KEEPALIVE const char *wsc_version() { return keep(AboutDialog::gitCommit().toUtf8()); }
}

// window.webSavvyCAN (EM_JS: its body may contain commas, EM_ASM's may not)
EM_JS(void, wsc_install_hook, (), {
    window.webSavvyCAN = {
        version: UTF8ToString(_wsc_version()),
        stats: () => JSON.parse(UTF8ToString(_wsc_stats())),
        theme: () => UTF8ToString(_wsc_theme()),
        traceTail: (n) => JSON.parse(UTF8ToString(_wsc_trace_tail(n | 0))),
    };
});
#endif

void install(CommFrameModel *model)
{
    g_model = model;
    if (!registry().contains("connected")) {
        registerStat("connected", [] {
            for (CANConnection *c : CANConManager::getInstance()->getConnections())
                if (c && c->getStatus() == CANCon::CONNECTED) return QJsonValue(true);
            return QJsonValue(false);
        });
    }
    if (!registry().contains("framesInTrace")) {
        registerStat("framesInTrace", [] {
            return QJsonValue(g_model ? double(g_model->getFilteredListReference()->count()) : 0.0);
        });
    }
    // the frame store and the receive path (web-savvycan framestore)
    if (!registry().contains("framesReceived")) {
        // frames the trace took in since the last Clear (kept, or since evicted by the ring)
        registerStat("framesReceived", [] { return QJsonValue(g_model ? double(g_model->framesIngested()) : 0.0); });
    }
    if (!registry().contains("framesEvicted")) {
        // the oldest frames dropped by the ring (MaximumFrames) to make room, since the last Clear
        registerStat("framesEvicted", [] { return QJsonValue(g_model ? double(g_model->framesWrapped()) : 0.0); });
    }
    if (!registry().contains("framesDropped")) {
        // frames the connections had to discard (queue to the GUI full): the status bar's "Dropped"
        registerStat("framesDropped", [] { return QJsonValue(double(CANConManager::getInstance()->getDroppedFrames())); });
    }
    if (!registry().contains("queued")) {
        // frames received by the connections, not yet in the trace
        registerStat("queued", [] { return QJsonValue(double(CANConManager::getInstance()->getQueuedFrames())); });
    }
#ifdef Q_OS_WASM
    if (!registry().contains("logging")) {
        registerStat("logging", [] { return QJsonValue(WebDiskLog::instance()->isActive()); });
        registerStat("logFile", [] { return QJsonValue(WebDiskLog::instance()->fileName()); });
        registerStat("logFramesWritten", [] { return QJsonValue(double(WebDiskLog::instance()->framesWritten())); });
    }
    wsc_install_hook();
#endif
}

} // namespace TestHook
