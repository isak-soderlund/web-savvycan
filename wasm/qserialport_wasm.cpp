// web-savvycan wasm build: the QSerialPort stand-in (see qserialport_wasm.h).
// Placeholder: no device behind it yet. The Web Serial backing replaces open(), close(),
// writeData(), the control lines and availablePorts().
#include "qserialport_wasm.h"

#include <cstring>

QSerialPort::QSerialPort(QObject *parent) : QIODevice(parent) {}

QSerialPort::QSerialPort(const QString &name, QObject *parent) : QIODevice(parent), m_portName(name) {}

QSerialPort::QSerialPort(const QSerialPortInfo &info, QObject *parent)
    : QIODevice(parent), m_portName(info.portName()) {}

QSerialPort::~QSerialPort()
{
    if (isOpen()) close();
}

void QSerialPort::setPortName(const QString &name) { m_portName = name; }
QString QSerialPort::portName() const { return m_portName; }

bool QSerialPort::open(OpenMode mode)
{
    Q_UNUSED(mode);
    setError(DeviceNotFoundError, QStringLiteral("Web Serial is not connected yet"));
    return false;
}

void QSerialPort::close()
{
    m_readBuffer.clear();
    QIODevice::close();
}

bool QSerialPort::setBaudRate(qint32 baudRate, Directions directions)
{
    m_baudRate = baudRate;
    emit baudRateChanged(baudRate, directions);
    return true;
}
qint32 QSerialPort::baudRate(Directions) const { return m_baudRate; }
bool QSerialPort::setDataBits(DataBits dataBits) { m_dataBits = dataBits; return true; }
QSerialPort::DataBits QSerialPort::dataBits() const { return m_dataBits; }
bool QSerialPort::setParity(Parity parity) { m_parity = parity; return true; }
QSerialPort::Parity QSerialPort::parity() const { return m_parity; }
bool QSerialPort::setStopBits(StopBits stopBits) { m_stopBits = stopBits; return true; }
QSerialPort::StopBits QSerialPort::stopBits() const { return m_stopBits; }
bool QSerialPort::setFlowControl(FlowControl flowControl) { m_flowControl = flowControl; return true; }
QSerialPort::FlowControl QSerialPort::flowControl() const { return m_flowControl; }

bool QSerialPort::setDataTerminalReady(bool set)
{
    m_dtr = set;
    emit dataTerminalReadyChanged(set);
    return true;
}
bool QSerialPort::isDataTerminalReady() { return m_dtr; }

bool QSerialPort::setRequestToSend(bool set)
{
    m_rts = set;
    emit requestToSendChanged(set);
    return true;
}
bool QSerialPort::isRequestToSend() { return m_rts; }

bool QSerialPort::flush() { return isOpen(); }

bool QSerialPort::clear(Directions directions)
{
    if (directions & Input) m_readBuffer.clear();
    return true;
}

QSerialPort::SerialPortError QSerialPort::error() const { return m_error; }
void QSerialPort::clearError() { setError(NoError); }

bool QSerialPort::isSequential() const { return true; }

qint64 QSerialPort::bytesAvailable() const
{
    return m_readBuffer.size() + QIODevice::bytesAvailable();
}

void QSerialPort::deliverIncoming(const QByteArray &data)
{
    if (data.isEmpty()) return;
    m_readBuffer.append(data);
    emit readyRead();
}

void QSerialPort::setError(SerialPortError error, const QString &text)
{
    m_error = error;
    if (error == NoError) return;
    setErrorString(text.isEmpty() ? QStringLiteral("Serial port error") : text);
    emit errorOccurred(error);
}

qint64 QSerialPort::readData(char *data, qint64 maxSize)
{
    const qint64 n = qMin<qint64>(maxSize, m_readBuffer.size());
    if (n <= 0) return 0;
    memcpy(data, m_readBuffer.constData(), size_t(n));
    m_readBuffer.remove(0, n);
    return n;
}

qint64 QSerialPort::writeData(const char *data, qint64 maxSize)
{
    Q_UNUSED(data);
    if (!isOpen()) {
        setError(NotOpenError, QStringLiteral("Port not open"));
        return -1;
    }
    return maxSize; // placeholder: Web Serial writer.write() goes here
}

QList<QSerialPortInfo> QSerialPortInfo::availablePorts()
{
    return {}; // placeholder: the ports the page was granted (navigator.serial.getPorts())
}
