#ifndef TESTHOOK_H
#define TESTHOOK_H

/*
 * web-savvycan: the test hook for the browser tests (branch tests, tests/web/README.md).
 *
 * In the wasm build install() sets window.webSavvyCAN = { version, stats(), theme(), traceTail(n) }.
 * stats() is built from named counters: this file registers "connected", "framesInTrace", the frame store's
 * framesReceived/framesEvicted/framesDropped/queued and the logging state from WebDiskLog (logging, logFile,
 * logFramesWritten); registerStat() adds others or replaces one (install() keeps an entry already registered).
 * On the desktop install() does nothing; registerStat() and stats() work the same (for unit tests).
 */

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <functional>

class CommFrameModel;

namespace TestHook {

using StatFn = std::function<QJsonValue()>;

void registerStat(const QString &name, StatFn fn);
QJsonObject stats();
QString theme();                         // "light" / "dark"
QJsonArray traceTail(int n);             // [{id, ext, fd, brs, esi, data: [bytes]}], oldest first

// Call once, after the main window exists.
void install(CommFrameModel *model);

} // namespace TestHook

#endif // TESTHOOK_H
