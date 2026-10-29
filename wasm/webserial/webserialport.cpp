#include "webserialport.h"

#include <QDebug>
#include <QMetaObject>

// ---------------------------------------------------------------------------------------------------------------
// QSerialPort

QSerialPort::QSerialPort(const QString &name, QObject *parent) : QIODevice(parent), m_portName(name) {}

QSerialPort::QSerialPort(const QSerialPortInfo &info, QObject *parent) : QSerialPort(info.portName(), parent) {}

QSerialPort::~QSerialPort()
{
    if (isOpen()) close();
}

bool QSerialPort::open(OpenMode mode)
{
    if (isOpen())
    {
        setError(OpenError, tr("The port is already open"));
        return false;
    }
    if (!WebSerialHub::isSupported())
    {
        setError(UnsupportedOperationError, tr("This browser has no Web Serial"));
        return false;
    }
    if (m_portName.isEmpty())
    {
        setError(DeviceNotFoundError, tr("No serial port chosen"));
        return false;
    }
    clearError();
    m_rx.clear();
    m_readyReadQueued = false;
    m_pendingTx.clear();
    m_signalsPending = false;
    QIODevice::open(mode | Unbuffered);
    m_opening = true;
    WebSerialHub *hub = WebSerialHub::instance();
    m_handle = hub->m_nextHandle++;
    hub->m_ports.insert(m_handle, this);
    WebSerialHub::backend()->open(m_handle, m_portName, m_baudRate, m_flowControl == HardwareControl);
    return true;
}

void QSerialPort::releaseHandle()
{
    if (m_handle)
    {
        WebSerialHub::instance()->m_ports.remove(m_handle);
        WebSerialHub::backend()->close(m_handle);
        m_handle = 0;
    }
    m_opening = false;
    m_pendingTx.clear();
}

void QSerialPort::close()
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return;
    }
    QIODevice::close(); // emits aboutToClose() while still open, as Qt's
    releaseHandle();
    m_rx.clear();
}

void QSerialPort::opened(bool ok, SerialPortError err, const QString &message)
{
    if (!m_opening) return;
    if (!ok)
    {
        // As Qt: open() failed. The device is closed before errorOccurred(), so a handler may delete or reopen it.
        releaseHandle();
        QIODevice::close();
        setError(err == NoError ? OpenError : err, message);
        return;
    }
    m_opening = false;
    if (m_signalsPending) sendSignals();
    if (!m_pendingTx.isEmpty()) WebSerialHub::backend()->write(m_handle, std::exchange(m_pendingTx, {}));
}

void QSerialPort::setError(SerialPortError err, const QString &message)
{
    m_error = err;
    if (err == NoError) setErrorString(tr("No error"));
    else setErrorString(message.isEmpty() ? tr("Serial port error %1").arg(int(err)) : message);
    emit errorOccurred(err);
}

bool QSerialPort::setBaudRate(qint32 baudRate)
{
    if (baudRate <= 0) return false;
    m_baudRate = baudRate; // used at the next open() (Web Serial can't change it on an open port)
    return true;
}

bool QSerialPort::setFlowControl(FlowControl flowControl)
{
    m_flowControl = flowControl; // at the next open(); Web Serial has no software flow control
    return true;
}

void QSerialPort::sendSignals()
{
    m_signalsPending = false;
    WebSerialHub::backend()->setSignals(m_handle, m_dtr, m_rts);
}

bool QSerialPort::setDataTerminalReady(bool set)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    m_dtr = set;
    // Sent even when unchanged: Web Serial starts with DTR and RTS on, and rc1 takes "both low again" as a cue.
    if (m_opening) m_signalsPending = true;
    else sendSignals();
    return true;
}

bool QSerialPort::setRequestToSend(bool set)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    m_rts = set;
    if (m_opening) m_signalsPending = true;
    else sendSignals();
    return true;
}

qint64 QSerialPort::readData(char *data, qint64 maxSize)
{
    const qint64 n = qMin<qint64>(maxSize, m_rx.size());
    if (n <= 0) return 0;
    memcpy(data, m_rx.constData(), size_t(n));
    m_rx.remove(0, n);
    return n;
}

qint64 QSerialPort::writeData(const char *data, qint64 maxSize)
{
    if (!m_handle) return -1;
    if (maxSize <= 0) return 0;
    const QByteArray bytes(data, maxSize);
    if (m_opening) m_pendingTx.append(bytes);
    else WebSerialHub::backend()->write(m_handle, bytes);
    return maxSize;
}

// ---------------------------------------------------------------------------------------------------------------
// QSerialPortInfo

QSerialPortInfo::QSerialPortInfo(const QString &name) : m_name(name)
{
    for (const QSerialPortInfo &info : availablePorts())
        if (info.portName() == name) m_description = info.description();
}

QList<QSerialPortInfo> QSerialPortInfo::availablePorts()
{
    return WebSerialHub::backend() ? WebSerialHub::backend()->ports() : QList<QSerialPortInfo>();
}

// ---------------------------------------------------------------------------------------------------------------
// WebSerialHub

WebSerialHub *WebSerialHub::instance()
{
    static WebSerialHub *hub = new WebSerialHub(); // lives as long as the page
    return hub;
}

void WebSerialHub::requestPort(bool anyPort)
{
    if (!isSupported())
        deliverPortChosen(QString(), tr("This browser has no Web Serial. Use Chrome, Edge or Opera on a computer "
                                        "(or Firefox 151 or later)."));
    else
        s_backend->requestPort(anyPort);
}

void WebSerialHub::listPorts()
{
    if (s_backend) s_backend->listPorts();
    else deliverPortsChanged();
}

// deliver(port) from the event loop, if the handle is open now and still is then.
template <typename F> void WebSerialHub::later(int handle, F deliver)
{
    QSerialPort *p = instance()->m_ports.value(handle);
    if (!p) return;
    // A deleted port's queued calls are dropped by Qt (p is the context object).
    QMetaObject::invokeMethod(p, [p, handle, deliver] { if (p->m_handle == handle) deliver(p); }, Qt::QueuedConnection);
}

void WebSerialHub::deliverOpened(int handle, bool ok, int serialPortError, const QString &message)
{
    later(handle, [=](QSerialPort *p) { p->opened(ok, QSerialPort::SerialPortError(serialPortError), message); });
}

void WebSerialHub::deliverData(int handle, const QByteArray &bytes)
{
    QSerialPort *p = instance()->m_ports.value(handle);
    if (!p || bytes.isEmpty()) return;
    p->m_rx.append(bytes);
    if (p->m_readyReadQueued) return; // one readyRead for everything that came meanwhile
    p->m_readyReadQueued = true;
    later(handle, [](QSerialPort *p) {
        p->m_readyReadQueued = false;
        emit p->readyRead();
    });
}

void WebSerialHub::deliverError(int handle, int serialPortError, const QString &message)
{
    // As Qt: the port stays open until the app closes it (SavvyCAN does, on ResourceError).
    later(handle, [=](QSerialPort *p) { p->setError(QSerialPort::SerialPortError(serialPortError), message); });
}

void WebSerialHub::deliverPortsChanged()
{
    QMetaObject::invokeMethod(instance(), &WebSerialHub::portsChanged, Qt::QueuedConnection);
}

void WebSerialHub::deliverPortChosen(const QString &name, const QString &error)
{
    QMetaObject::invokeMethod(instance(), [name, error] { emit instance()->portChosen(name, error); },
                              Qt::QueuedConnection);
}
