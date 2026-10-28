// A WebSerialBackend for tests: records what the port asks for; the test answers through WebSerialHub::deliver*().
// With autoOpen, open() answers "opened" at once (for a port in portList, else "not available").
#pragma once

#include "webserialport.h"

class FakeBackend : public WebSerialBackend
{
public:
    struct Call
    {
        QString what; // "open", "write", "signals", "close", "request"
        int handle = 0;
        QString name;
        QByteArray bytes;
        qint32 baudRate = 0;
        bool hardwareFlowControl = false;
        bool dtr = false, rts = false, any = false;
    };

    bool supported = true;
    bool autoOpen = false;
    QList<QSerialPortInfo> portList;
    QList<Call> calls;

    void reset()
    {
        supported = true;
        autoOpen = false;
        portList.clear();
        calls.clear();
    }

    bool isSupported() const override { return supported; }
    QList<QSerialPortInfo> ports() const override { return portList; }

    void open(int handle, const QString &name, qint32 baudRate, bool hardwareFlowControl) override
    {
        Call c{QStringLiteral("open"), handle, name};
        c.baudRate = baudRate;
        c.hardwareFlowControl = hardwareFlowControl;
        calls.append(c);
        if (!autoOpen) return;
        bool known = false;
        for (const auto &p : portList) known |= p.portName() == name;
        if (known) WebSerialHub::deliverOpened(handle, true, 0, QString());
        else WebSerialHub::deliverOpened(handle, false, QSerialPort::DeviceNotFoundError, name + " is not available");
    }
    void write(int handle, const QByteArray &bytes) override
    {
        Call c{QStringLiteral("write"), handle};
        c.bytes = bytes;
        calls.append(c);
    }
    void setSignals(int handle, bool dtr, bool rts) override
    {
        Call c{QStringLiteral("signals"), handle};
        c.dtr = dtr;
        c.rts = rts;
        calls.append(c);
    }
    void close(int handle) override { calls.append(Call{QStringLiteral("close"), handle}); }
    void requestPort(bool anyPort) override
    {
        Call c{QStringLiteral("request")};
        c.any = anyPort;
        calls.append(c);
    }

    QList<Call> callsOf(const QString &what) const
    {
        QList<Call> out;
        for (const auto &c : calls)
            if (c.what == what) out.append(c);
        return out;
    }

    static QSerialPortInfo rc1() { return QSerialPortInfo("WebSerial 1209:0001", "resolve-can (rc1)"); }
};
