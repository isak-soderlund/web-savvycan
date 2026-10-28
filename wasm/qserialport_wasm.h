// web-savvycan wasm build: a QSerialPort / QSerialPortInfo stand-in.
//
// Qt Serial Port does not build for WebAssembly, so the wasm build gets these classes from here
// (wasm/include/QSerialPort, QSerialPortInfo and QtSerialPort/... forward to this header). They
// mirror the part of Qt's API SavvyCAN uses (gvretserial.cpp, lawicel_serial.cpp,
// newconnectiondialog.cpp), so the GVRET/LAWICEL code stays as it is.
//
// For now the port does nothing (open() fails with DeviceNotFoundError, no ports are listed).
// The Web Serial backing (a JS read loop pushing bytes in, write() going to writer.write(),
// navigator.serial.requestPort() for the port list) goes into qserialport_wasm.cpp.
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QString>

class QSerialPortInfo;

class QSerialPort : public QIODevice
{
    Q_OBJECT
public:
    enum Direction { Input = 1, Output = 2, AllDirections = Input | Output };
    Q_DECLARE_FLAGS(Directions, Direction)

    enum BaudRate { Baud1200 = 1200, Baud2400 = 2400, Baud4800 = 4800, Baud9600 = 9600,
                    Baud19200 = 19200, Baud38400 = 38400, Baud57600 = 57600, Baud115200 = 115200 };
    enum DataBits { Data5 = 5, Data6 = 6, Data7 = 7, Data8 = 8 };
    enum Parity { NoParity = 0, EvenParity = 2, OddParity = 3, SpaceParity = 4, MarkParity = 5 };
    enum StopBits { OneStop = 1, OneAndHalfStop = 3, TwoStop = 2 };
    enum FlowControl { NoFlowControl, HardwareControl, SoftwareControl };
    enum SerialPortError { NoError, DeviceNotFoundError, PermissionError, OpenError, WriteError,
                           ReadError, ResourceError, UnsupportedOperationError, UnknownError,
                           TimeoutError, NotOpenError,
                           // not in Qt 6 any more, but SavvyCAN's switch statements name them
                           ParityError, FramingError, BreakConditionError };
    Q_ENUM(SerialPortError)

    explicit QSerialPort(QObject *parent = nullptr);
    explicit QSerialPort(const QString &name, QObject *parent = nullptr);
    explicit QSerialPort(const QSerialPortInfo &info, QObject *parent = nullptr);
    ~QSerialPort() override;

    void setPortName(const QString &name);
    QString portName() const;

    bool open(OpenMode mode) override;
    void close() override;

    bool setBaudRate(qint32 baudRate, Directions directions = AllDirections);
    qint32 baudRate(Directions directions = AllDirections) const;
    bool setDataBits(DataBits dataBits);
    DataBits dataBits() const;
    bool setParity(Parity parity);
    Parity parity() const;
    bool setStopBits(StopBits stopBits);
    StopBits stopBits() const;
    bool setFlowControl(FlowControl flowControl);
    FlowControl flowControl() const;

    bool setDataTerminalReady(bool set);
    bool isDataTerminalReady();
    bool setRequestToSend(bool set);
    bool isRequestToSend();

    bool flush();
    bool clear(Directions directions = AllDirections);
    SerialPortError error() const;
    void clearError();

    bool isSequential() const override;
    qint64 bytesAvailable() const override;

    // For the Web Serial backing: bytes that arrived from the device. Appends them to the read
    // buffer and emits readyRead().
    void deliverIncoming(const QByteArray &data);
    // For the Web Serial backing: report an error (emits errorOccurred()).
    void setError(SerialPortError error, const QString &text = QString());

signals:
    void errorOccurred(QSerialPort::SerialPortError error);
    void baudRateChanged(qint32 baudRate, QSerialPort::Directions directions);
    void dataTerminalReadyChanged(bool set);
    void requestToSendChanged(bool set);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    QString m_portName;
    qint32 m_baudRate = Baud115200;
    DataBits m_dataBits = Data8;
    Parity m_parity = NoParity;
    StopBits m_stopBits = OneStop;
    FlowControl m_flowControl = NoFlowControl;
    bool m_dtr = false;
    bool m_rts = false;
    SerialPortError m_error = NoError;
    QByteArray m_readBuffer;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(QSerialPort::Directions)

class QSerialPortInfo
{
public:
    QSerialPortInfo() = default;
    explicit QSerialPortInfo(const QSerialPort &port) : m_portName(port.portName()) {}
    explicit QSerialPortInfo(const QString &name) : m_portName(name) {}

    static QList<QSerialPortInfo> availablePorts();

    QString portName() const { return m_portName; }
    QString systemLocation() const { return m_portName; }
    QString description() const { return m_description; }
    QString manufacturer() const { return m_manufacturer; }
    QString serialNumber() const { return m_serialNumber; }
    bool hasVendorIdentifier() const { return m_vendorId >= 0; }
    quint16 vendorIdentifier() const { return quint16(m_vendorId < 0 ? 0 : m_vendorId); }
    bool hasProductIdentifier() const { return m_productId >= 0; }
    quint16 productIdentifier() const { return quint16(m_productId < 0 ? 0 : m_productId); }
    bool isNull() const { return m_portName.isEmpty(); }

private:
    friend class QSerialPort;
    QString m_portName;
    QString m_description;
    QString m_manufacturer;
    QString m_serialNumber;
    int m_vendorId = -1;
    int m_productId = -1;
};
