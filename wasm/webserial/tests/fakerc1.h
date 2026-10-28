// A fake resolve-can rc1 on the serial port, for desktop tests: starts in LAWICEL/slcan, switches to GVRET on
// 0xE7 0xE7 (firmware/proto/src/serial.rs), answers SavvyCAN's GVRET queries as resolve-can's test/hil/gvret_fake.py
// does (one bus, CAN FD, the bus closed at the start of a session). busFrame() puts a frame "on the bus": it reaches
// the host if the bus is open. Thread-safe: SavvyCAN's connections write from their own thread.
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QtEndian>

#include "webserialhub.h"

class FakeRc1
{
public:
    QStringList log; // "setup bus0 enabled=1 ...", "slcan O", "tx 123 0102"

    FakeRc1() { clock.start(); }

    // Bytes from the host (the backend's write()).
    void fromHost(int handle, const QByteArray &bytes)
    {
        QMutexLocker lock(&mutex);
        this->handle = handle;
        buf += bytes;
        while (!buf.isEmpty())
        {
            const int before = buf.size();
            if (gvret) parseGvret();
            else parseSlcan();
            if (buf.size() == before) break; // waits for more bytes
        }
    }

    void busFrame(quint32 id, const QByteArray &data, bool extended = false, bool fd = false, bool brs = false)
    {
        QMutexLocker lock(&mutex);
        if (!enabled || !handle) return;
        QByteArray out;
        if (gvret)
        {
            out.append(char(0xF1));
            out.append(char(fd ? 20 : 0));
            appendLe32(out, us());
            appendLe32(out, id | (extended ? 0x80000000u : 0));
            if (fd)
            {
                out.append(char(data.size() | (brs ? 0x80 : 0)));
                out.append(char(0));
            }
            else
                out.append(char(data.size()));
            out += data;
            out.append(char(0));
        }
        else
        {
            out += extended ? QByteArray("T") + QByteArray::number(id, 16).rightJustified(8, '0').toUpper()
                            : QByteArray("t") + QByteArray::number(id, 16).rightJustified(3, '0').toUpper();
            out += QByteArray::number(int(data.size()));
            out += data.toHex().toUpper();
            out += '\r';
        }
        WebSerialHub::deliverData(handle, out);
    }

    bool isGvret() { QMutexLocker lock(&mutex); return gvret; }
    bool isEnabled() { QMutexLocker lock(&mutex); return enabled; }
    QStringList logCopy() { QMutexLocker lock(&mutex); return log; }

private:
    QMutex mutex;
    QElapsedTimer clock;
    QByteArray buf;
    int handle = 0;
    bool gvret = false;
    bool enabled = false, listenOnly = false, fd = false;
    quint32 bitrate = 500000, dataBitrate = 2000000;

    quint32 us() const { return quint32(clock.nsecsElapsed() / 1000); }
    static void appendLe32(QByteArray &b, quint32 v)
    {
        for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
    }
    static quint32 le32(const QByteArray &b, int at)
    {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + at));
    }
    void send(const QByteArray &b) { WebSerialHub::deliverData(handle, b); }

    void parseSlcan()
    {
        // 0xE7 0xE7 anywhere at a command start: GVRET from here on.
        if (buf.startsWith("\xE7\xE7"))
        {
            buf.remove(0, 2);
            gvret = true;
            enabled = false;
            log << QStringLiteral("gvret");
            return;
        }
        if (buf.size() == 1 && uchar(buf[0]) == 0xE7) return;
        const int cr = buf.indexOf('\r');
        if (cr < 0) return;
        const QByteArray cmd = buf.left(cr);
        buf.remove(0, cr + 1);
        log << QStringLiteral("slcan ") + QString::fromLatin1(cmd);
        bool ok = true;
        switch (cmd.isEmpty() ? 0 : cmd[0])
        {
        case 'O': enabled = true; listenOnly = false; break;
        case 'L': enabled = true; listenOnly = true; break;
        case 'C': enabled = false; break;
        case 'S': {
            static const quint32 rates[] = {10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
            const int i = cmd.mid(1).toInt();
            ok = !enabled && i >= 0 && i <= 8;
            if (ok) bitrate = rates[i];
            break;
        }
        case 'Y': ok = !enabled; fd = true; dataBitrate = cmd.mid(1).toUInt() * 1000000; break;
        case 'V': send("V0103\r"); return;
        case 't': case 'T': case 'd': case 'D': case 'b': case 'B': ok = enabled && !listenOnly; break;
        case 0: break;
        default: ok = false;
        }
        send(ok ? QByteArray("\r") : QByteArray("\a"));
    }

    void parseGvret()
    {
        const int start = buf.indexOf(char(0xF1));
        if (start < 0)
        {
            buf.clear();
            return;
        }
        buf.remove(0, start);
        if (buf.size() < 2) return;
        const int op = uchar(buf[1]);
        if (op == 0xF1)
        {
            buf.remove(0, 1);
            return;
        }
        int n;
        switch (op)
        {
        case 0: case 11: case 20:
            if (buf.size() < 8) return;
            n = 6 + (uchar(buf[7]) & 0x7F);
            break;
        case 5: n = 8; break;
        case 21: n = 6; break;
        case 14: n = 12; break;
        case 4: case 8: case 10: n = 1; break;
        default:
            buf.remove(0, 2);
            reply(op);
            return;
        }
        if (buf.size() < 2 + n) return;
        const QByteArray a = buf.mid(2, n);
        buf.remove(0, 2 + n);
        command(op, a);
    }

    void reply(int op)
    {
        QByteArray body;
        switch (op)
        {
        case 1: appendLe32(body, us()); break;
        case 2: body = QByteArray(2, 0); break;
        case 3: body = QByteArray(15, 0); break;
        case 6:
            body.append(char(int(enabled) | int(listenOnly) << 4));
            appendLe32(body, bitrate);
            body += QByteArray(5, 0);
            break;
        case 7: body = QByteArray("\x64\x00\x20\x00\x00\x00", 6); break;
        case 9: body = QByteArray("\xDE\xAD", 2); break;
        case 12: body = QByteArray("\x01", 1); break;
        case 13: body = QByteArray(15, 0); break;
        case 22:
            body.append(char(1));
            body.append(char(0x01 | int(fd) << 1));
            appendLe32(body, dataBitrate);
            break;
        default: return;
        }
        send(QByteArray("\xF1", 1) + char(op) + body);
    }

    void command(int op, const QByteArray &a)
    {
        if (op == 5)
        {
            const quint32 word = le32(a, 0);
            if (word & 0x80000000u)
            {
                enabled = word & 0x40000000u;
                listenOnly = word & 0x20000000u;
                bitrate = word & 0x0FFFFFFFu;
            }
            log << QStringLiteral("setup bus0 enabled=%1 listen_only=%2 bitrate=%3")
                       .arg(int(enabled)).arg(int(listenOnly)).arg(bitrate);
        }
        else if (op == 21)
        {
            fd = uchar(a[1]) & 0x02;
            const quint32 rate = le32(a, 2);
            if (rate) dataBitrate = rate;
            log << QStringLiteral("setup_fd fd=%1 data_bitrate=%2").arg(int(fd)).arg(dataBitrate);
        }
        else if (op == 0 || op == 11 || op == 20)
        {
            const quint32 id = le32(a, 0) & 0x1FFFFFFF;
            const int len = uchar(a[5]) & 0x7F;
            log << QStringLiteral("tx %1 %2").arg(id, 0, 16).arg(QString::fromLatin1(a.mid(6, len).toHex()));
        }
    }
};
