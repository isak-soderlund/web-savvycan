// web-savvycan: the browser backend of the QSerialPort stand-in (wasm build only). The JS half is webserial.js
// (globalThis.resolveWebSerial), linked with --pre-js; C++ calls it through EM_JS, it answers through the embind
// functions below.
#include "webserialport.h"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

using emscripten::val;

// --- C++ -> JS ------------------------------------------------------------------------------------------------

EM_JS(int, wsjs_supported, (), { return resolveWebSerial.supported ? 1 : 0; });

EM_JS(void, wsjs_open, (int handle, const char *name, int baud, int hardwareFlow), {
    resolveWebSerial.open(handle, UTF8ToString(name), { baudRate: baud, flowControl: hardwareFlow ? 'hardware' : 'none' });
});

// Copies the bytes out of wasm memory at once (the C++ buffer is gone after the call).
EM_JS(void, wsjs_write, (int handle, const char *data, int len), {
    resolveWebSerial.write(handle, HEAPU8.slice(data, data + len));
});

EM_JS(void, wsjs_set_signals, (int handle, int dtr, int rts), { resolveWebSerial.setSignals(handle, dtr != 0, rts != 0); });

EM_JS(void, wsjs_close, (int handle), { resolveWebSerial.close(handle); });

EM_JS(void, wsjs_request_port, (int anyPort), { resolveWebSerial.requestPort(anyPort != 0); });

namespace {

QString toQString(const val &v) { return v.isString() ? QString::fromStdString(v.as<std::string>()) : QString(); }

class BrowserBackend final : public WebSerialBackend
{
public:
    bool isSupported() const override { return wsjs_supported() != 0; }

    QList<QSerialPortInfo> ports() const override
    {
        QList<QSerialPortInfo> list;
        val arr = val::global("resolveWebSerial").call<val>("ports");
        const int n = arr["length"].as<int>();
        for (int i = 0; i < n; ++i) list.append(QSerialPortInfo(toQString(arr[i]["name"]), toQString(arr[i]["description"])));
        return list;
    }

    void open(int handle, const QString &name, qint32 baudRate, bool hardwareFlowControl) override
    {
        wsjs_open(handle, name.toUtf8().constData(), baudRate, hardwareFlowControl);
    }
    void write(int handle, const QByteArray &bytes) override { wsjs_write(handle, bytes.constData(), int(bytes.size())); }
    void setSignals(int handle, bool dtr, bool rts) override { wsjs_set_signals(handle, dtr, rts); }
    void close(int handle) override { wsjs_close(handle); }
    void requestPort(bool anyPort) override { wsjs_request_port(anyPort); }
};

BrowserBackend s_browserBackend;

// Installed before main(): the restored connections of an earlier visit open their port at start-up.
struct Installer
{
    Installer() { WebSerialHub::setBackend(&s_browserBackend); }
} s_installer;

// --- JS -> C++ ------------------------------------------------------------------------------------------------

void webSerialOpened(int handle, bool ok, int error, val message)
{
    WebSerialHub::deliverOpened(handle, ok, error, toQString(message));
}

void webSerialData(int handle, val chunk)
{
    // One copy, straight into the QByteArray: a typed-array view of its bytes, filled by Uint8Array.set().
    const unsigned len = chunk["length"].as<unsigned>();
    if (!len) return;
    QByteArray bytes(qsizetype(len), Qt::Uninitialized);
    val view(emscripten::typed_memory_view(len, reinterpret_cast<unsigned char *>(bytes.data())));
    view.call<void>("set", chunk);
    WebSerialHub::deliverData(handle, bytes);
}

void webSerialError(int handle, int error, val message) { WebSerialHub::deliverError(handle, error, toQString(message)); }

void webSerialPortsChanged() { WebSerialHub::deliverPortsChanged(); }

void webSerialPortChosen(val name, val error) { WebSerialHub::deliverPortChosen(toQString(name), toQString(error)); }

} // namespace

EMSCRIPTEN_BINDINGS(web_savvycan_webserial)
{
    emscripten::function("webSerialOpened", &webSerialOpened);
    emscripten::function("webSerialData", &webSerialData);
    emscripten::function("webSerialError", &webSerialError);
    emscripten::function("webSerialPortsChanged", &webSerialPortsChanged);
    emscripten::function("webSerialPortChosen", &webSerialPortChosen);
}
