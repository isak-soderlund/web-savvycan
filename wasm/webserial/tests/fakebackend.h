// A WebSerialBackend for tests: records what the port asks for; the test answers through WebSerialHub::deliver*().
// With autoOpen, open() answers "opened" at once; `device` (if set) gets every write, like a device on the wire.
#pragma once

#include <functional>

#include <QMutex>
#include <QMutexLocker>

#include "webserialhub.h"

class FakeBackend : public WebSerialBackend
{
public:
    struct Call
    {
        QString what; // "open", "write", "signals", "close", "request"
        int handle = 0;
        QString name;
        QByteArray bytes;
        WebSerialOpenOptions options;
        bool dtr = false, rts = false, brk = false, any = false;
    };

    bool supported = true;
    bool autoOpen = false;
    QList<WebSerialPortDescription> portList;
    QList<Call> calls; // lock `mutex` when another thread may be calling (SavvyCAN's connection threads)
    mutable QMutex mutex;
    std::function<void(int handle, const QByteArray &bytes)> device;

    void reset()
    {
        QMutexLocker lock(&mutex);
        supported = true;
        autoOpen = false;
        portList.clear();
        calls.clear();
        device = nullptr;
    }
    void record(const Call &c)
    {
        QMutexLocker lock(&mutex);
        calls.append(c);
    }

    bool isSupported() const override { return supported; }
    QList<WebSerialPortDescription> ports() const override { return portList; }

    void open(int handle, const QString &name, const WebSerialOpenOptions &options) override
    {
        Call c{QStringLiteral("open"), handle, name};
        c.options = options;
        record(c);
        if (autoOpen)
        {
            bool known = false;
            for (const auto &p : portList) known |= p.name == name;
            if (known) WebSerialHub::deliverOpened(handle, true, 0, QString());
            else WebSerialHub::deliverOpened(handle, false, 1, name + QStringLiteral(" is not available"));
        }
    }
    void write(int handle, const QByteArray &bytes) override
    {
        Call c{QStringLiteral("write"), handle};
        c.bytes = bytes;
        record(c);
        WebSerialHub::deliverWritten(handle, bytes.size());
        if (device) device(handle, bytes);
    }
    void setSignals(int handle, bool dtr, bool rts, bool brk) override
    {
        Call c{QStringLiteral("signals"), handle};
        c.dtr = dtr;
        c.rts = rts;
        c.brk = brk;
        record(c);
    }
    void close(int handle) override { record(Call{QStringLiteral("close"), handle}); }
    void requestPort(bool anyPort) override
    {
        Call c{QStringLiteral("request")};
        c.any = anyPort;
        record(c);
    }

    QList<Call> callsOf(const QString &what) const
    {
        QMutexLocker lock(&mutex);
        QList<Call> out;
        for (const auto &c : calls)
            if (c.what == what) out.append(c);
        return out;
    }
    QByteArray written(int handle) const
    {
        QMutexLocker lock(&mutex);
        QByteArray all;
        for (const auto &c : calls)
            if (c.what == QLatin1String("write") && c.handle == handle) all += c.bytes;
        return all;
    }

    static WebSerialPortDescription rc1(const QString &name = QStringLiteral("WebSerial 1209:0001"))
    {
        WebSerialPortDescription d;
        d.name = name;
        d.description = QStringLiteral("resolve-can (rc1)");
        d.vid = 0x1209;
        d.pid = 0x0001;
        d.hasUsbIds = true;
        return d;
    }
};
