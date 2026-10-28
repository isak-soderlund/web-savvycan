#include "webdisklog.h"

#include <emscripten/val.h>

using emscripten::val;

// disklog.js is linked in with --pre-js (wasm/disklog/disklog.cmake), so it is there before main().
static val api() { return val::global("SavvyDiskLog"); }

static bool s_active = false;
static QString s_fileName;
static qint64 s_bytes = 0;
static qint64 s_frames = 0;

namespace WebDiskLog {

void start(const QByteArray &header)
{
    if (s_active) return;
    val opts = val::object();
    opts.set("header", val(header.toStdString()));
    s_fileName = QString::fromStdString(api().call<val>("start", opts)["name"].as<std::string>());
    s_active = true;
    s_bytes = header.size();
    s_frames = 0;
}

bool write(const QByteArray &rows, int frames)
{
    if (!s_active) return false;
    s_bytes += rows.size();
    s_frames += frames;
    // A view into the wasm heap; writeCopy copies it before returning.
    val view(emscripten::typed_memory_view(static_cast<size_t>(rows.size()),
                                           reinterpret_cast<const unsigned char *>(rows.constData())));
    return api().call<bool>("writeCopy", view);
}

void setFileName(const QString &name)
{
    if (!name.isEmpty()) s_fileName = name;
}

void stop()
{
    if (!s_active) return;
    s_active = false;
    val opts = val::object();
    opts.set("name", val(s_fileName.toStdString()));
    s_fileName = QString::fromStdString(api().call<val>("stop", opts)["name"].as<std::string>());
}

bool isActive() { return s_active; }
QString fileName() { return s_fileName; }
qint64 bytesWritten() { return s_bytes; }
qint64 framesWritten() { return s_frames; }

} // namespace WebDiskLog
