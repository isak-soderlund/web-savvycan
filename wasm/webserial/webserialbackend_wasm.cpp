// web-savvycan: the browser backend of the QSerialPort stand-in (wasm build only). The JS half is webserial.js,
// linked with --pre-js; C++ calls it through EM_JS, it answers through the embind functions below.
#include "webserialhub.h"

#include <QCoreApplication>
#include <QString>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

// --- C++ -> JS ------------------------------------------------------------------------------------------------

EM_JS(int, wsjs_supported, (), { return webSerialBridge.supported ? 1 : 0; });

EM_JS(void, wsjs_open, (int handle, const char *name, int baud, int dataBits, int stopBits, const char *parity,
                        const char *flow), {
    webSerialBridge.open(handle, UTF8ToString(name), {
        baudRate: baud, dataBits: dataBits, stopBits: stopBits,
        parity: UTF8ToString(parity), flowControl: UTF8ToString(flow)
    });
});

// Copies the bytes out of wasm memory at once (the C++ buffer is gone after the call).
EM_JS(void, wsjs_write, (int handle, const char *data, int len), {
    webSerialBridge.write(handle, HEAPU8.slice(data, data + len));
});

EM_JS(void, wsjs_set_signals, (int handle, int dtr, int rts, int brk), {
    webSerialBridge.setSignals(handle, dtr != 0, rts != 0, brk != 0);
});

EM_JS(void, wsjs_close, (int handle), { webSerialBridge.close(handle); });

EM_JS(void, wsjs_request_port, (int anyPort), { webSerialBridge.requestPort(anyPort != 0); });

namespace {

class BrowserBackend final : public WebSerialBackend
{
public:
    bool isSupported() const override { return wsjs_supported() != 0; }

    QList<WebSerialPortDescription> ports() const override
    {
        QList<WebSerialPortDescription> list;
        emscripten::val bridge = emscripten::val::global("resolveWebSerial");
        if (bridge.isUndefined()) return list;
        emscripten::val arr = bridge.call<emscripten::val>("ports");
        const int n = arr["length"].as<int>();
        for (int i = 0; i < n; ++i)
        {
            emscripten::val e = arr[i];
            WebSerialPortDescription d;
            d.name = QString::fromStdString(e["name"].as<std::string>());
            d.description = QString::fromStdString(e["description"].as<std::string>());
            d.vid = quint16(e["vid"].as<int>());
            d.pid = quint16(e["pid"].as<int>());
            d.hasUsbIds = e["hasUsbIds"].as<bool>();
            list.append(d);
        }
        return list;
    }

    void open(int handle, const QString &name, const WebSerialOpenOptions &o) override
    {
        wsjs_open(handle, name.toUtf8().constData(), o.baudRate, o.dataBits, o.stopBits,
                  o.parity.toUtf8().constData(), o.flowControl.toUtf8().constData());
    }

    void write(int handle, const QByteArray &bytes) override
    {
        wsjs_write(handle, bytes.constData(), int(bytes.size()));
    }

    void setSignals(int handle, bool dtr, bool rts, bool brk) override
    {
        wsjs_set_signals(handle, dtr, rts, brk);
    }

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

QString toQString(const emscripten::val &v)
{
    return v.isString() ? QString::fromStdString(v.as<std::string>()) : QString();
}

void webSerialOpened(int handle, bool ok, int error, emscripten::val message)
{
    WebSerialHub::deliverOpened(handle, ok, error, toQString(message));
}

void webSerialData(int handle, emscripten::val chunk)
{
    // One copy, straight into the QByteArray: a typed-array view of its bytes, filled by Uint8Array.set().
    const unsigned len = chunk["length"].as<unsigned>();
    if (!len) return;
    QByteArray bytes(qsizetype(len), Qt::Uninitialized);
    emscripten::val view(emscripten::typed_memory_view(len, reinterpret_cast<unsigned char *>(bytes.data())));
    view.call<void>("set", chunk);
    WebSerialHub::deliverData(handle, bytes);
}

void webSerialWritten(int handle, double bytes) { WebSerialHub::deliverWritten(handle, qint64(bytes)); }

void webSerialError(int handle, int error, emscripten::val message)
{
    WebSerialHub::deliverError(handle, error, toQString(message));
}

void webSerialReadError(int handle, emscripten::val message)
{
    WebSerialHub::deliverReadError(handle, toQString(message));
}

void webSerialPortsChanged() { WebSerialHub::deliverPortsChanged(); }

void webSerialPortChosen(emscripten::val name, emscripten::val error)
{
    WebSerialHub::deliverPortChosen(toQString(name), toQString(error));
}

} // namespace

EMSCRIPTEN_BINDINGS(web_savvycan_webserial)
{
    emscripten::function("webSerialOpened", &webSerialOpened);
    emscripten::function("webSerialData", &webSerialData);
    emscripten::function("webSerialWritten", &webSerialWritten);
    emscripten::function("webSerialError", &webSerialError);
    emscripten::function("webSerialReadError", &webSerialReadError);
    emscripten::function("webSerialPortsChanged", &webSerialPortsChanged);
    emscripten::function("webSerialPortChosen", &webSerialPortChosen);
}
