#include "webserialhub.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutexLocker>

WebSerialBackend *WebSerialHub::s_backend = nullptr;

WebSerialHub::WebSerialHub(QObject *parent) : QObject(parent) {}

WebSerialHub *WebSerialHub::instance()
{
    static WebSerialHub *hub = new WebSerialHub(); // lives as long as the page
    return hub;
}

void WebSerialHub::setBackend(WebSerialBackend *backend) { s_backend = backend; }
WebSerialBackend *WebSerialHub::backend() { return s_backend; }

bool WebSerialHub::isSupported() { return s_backend && s_backend->isSupported(); }

void WebSerialHub::requestPort(bool anyPort)
{
    if (!isSupported())
    {
        deliverPortChosen(QString(), tr("This browser has no Web Serial. Use Chrome, Edge or Opera on a computer "
                                        "(or Firefox 151 or later)."));
        return;
    }
    s_backend->requestPort(anyPort);
}

int WebSerialHub::registerPort(QSerialPort *port)
{
    QMutexLocker lock(&m_mutex);
    int handle = m_nextHandle++;
    if (m_nextHandle <= 0) m_nextHandle = 1; // wrapped after 2^31 opens: never 0
    m_ports.insert(handle, port);
    return handle;
}

void WebSerialHub::unregisterPort(int handle)
{
    QMutexLocker lock(&m_mutex);
    m_ports.remove(handle);
    m_openData.remove(handle);
}

QSerialPort *WebSerialHub::port(int handle) const
{
    QMutexLocker lock(&m_mutex);
    return m_ports.value(handle).data();
}

QList<QSerialPortInfo> WebSerialHub::availablePorts()
{
    QList<QSerialPortInfo> list;
    if (!s_backend) return list;
    for (const WebSerialPortDescription &d : s_backend->ports())
    {
        QSerialPortInfo info;
        info.m_name = d.name;
        info.m_description = d.description;
        info.m_vid = d.vid;
        info.m_pid = d.pid;
        info.m_hasUsbIds = d.hasUsbIds;
        list.append(info);
    }
    return list;
}

void WebSerialHub::processPending()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

// Queues `deliver(port)` into the port's thread, if the handle is open. At delivery the port must still have this
// handle (closed or reopened ports get nothing). Any report other than data closes the open data batch, so the
// order of reports is kept.
template <typename F> void WebSerialHub::toPort(int handle, F &&deliver)
{
    WebSerialHub *hub = instance();
    QPointer<QSerialPort> p;
    {
        QMutexLocker lock(&hub->m_mutex);
        p = hub->m_ports.value(handle);
        hub->m_openData.remove(handle);
    }
    if (!p) return;
    QMetaObject::invokeMethod(p.data(), [p, handle, deliver = std::forward<F>(deliver)]() {
        if (p && p->m_handle == handle) deliver(p.data());
    }, Qt::QueuedConnection);
}

void WebSerialHub::deliverOpened(int handle, bool ok, int serialPortError, const QString &message)
{
    toPort(handle, [=](QSerialPort *p) { p->hubOpened(ok, QSerialPort::SerialPortError(serialPortError), message); });
}

void WebSerialHub::deliverData(int handle, const QByteArray &bytes)
{
    if (bytes.isEmpty()) return;
    WebSerialHub *hub = instance();
    QPointer<QSerialPort> p;
    std::shared_ptr<QByteArray> batch;
    {
        QMutexLocker lock(&hub->m_mutex);
        p = hub->m_ports.value(handle);
        if (!p) return;
        auto it = hub->m_openData.constFind(handle);
        if (it != hub->m_openData.constEnd())
        {
            it.value()->append(bytes); // the port hasn't taken the last batch yet: one readyRead for both
            return;
        }
        batch = std::make_shared<QByteArray>(bytes);
        hub->m_openData.insert(handle, batch);
    }
    QMetaObject::invokeMethod(p.data(), [p, handle, batch, hub]() {
        QByteArray bytes;
        {
            QMutexLocker lock(&hub->m_mutex);
            auto it = hub->m_openData.find(handle);
            if (it != hub->m_openData.end() && it.value() == batch) hub->m_openData.erase(it);
            bytes = std::move(*batch);
        }
        if (p && p->m_handle == handle) p->hubData(bytes);
    }, Qt::QueuedConnection);
}

void WebSerialHub::deliverWritten(int handle, qint64 bytes)
{
    toPort(handle, [=](QSerialPort *p) { p->hubWritten(bytes); });
}

void WebSerialHub::deliverError(int handle, int serialPortError, const QString &message)
{
    toPort(handle, [=](QSerialPort *p) { p->hubError(QSerialPort::SerialPortError(serialPortError), message); });
}

void WebSerialHub::deliverReadError(int handle, const QString &message)
{
    toPort(handle, [=](QSerialPort *p) { p->hubReadError(message); });
}

void WebSerialHub::deliverPortsChanged()
{
    WebSerialHub *hub = instance();
    QMetaObject::invokeMethod(hub, [hub]() { emit hub->portsChanged(); }, Qt::QueuedConnection);
}

void WebSerialHub::deliverPortChosen(const QString &name, const QString &error)
{
    WebSerialHub *hub = instance();
    QMetaObject::invokeMethod(hub, [hub, name, error]() { emit hub->portChosen(name, error); }, Qt::QueuedConnection);
}
