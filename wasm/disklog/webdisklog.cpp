#include "webdisklog.h"

#include <QDateTime>
#include <QMetaObject>
#include <QPointer>

#include <emscripten/bind.h>
#include <emscripten/val.h>

using emscripten::val;

// disklog.js is linked in with --pre-js (wasm/disklog/disklog.cmake), so it is there before main().
static val api()
{
    return val::global("SavvyDiskLog");
}

static bool apiPresent()
{
    return api().typeOf().as<std::string>() == "object";
}

static QString str(const val &v, const char *key)
{
    val x = v[key];
    if (x.isUndefined() || x.isNull()) return QString();
    return QString::fromStdString(x.as<std::string>());
}

// JS -> C++: every event of the module (and the answers of start/queryDirectory).
static void savvycanDiskLogEvent(val ev)
{
    const QString type = str(ev, "type");
    const QString name = str(ev, "name");
    const QString code = str(ev, "code");
    const QString message = str(ev, "message");
    const QString backend = str(ev, "backend");
    const QString permission = str(ev, "permission");
    const double bytes = ev["bytes"].isNumber() ? ev["bytes"].as<double>() : 0.0;
    const bool crashSafe = ev["crashSafe"].isUndefined() ? true : ev["crashSafe"].as<bool>();
    const int part = ev["part"].isNumber() ? ev["part"].as<int>() : 0;
    // Called from a JS promise callback: deliver through Qt's event loop.
    QMetaObject::invokeMethod(WebDiskLog::instance(), [=]() {
        if (type == QLatin1String("directory"))
            emit WebDiskLog::instance()->directoryInfo(name, permission);
        else
            WebDiskLog::instance()->handleEvent(type, name, code, message, bytes, crashSafe, backend, part);
    }, Qt::QueuedConnection);
}

EMSCRIPTEN_BINDINGS(savvycan_disklog)
{
    emscripten::function("savvycanDiskLogEvent", &savvycanDiskLogEvent);
}

WebDiskLog *WebDiskLog::instance()
{
    static WebDiskLog *self = new WebDiskLog();
    return self;
}

WebDiskLog::WebDiskLog(QObject *parent) : QObject(parent)
{
    if (apiPresent())
        api().call<void>("setListener", val::module_property("savvycanDiskLogEvent"));
}

QString WebDiskLog::backend() const
{
    if (!apiPresent()) return QStringLiteral("none");
    return str(api().call<val>("support"), "backend");
}

void WebDiskLog::start(Mode mode, const QByteArray &header, qint64 splitBytes)
{
    if (m_active || m_starting) return;
    if (!apiPresent())
    {
        emit startFailed(QStringLiteral("unsupported"), tr("The disk logging module is missing."));
        return;
    }
    m_starting = true;
    val opts = val::object();
    switch (mode)
    {
    case Mode::PromptForName: opts.set("mode", val("prompt")); break;
    case Mode::StartTimeName: opts.set("mode", val("direct")); break;
    case Mode::Download:      opts.set("mode", val("download")); break;
    }
    opts.set("header", val(header.toStdString()));
    if (mode == Mode::StartTimeName && splitBytes > 0) opts.set("splitBytes", val(double(splitBytes)));
    // Success arrives as the "started" event; a refusal or failure as "startfailed".
    api()["bridge"].call<void>("start", opts);
}

bool WebDiskLog::write(const QByteArray &rows)
{
    if (!m_active) return false;
    if (rows.isEmpty()) return true;
    // A view into the wasm heap; writeCopy copies it before returning.
    val view(emscripten::typed_memory_view(static_cast<size_t>(rows.size()),
                                           reinterpret_cast<const unsigned char *>(rows.constData())));
    return api().call<bool>("writeCopy", view);
}

void WebDiskLog::stop()
{
    if (!m_active) return;
    // "stopped" (or "error") follows; isActive() is false from now on, so no more rows are taken.
    m_active = false;
    api()["bridge"].call<void>("stop");
}

void WebDiskLog::queryDirectory()
{
    if (!apiPresent()) { emit directoryInfo(QString(), QString()); return; }
    api()["bridge"].call<void>("queryDirectory");
}

void WebDiskLog::chooseDirectory()
{
    if (!apiPresent()) return;
    api()["bridge"].call<void>("chooseDirectory");
}

void WebDiskLog::handleEvent(const QString &type, const QString &name, const QString &code,
                             const QString &message, double bytes, bool crashSafe, const QString &backend, int part)
{
    if (type == QLatin1String("started"))
    {
        m_starting = false;
        m_active = true;
        m_fileName = name;
        m_backend = backend;
        m_crashSafe = crashSafe;
        m_committedBytes = 0;
        m_part = 1;
        m_lastCommit = QDateTime::currentMSecsSinceEpoch();
        emit started(name, backend, crashSafe);
    }
    else if (type == QLatin1String("startfailed"))
    {
        m_starting = false;
        emit startFailed(code, message);
    }
    else if (type == QLatin1String("committed"))
    {
        m_committedBytes = static_cast<qint64>(bytes);
        m_lastCommit = QDateTime::currentMSecsSinceEpoch();
        emit committed(m_committedBytes);
    }
    else if (type == QLatin1String("rotated"))
    {
        m_fileName = name;
        m_part = part;
        m_committedBytes = static_cast<qint64>(bytes);
        m_lastCommit = QDateTime::currentMSecsSinceEpoch();
        emit fileChanged(name, part);
    }
    else if (type == QLatin1String("error"))
    {
        // also the rejection of stop(): the module has reported it as an "error" event already
        if (!name.isEmpty() || m_active)
        {
            m_active = false;
            m_starting = false;
            m_committedBytes = static_cast<qint64>(bytes);
            emit failed(code, message, m_committedBytes);
        }
    }
    else if (type == QLatin1String("stopped"))
    {
        m_active = false;
        m_committedBytes = static_cast<qint64>(bytes);
        emit stopped(name, m_committedBytes);
    }
    else if (type == QLatin1String("directoryfailed"))
    {
        queryDirectory();
    }
}
