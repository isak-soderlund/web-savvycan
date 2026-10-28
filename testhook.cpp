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
#include <emscripten/bind.h>
#include <string>
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
    const QVector<CommFrame> *list = g_model->getFilteredListReference();
    const int count = list->count();
    for (int i = qMax(0, count - n); i < count; ++i) {
        const CommFrame &f = list->at(i);
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
static std::string jsonStats() { return QJsonDocument(stats()).toJson(QJsonDocument::Compact).toStdString(); }
static std::string jsonTheme() { return theme().toStdString(); }
static std::string jsonTraceTail(int n) { return QJsonDocument(traceTail(n)).toJson(QJsonDocument::Compact).toStdString(); }
static std::string jsonVersion() { return AboutDialog::gitCommit().toStdString(); }

EMSCRIPTEN_BINDINGS(web_savvycan_testhook)
{
    emscripten::function("wscStats", &jsonStats);
    emscripten::function("wscTheme", &jsonTheme);
    emscripten::function("wscTraceTail", &jsonTraceTail);
    emscripten::function("wscVersion", &jsonVersion);
}
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
            return QJsonValue(g_model ? g_model->getFilteredListReference()->count() : 0);
        });
    }
#ifdef Q_OS_WASM
    EM_ASM({
        const mod = Module;
        window.webSavvyCAN = {
            version: mod.wscVersion(),
            stats: () => JSON.parse(mod.wscStats()),
            theme: () => mod.wscTheme(),
            traceTail: (n) => JSON.parse(mod.wscTraceTail(n | 0)),
        };
    });
#endif
}

} // namespace TestHook
