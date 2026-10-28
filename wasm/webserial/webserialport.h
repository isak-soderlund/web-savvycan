// web-savvycan: a QSerialPort / QSerialPortInfo stand-in backed by Web Serial, for the WebAssembly build.
//
// Qt Serial Port does not build for Emscripten, so the wasm build puts wasm/webserial/include first on the include
// path: <QSerialPort> and <QSerialPortInfo> land here. The API is the subset SavvyCAN uses (GVRetSerial,
// LAWICELSerial, NewConnectionDialog), with Qt's names, enum values and signals, so that code stays as it is.
//
// How it differs from Qt's QSerialPort (Web Serial is asynchronous, the page has one thread):
//  - open() checks nothing it cannot check at once and returns true; the port really opens a moment later. Writes and
//    DTR/RTS changes made meanwhile are queued and sent once it is open. If it can't be opened, errorOccurred() comes
//    from the event loop (DeviceNotFoundError, PermissionError or OpenError) and the device is closed again.
//  - Baud rate, data bits, parity, stop bits and flow control are taken at open(); changing them while open is
//    remembered but only used at the next open() (Web Serial can't change them on an open port).
//  - flush() hands nothing over (writes go to the browser as they are made) and returns true; waitForReadyRead() and
//    waitForBytesWritten() can't block on the page and return false.
//  - Recoverable read errors (buffer overrun, framing, parity, break) are counted (readErrorCount()) and logged, not
//    reported as errorOccurred(): the stream goes on. A lost device is ResourceError, as in Qt.
//
// The browser side (wasm) is webserialbackend_wasm.cpp + webserial.js; tests install their own WebSerialBackend.
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QObject>
#include <QString>

class QSerialPortInfo;

class QSerialPort : public QIODevice
{
    Q_OBJECT

public:
    // Values as in Qt 6's QSerialPort.
    enum Direction { Input = 1, Output = 2, AllDirections = Input | Output };
    Q_DECLARE_FLAGS(Directions, Direction)
    Q_FLAG(Directions)

    enum BaudRate { Baud1200 = 1200, Baud2400 = 2400, Baud4800 = 4800, Baud9600 = 9600, Baud19200 = 19200,
                    Baud38400 = 38400, Baud57600 = 57600, Baud115200 = 115200 };
    Q_ENUM(BaudRate)

    enum DataBits { Data5 = 5, Data6 = 6, Data7 = 7, Data8 = 8 };
    Q_ENUM(DataBits)

    enum Parity { NoParity = 0, EvenParity = 2, OddParity = 3, SpaceParity = 4, MarkParity = 5 };
    Q_ENUM(Parity)

    enum StopBits { OneStop = 1, OneAndHalfStop = 3, TwoStop = 2 };
    Q_ENUM(StopBits)

    enum FlowControl { NoFlowControl, HardwareControl, SoftwareControl };
    Q_ENUM(FlowControl)

    enum PinoutSignal {
        NoSignal = 0x00, TransmittedDataSignal = 0x01, ReceivedDataSignal = 0x02, DataTerminalReadySignal = 0x04,
        DataCarrierDetectSignal = 0x08, DataSetReadySignal = 0x10, RingIndicatorSignal = 0x20,
        RequestToSendSignal = 0x40, ClearToSendSignal = 0x80, SecondaryTransmittedDataSignal = 0x100,
        SecondaryReceivedDataSignal = 0x200
    };
    Q_DECLARE_FLAGS(PinoutSignals, PinoutSignal)
    Q_FLAG(PinoutSignals)

    enum SerialPortError { NoError = 0, DeviceNotFoundError = 1, PermissionError = 2, OpenError = 3, WriteError = 7,
                           ReadError = 8, ResourceError = 9, UnsupportedOperationError = 10, UnknownError = 11,
                           TimeoutError = 12, NotOpenError = 13 };
    Q_ENUM(SerialPortError)

    explicit QSerialPort(QObject *parent = nullptr);
    explicit QSerialPort(const QString &name, QObject *parent = nullptr);
    explicit QSerialPort(const QSerialPortInfo &info, QObject *parent = nullptr);
    ~QSerialPort() override;

    void setPortName(const QString &name);
    QString portName() const { return m_portName; }
    void setPort(const QSerialPortInfo &info);

    bool open(OpenMode mode) override;
    void close() override;

    bool setBaudRate(qint32 baudRate, Directions directions = AllDirections);
    qint32 baudRate(Directions directions = AllDirections) const;
    bool setDataBits(DataBits dataBits);
    DataBits dataBits() const { return m_dataBits; }
    bool setParity(Parity parity);
    Parity parity() const { return m_parity; }
    bool setStopBits(StopBits stopBits);
    StopBits stopBits() const { return m_stopBits; }
    bool setFlowControl(FlowControl flowControl);
    FlowControl flowControl() const { return m_flowControl; }

    bool setDataTerminalReady(bool set);
    bool isDataTerminalReady() { return m_dtr; }
    bool setRequestToSend(bool set);
    bool isRequestToSend() { return m_rts; }
    PinoutSignals pinoutSignals();
    bool setBreakEnabled(bool set = true);
    bool isBreakEnabled() const { return m_break; }

    bool flush();
    bool clear(Directions directions = AllDirections);

    SerialPortError error() const { return m_error; }
    void clearError();

    qint64 readBufferSize() const { return m_readBufferSize; }
    void setReadBufferSize(qint64 size) { m_readBufferSize = size; }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override;
    bool canReadLine() const override;
    bool waitForReadyRead(int msecs = 30000) override;
    bool waitForBytesWritten(int msecs = 30000) override;

    // web-savvycan extras
    bool isOpening() const { return m_state == State::Opening; } // open() returned true, the browser is still at it
    quint64 readErrorCount() const { return m_readErrors; }      // recoverable read errors (overrun, framing, ...)
    int handle() const { return m_handle; }                     // id shared with the backend; 0 when closed

signals:
    void baudRateChanged(qint32 baudRate, QSerialPort::Directions directions);
    void dataBitsChanged(QSerialPort::DataBits dataBits);
    void parityChanged(QSerialPort::Parity parity);
    void stopBitsChanged(QSerialPort::StopBits stopBits);
    void flowControlChanged(QSerialPort::FlowControl flowControl);
    void dataTerminalReadyChanged(bool set);
    void requestToSendChanged(bool set);
    void breakEnabledChanged(bool set);
    void errorOccurred(QSerialPort::SerialPortError error);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 readLineData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    friend class WebSerialHub;
    enum class State { Closed, Opening, Open };

    // Called by WebSerialHub, from the Qt event loop.
    void hubOpened(bool ok, SerialPortError err, const QString &message);
    void hubData(const QByteArray &bytes);
    void hubWritten(qint64 bytes);
    void hubError(SerialPortError err, const QString &message);
    void hubReadError(const QString &message);

    void setError(SerialPortError err, const QString &message = QString());
    void sendSignals();
    void releaseHandle();

    QString m_portName;
    State m_state = State::Closed;
    int m_handle = 0;
    QByteArray m_rx;
    QByteArray m_pendingTx;      // written while opening
    bool m_signalsPending = false;
    qint64 m_bytesToWrite = 0;   // handed to the browser, not yet confirmed
    qint64 m_readBufferSize = 0; // accepted for API compatibility; the browser buffer is set by the backend
    quint64 m_readErrors = 0;
    qint32 m_baudRate = Baud115200;
    DataBits m_dataBits = Data8;
    Parity m_parity = NoParity;
    StopBits m_stopBits = OneStop;
    FlowControl m_flowControl = NoFlowControl;
    bool m_dtr = false;
    bool m_rts = false;
    bool m_break = false;
    SerialPortError m_error = NoError;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(QSerialPort::Directions)
Q_DECLARE_OPERATORS_FOR_FLAGS(QSerialPort::PinoutSignals)

class QSerialPortInfo
{
public:
    QSerialPortInfo() = default;
    explicit QSerialPortInfo(const QString &name);
    explicit QSerialPortInfo(const QSerialPort &port);

    QString portName() const { return m_name; }
    QString systemLocation() const { return m_name; }
    QString description() const { return m_description; }
    QString manufacturer() const { return m_manufacturer; }
    QString serialNumber() const { return QString(); } // Web Serial does not tell
    quint16 vendorIdentifier() const { return m_vid; }
    quint16 productIdentifier() const { return m_pid; }
    bool hasVendorIdentifier() const { return m_hasUsbIds; }
    bool hasProductIdentifier() const { return m_hasUsbIds; }
    bool isNull() const { return m_name.isEmpty(); }
    void swap(QSerialPortInfo &other) noexcept;

    // The ports this page may open: the ones the user granted (now or on an earlier visit) that are plugged in.
    static QList<QSerialPortInfo> availablePorts();
    static QList<qint32> standardBaudRates();

private:
    friend class WebSerialHub;
    QString m_name;
    QString m_description;
    QString m_manufacturer;
    quint16 m_vid = 0;
    quint16 m_pid = 0;
    bool m_hasUsbIds = false;
};
