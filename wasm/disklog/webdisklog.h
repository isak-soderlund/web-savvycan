#ifndef WEBDISKLOG_H
#define WEBDISKLOG_H

// web-savvycan: continuous logging in the browser (WebAssembly build only). A thin front for
// wasm/disklog/disklog.js (globalThis.SavvyDiskLog): the rows are collected in the page and stop()
// starts one download of the CSV. Everything is synchronous.

#include <QByteArray>
#include <QString>

namespace WebDiskLog {

void start(const QByteArray &header);      // the file is named by the start time
bool write(const QByteArray &rows, int frames);
void setFileName(const QString &name);     // the download's name (asked at stop)
void stop();                               // starts the download

bool isActive();
QString fileName();
qint64 bytesWritten();                     // header + rows so far
qint64 framesWritten();

} // namespace WebDiskLog

#endif // WEBDISKLOG_H
