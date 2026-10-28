#include "webserialport.h"
#include "webserialhub.h"

#include <QDebug>

// ---------------------------------------------------------------------------------------------------------------
// QSerialPort

QSerialPort::QSerialPort(QObject *parent) : QIODevice(parent) {}

QSerialPort::QSerialPort(const QString &name, QObject *parent) : QIODevice(parent), m_portName(name) {}

QSerialPort::QSerialPort(const QSerialPortInfo &info, QObject *parent) : QIODevice(parent), m_portName(info.portName())
{
}

QSerialPort::~QSerialPort()
{
    if (isOpen()) close();
}

void QSerialPort::setPortName(const QString &name) { m_portName = name; }
void QSerialPort::setPort(const QSerialPortInfo &info) { m_portName = info.portName(); }

bool QSerialPort::open(OpenMode mode)
{
    if (isOpen())
    {
        setError(OpenError, tr("The port is already open"));
        return false;
    }
    if (!(mode & ReadWrite))
    {
        setError(UnsupportedOperationError, tr("Unsupported open mode"));
        return false;
    }
    WebSerialBackend *backend = WebSerialHub::backend();
    if (!backend || !backend->isSupported())
    {
        setError(UnsupportedOperationError, tr("This browser has no Web Serial"));
        return false;
    }
    if (m_portName.isEmpty())
    {
        setError(DeviceNotFoundError, tr("No serial port chosen"));
        return false;
    }

    // Web Serial: 7 or 8 data bits, 1 or 2 stop bits, parity none/even/odd, flow control none/hardware.
    WebSerialOpenOptions o;
    o.baudRate = m_baudRate > 0 ? m_baudRate : 115200;
    o.dataBits = m_dataBits == Data7 ? 7 : 8;
    o.stopBits = m_stopBits == TwoStop ? 2 : 1;
    o.parity = m_parity == EvenParity ? QStringLiteral("even") : m_parity == OddParity ? QStringLiteral("odd")
                                                                                        : QStringLiteral("none");
    o.flowControl = m_flowControl == HardwareControl ? QStringLiteral("hardware") : QStringLiteral("none");
    if (m_dataBits == Data5 || m_dataBits == Data6 || m_stopBits == OneAndHalfStop || m_parity == SpaceParity
        || m_parity == MarkParity || m_flowControl == SoftwareControl)
        qWarning() << "WebSerial:" << m_portName << "settings Web Serial lacks were replaced (8 data bits / 1 stop bit "
                      "/ no parity / no flow control)";

    clearError();
    m_rx.clear();
    m_pendingTx.clear();
    m_bytesToWrite = 0;
    m_readErrors = 0;
    m_signalsPending = false;
    QIODevice::open(mode | Unbuffered);
    m_state = State::Opening;
    m_handle = WebSerialHub::instance()->registerPort(this);
    backend->open(m_handle, m_portName, o);
    return true;
}

void QSerialPort::releaseHandle()
{
    if (m_handle)
    {
        WebSerialHub::instance()->unregisterPort(m_handle);
        if (WebSerialBackend *backend = WebSerialHub::backend()) backend->close(m_handle);
        m_handle = 0;
    }
    m_state = State::Closed;
    m_pendingTx.clear();
    m_bytesToWrite = 0;
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

void QSerialPort::hubOpened(bool ok, SerialPortError err, const QString &message)
{
    if (m_state != State::Opening) return;
    if (!ok)
    {
        // As Qt: open() failed. The device is closed before errorOccurred(), so a handler may delete or reopen it.
        releaseHandle();
        QIODevice::close();
        setError(err == NoError ? OpenError : err, message);
        return;
    }
    m_state = State::Open;
    if (m_signalsPending) sendSignals();
    if (!m_pendingTx.isEmpty())
    {
        const QByteArray tx = std::exchange(m_pendingTx, {});
        WebSerialHub::backend()->write(m_handle, tx);
    }
}

void QSerialPort::hubData(const QByteArray &bytes)
{
    m_rx.append(bytes);
    emit readyRead();
}

void QSerialPort::hubWritten(qint64 bytes)
{
    m_bytesToWrite = qMax<qint64>(0, m_bytesToWrite - bytes);
    emit bytesWritten(bytes);
}

void QSerialPort::hubError(SerialPortError err, const QString &message)
{
    // As Qt: the port stays "open" until the app closes it (SavvyCAN does, on ResourceError).
    setError(err, message);
}

void QSerialPort::hubReadError(const QString &message)
{
    ++m_readErrors;
    qWarning() << "WebSerial:" << m_portName << "read error, data lost, reading on:" << message;
}

void QSerialPort::setError(SerialPortError err, const QString &message)
{
    m_error = err;
    if (err == NoError) setErrorString(tr("No error"));
    else setErrorString(message.isEmpty() ? tr("Serial port error %1").arg(int(err)) : message);
    emit errorOccurred(err);
}

void QSerialPort::clearError() { setError(NoError); }

bool QSerialPort::setBaudRate(qint32 baudRate, Directions directions)
{
    Q_UNUSED(directions);
    if (baudRate <= 0)
    {
        setError(UnsupportedOperationError, tr("Invalid baud rate"));
        return false;
    }
    if (baudRate == m_baudRate) return true;
    m_baudRate = baudRate;
    if (isOpen()) qWarning() << "WebSerial:" << m_portName << "baud rate change takes effect at the next open()";
    emit baudRateChanged(baudRate, AllDirections);
    return true;
}

qint32 QSerialPort::baudRate(Directions) const { return m_baudRate; }

bool QSerialPort::setDataBits(DataBits dataBits)
{
    if (dataBits == m_dataBits) return true;
    m_dataBits = dataBits;
    emit dataBitsChanged(dataBits);
    return true;
}

bool QSerialPort::setParity(Parity parity)
{
    if (parity == m_parity) return true;
    m_parity = parity;
    emit parityChanged(parity);
    return true;
}

bool QSerialPort::setStopBits(StopBits stopBits)
{
    if (stopBits == m_stopBits) return true;
    m_stopBits = stopBits;
    emit stopBitsChanged(stopBits);
    return true;
}

bool QSerialPort::setFlowControl(FlowControl flowControl)
{
    if (flowControl == m_flowControl) return true;
    m_flowControl = flowControl;
    emit flowControlChanged(flowControl);
    return true;
}

void QSerialPort::sendSignals()
{
    m_signalsPending = false;
    WebSerialHub::backend()->setSignals(m_handle, m_dtr, m_rts, m_break);
}

bool QSerialPort::setDataTerminalReady(bool set)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    const bool changed = set != m_dtr;
    m_dtr = set;
    // Sent even when unchanged: Web Serial starts with DTR and RTS on, and rc1 takes "both low again" as a cue.
    if (m_state == State::Open) sendSignals();
    else m_signalsPending = true;
    if (changed) emit dataTerminalReadyChanged(set);
    return true;
}

bool QSerialPort::setRequestToSend(bool set)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    if (m_flowControl == HardwareControl)
    {
        setError(UnsupportedOperationError, tr("RTS is driven by hardware flow control"));
        return false;
    }
    const bool changed = set != m_rts;
    m_rts = set;
    if (m_state == State::Open) sendSignals();
    else m_signalsPending = true;
    if (changed) emit requestToSendChanged(set);
    return true;
}

bool QSerialPort::setBreakEnabled(bool set)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    const bool changed = set != m_break;
    m_break = set;
    if (m_state == State::Open) sendSignals();
    else m_signalsPending = true;
    if (changed) emit breakEnabledChanged(set);
    return true;
}

QSerialPort::PinoutSignals QSerialPort::pinoutSignals()
{
    // Web Serial reads the inputs (CTS, DSR, DCD, RI) asynchronously only: the outputs are reported.
    PinoutSignals s = NoSignal;
    if (m_dtr) s |= DataTerminalReadySignal;
    if (m_rts) s |= RequestToSendSignal;
    return s;
}

bool QSerialPort::flush()
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    return true; // every write is with the browser already
}

bool QSerialPort::clear(Directions directions)
{
    if (!isOpen())
    {
        setError(NotOpenError, tr("The port is not open"));
        return false;
    }
    if (directions & Input) m_rx.clear();
    if (directions & Output) m_pendingTx.clear(); // what the browser has is beyond reach
    return true;
}

qint64 QSerialPort::bytesAvailable() const { return m_rx.size() + QIODevice::bytesAvailable(); }

qint64 QSerialPort::bytesToWrite() const { return m_bytesToWrite + m_pendingTx.size(); }

bool QSerialPort::canReadLine() const { return m_rx.contains('\n') || QIODevice::canReadLine(); }

bool QSerialPort::waitForReadyRead(int) { return false; }
bool QSerialPort::waitForBytesWritten(int) { return false; }

qint64 QSerialPort::readData(char *data, qint64 maxSize)
{
    const qint64 n = qMin<qint64>(maxSize, m_rx.size());
    if (n <= 0) return 0;
    memcpy(data, m_rx.constData(), size_t(n));
    m_rx.remove(0, n);
    return n;
}

qint64 QSerialPort::readLineData(char *data, qint64 maxSize)
{
    // QIODevice::readLine leaves room for the terminating 0.
    const qsizetype nl = m_rx.indexOf('\n');
    const qint64 want = nl >= 0 ? nl + 1 : m_rx.size();
    return readData(data, qMin(want, maxSize));
}

qint64 QSerialPort::writeData(const char *data, qint64 maxSize)
{
    if (m_state == State::Closed) return -1;
    if (maxSize <= 0) return 0;
    const QByteArray bytes(data, maxSize);
    if (m_state == State::Opening)
    {
        m_pendingTx.append(bytes);
        return maxSize;
    }
    m_bytesToWrite += maxSize;
    WebSerialHub::backend()->write(m_handle, bytes);
    return maxSize;
}

// ---------------------------------------------------------------------------------------------------------------
// QSerialPortInfo

QSerialPortInfo::QSerialPortInfo(const QString &name)
{
    for (const QSerialPortInfo &info : availablePorts())
    {
        if (info.portName() == name)
        {
            *this = info;
            return;
        }
    }
    m_name = name; // not (yet) available: keep the name, as Qt's does for a missing device on some platforms
}

QSerialPortInfo::QSerialPortInfo(const QSerialPort &port) : QSerialPortInfo(port.portName()) {}

void QSerialPortInfo::swap(QSerialPortInfo &other) noexcept
{
    std::swap(m_name, other.m_name);
    std::swap(m_description, other.m_description);
    std::swap(m_manufacturer, other.m_manufacturer);
    std::swap(m_vid, other.m_vid);
    std::swap(m_pid, other.m_pid);
    std::swap(m_hasUsbIds, other.m_hasUsbIds);
}

QList<QSerialPortInfo> QSerialPortInfo::availablePorts() { return WebSerialHub::availablePorts(); }

QList<qint32> QSerialPortInfo::standardBaudRates()
{
    return {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000, 3000000};
}
