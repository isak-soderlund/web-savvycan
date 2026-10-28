// web-savvycan: a QSerialPort / QSerialPortInfo stand-in backed by Web Serial, for the WebAssembly build.
//
// Qt Serial Port does not build for Emscripten, so the wasm build puts wasm/webserial/include first on the include
// path: <QSerialPort> and <QSerialPortInfo> land here. Only what SavvyCAN uses (GVRetSerial, LAWICELSerial,
// NewConnectionDialog), with Qt's names and enum values, so that code stays as it is.
//
// How it differs from Qt's QSerialPort (Web Serial is asynchronous, the page has one thread):
//  - open() returns true at once; the port really opens a moment later. Writes and DTR/RTS changes made meanwhile
//    are sent once it is open. If it can't be opened, errorOccurred() comes from the event loop
//    (DeviceNotFoundError, PermissionError or OpenError) and the device is closed again.
//  - Baud rate and flow control are taken at open(); always 8 data bits, no parity, 1 stop bit.
//  - A lost device is ResourceError, as in Qt; recoverable read errors (overrun, framing) are only logged by the
//    browser side: the stream goes on.
//
// The browser side is webserialbackend_wasm.cpp + webserial.js; tests install their own WebSerialBackend.
#pragma once

#include <QByteArray>
#include <QHash>
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
    enum DataBits { Data5 = 5, Data6 = 6, Data7 = 7, Data8 = 8 };
    enum FlowControl { NoFlowControl, HardwareControl, SoftwareControl };
    enum SerialPortError { NoError = 0, DeviceNotFoundError = 1, PermissionError = 2, OpenError = 3, WriteError = 7,
                           ReadError = 8, ResourceError = 9, UnsupportedOperationError = 10, UnknownError = 11,
                           TimeoutError = 12, NotOpenError = 13 };
    Q_ENUM(SerialPortError)

    explicit QSerialPort(const QString &name, QObject *parent = nullptr);
    explicit QSerialPort(const QSerialPortInfo &info, QObject *parent = nullptr);
    ~QSerialPort() override;

    QString portName() const { return m_portName; }

    bool open(OpenMode mode) override;
    void close() override;

    bool setBaudRate(qint32 baudRate);
    bool setDataBits(DataBits) { return true; } // SavvyCAN only sets Data8
    bool setFlowControl(FlowControl flowControl);
    bool setDataTerminalReady(bool set);
    bool setRequestToSend(bool set);

    SerialPortError error() const { return m_error; }
    void clearError() { setError(NoError); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_rx.size() + QIODevice::bytesAvailable(); }

    int handle() const { return m_handle; } // the id the backend knows the open port by; 0 when closed

signals:
    void errorOccurred(QSerialPort::SerialPortError error);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    friend class WebSerialHub;

    void opened(bool ok, SerialPortError err, const QString &message);
    void setError(SerialPortError err, const QString &message = QString());
    void sendSignals();
    void releaseHandle();

    QString m_portName;
    int m_handle = 0;
    bool m_opening = false;      // open() returned true, the browser is still at it
    QByteArray m_rx;
    bool m_readyReadQueued = false;
    QByteArray m_pendingTx;      // written while opening
    bool m_signalsPending = false;
    qint32 m_baudRate = 115200;
    FlowControl m_flowControl = NoFlowControl;
    bool m_dtr = false;
    bool m_rts = false;
    SerialPortError m_error = NoError;
};

class QSerialPortInfo
{
public:
    QSerialPortInfo(const QString &name, const QString &description) : m_name(name), m_description(description) {}
    explicit QSerialPortInfo(const QString &name); // the available port of that name, or just the name

    QString portName() const { return m_name; }
    QString description() const { return m_description; }

    // The ports this page may open: the ones the user granted (now or on an earlier visit) that are plugged in.
    static QList<QSerialPortInfo> availablePorts();

private:
    QString m_name;        // stable while plugged in: "WebSerial 1209:0001", "WebSerial 1209:0001 #2", "WebSerial #1"
    QString m_description; // "resolve-can (rc1)", "USB serial port", "Serial port"
};

// What talks to the browser (webserialbackend_wasm.cpp + webserial.js; tests install a fake). It answers through
// WebSerialHub's deliver*() functions.
class WebSerialBackend
{
public:
    virtual ~WebSerialBackend() = default;
    virtual bool isSupported() const = 0;                 // navigator.serial exists
    virtual QList<QSerialPortInfo> ports() const = 0;     // granted and plugged in, as last seen
    virtual void open(int handle, const QString &name, qint32 baudRate, bool hardwareFlowControl) = 0; // deliverOpened
    virtual void write(int handle, const QByteArray &bytes) = 0;   // failure: deliverError(WriteError)
    virtual void setSignals(int handle, bool dtr, bool rts) = 0;
    virtual void close(int handle) = 0;                   // no answer: the handle is forgotten already
    virtual void requestPort(bool anyPort) = 0;           // the browser's chooser; deliverPortChosen
};

// The backend's reports come in through deliver*(), from any call stack of the page's one thread (a JS promise
// callback, a test), and reach the ports and the dialog from Qt's event loop, in order: app code never runs inside
// a JS callback, so a slot may block (QDialog::exec under Asyncify). A closed (or reopened: new handle) or deleted
// port gets nothing more. Data goes into the port's buffer at once, with one readyRead for what came meanwhile.
class WebSerialHub : public QObject
{
    Q_OBJECT

public:
    static WebSerialHub *instance();

    static void setBackend(WebSerialBackend *backend) { s_backend = backend; } // not owned
    static WebSerialBackend *backend() { return s_backend; }
    static bool isSupported() { return s_backend && s_backend->isSupported(); }

    // Shows the browser's port chooser. Must run while handling a user's click or key press (Chrome's rule).
    // anyPort false: only resolve-can devices; true: every serial port. Answer: portChosen().
    static void requestPort(bool anyPort);

    static void deliverOpened(int handle, bool ok, int serialPortError, const QString &message);
    static void deliverData(int handle, const QByteArray &bytes);
    static void deliverError(int handle, int serialPortError, const QString &message);
    static void deliverPortsChanged();
    static void deliverPortChosen(const QString &name, const QString &error);

signals:
    void portsChanged();
    // The user chose a port (name) or didn't (name empty; error says why, empty when they just cancelled).
    void portChosen(const QString &name, const QString &error);

private:
    friend class QSerialPort;
    template <typename F> static void later(int handle, F deliver);

    static inline WebSerialBackend *s_backend = nullptr;
    QHash<int, QSerialPort *> m_ports; // open ports by handle
    int m_nextHandle = 1;
};
