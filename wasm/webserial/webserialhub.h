// web-savvycan: between the QSerialPort stand-in and the browser.
//
// WebSerialBackend is what talks to the browser (webserialbackend_wasm.cpp + webserial.js in the wasm build; tests
// install their own). The backend reports back through WebSerialHub's static deliver*() functions, from any call
// stack (a JS promise callback, a test): each report is queued, in order, to the port's thread's event loop (in the
// browser there is one thread; desktop tests run SavvyCAN's connections in their QThreads). A port that was closed
// (or reopened: new handle) or deleted gets nothing more. Reports never run app code inside a JS callback, so a slot
// may block (QDialog::exec under Asyncify) without unwinding through the browser's stack.
// Data for one port is merged while it waits for the event loop: one readyRead per batch, as with Qt's QSerialPort.
#pragma once

#include <memory>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

#include "webserialport.h"

struct WebSerialPortDescription
{
    QString name;         // stable while plugged in: "WebSerial 1209:0001", "WebSerial 1209:0001 #2", "WebSerial #1"
    QString description;  // "resolve-can (rc1)", "USB serial port", "Serial port"
    quint16 vid = 0;
    quint16 pid = 0;
    bool hasUsbIds = false;
};

struct WebSerialOpenOptions
{
    qint32 baudRate = 115200;
    int dataBits = 8;      // 7 or 8 (Web Serial)
    int stopBits = 1;      // 1 or 2
    QString parity;        // "none", "even", "odd"
    QString flowControl;   // "none", "hardware"
};

class WebSerialBackend
{
public:
    virtual ~WebSerialBackend() = default;
    virtual bool isSupported() const = 0;                       // navigator.serial exists
    virtual QList<WebSerialPortDescription> ports() const = 0;  // granted and plugged in, as last seen
    // Open asynchronously; answer with WebSerialHub::deliverOpened(handle, ...).
    virtual void open(int handle, const QString &name, const WebSerialOpenOptions &options) = 0;
    // Bytes for an open port; answer with deliverWritten() (or deliverError(WriteError)).
    virtual void write(int handle, const QByteArray &bytes) = 0;
    virtual void setSignals(int handle, bool dtr, bool rts, bool brk) = 0;
    // Close asynchronously; no answer needed (the hub has forgotten the handle already).
    virtual void close(int handle) = 0;
    // Ask the user for a port (browser chooser); answer with deliverPortChosen().
    virtual void requestPort(bool anyPort) = 0;
};

class WebSerialHub : public QObject
{
    Q_OBJECT

public:
    static WebSerialHub *instance();

    // The backend in use (the wasm build installs the browser one at start-up). Not owned.
    static void setBackend(WebSerialBackend *backend);
    static WebSerialBackend *backend();

    // Shows the browser's port chooser. Must run while handling a user's click or key press (Chrome's rule).
    // anyPort false: only resolve-can devices; true: every serial port. Answer: portChosen().
    static void requestPort(bool anyPort);
    static bool isSupported();

    // --- reports from the backend (any call stack; queued into the Qt event loop, in order) ---
    static void deliverOpened(int handle, bool ok, int serialPortError, const QString &message);
    static void deliverData(int handle, const QByteArray &bytes);
    static void deliverWritten(int handle, qint64 bytes);
    static void deliverError(int handle, int serialPortError, const QString &message);
    static void deliverReadError(int handle, const QString &message); // recoverable, the stream goes on
    static void deliverPortsChanged();
    static void deliverPortChosen(const QString &name, const QString &error);

    // Hands every queued report of this thread over now (tests; the event loop does it otherwise).
    static void processPending();

    // --- for QSerialPort / QSerialPortInfo ---
    int registerPort(QSerialPort *port);
    void unregisterPort(int handle);
    QSerialPort *port(int handle) const;
    static QList<QSerialPortInfo> availablePorts();

signals:
    void portsChanged();
    // The user chose a port (name) or didn't (name empty; error says why, empty when they just cancelled).
    void portChosen(const QString &name, const QString &error);

private:
    explicit WebSerialHub(QObject *parent = nullptr);
    template <typename F> static void toPort(int handle, F &&deliver);

    static WebSerialBackend *s_backend;
    mutable QMutex m_mutex;                      // m_ports, m_openData: reports may come from any thread
    QHash<int, QPointer<QSerialPort>> m_ports;
    QHash<int, std::shared_ptr<QByteArray>> m_openData; // data posted to a port and not yet taken: appended to
    int m_nextHandle = 1;
};
