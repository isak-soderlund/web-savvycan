#ifndef TESTFRAMES_H
#define TESTFRAMES_H

// Helpers shared by the frame tests: frames, GVRET and slcan encoders, isolated settings.

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include "can_structs.h"

namespace tf {

inline CommFrame makeFrame(quint32 id, int len, bool fd = false, bool brs = false, int bus = 0,
                           qint64 tsUs = 0, bool ext = false, quint8 fill = 0)
{
    CommFrame f;
    f.setFrameId(id);
    f.setExtendedFrameFormat(ext);
    QByteArray d(len, 0);
    for (int i = 0; i < len; i++) d[i] = char(quint8(fill + i));
    f.setPayload(d);
    f.setFrameType(fd ? CommFrame::CANFDDataFrame : CommFrame::CANDataFrame);
    if (fd) f.setBitrateSwitch(brs);
    f.setBus(bus);
    f.setReceived(true);
    f.setTimeStamp(CommFrame::TimeStamp(0, tsUs));
    return f;
}

inline void putLE32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; i++) b.append(char((v >> (8 * i)) & 0xFF));
}

// GVRET, device -> host: classic frame (command 0) or CAN FD frame (command 20)
inline QByteArray gvretFrame(quint32 id, const QByteArray &data, bool fd, bool brs, int bus, quint32 ts, bool ext = false)
{
    QByteArray b;
    b.append(char(0xF1));
    b.append(char(fd ? 20 : 0));
    putLE32(b, ts);
    putLE32(b, id | (ext ? 0x80000000u : 0));
    if (fd)
    {
        b.append(char((data.size() & 0x7F) | (brs ? 0x80 : 0)));
        b.append(char(bus));
    }
    else
    {
        b.append(char((data.size() & 0xF) | (bus << 4)));
    }
    b.append(data);
    b.append(char(0)); // trailing (checksum) byte
    return b;
}

inline int slcanDlcCode(int bytes)
{
    static const int codes[][2] = {{12, 9}, {16, 10}, {20, 11}, {24, 12}, {32, 13}, {48, 14}, {64, 15}};
    if (bytes <= 8) return bytes;
    for (auto &c : codes) if (c[0] == bytes) return c[1];
    return 0;
}

// slcan (LAWICEL) line, device -> host. hwMs >= 0 appends the 4-digit timestamp.
inline QByteArray slcanLine(quint32 id, const QByteArray &data, bool fd, bool brs, bool ext, int hwMs = -1)
{
    char cmd = fd ? (brs ? 'b' : 'd') : 't';
    if (ext) cmd = char(cmd - 'a' + 'A');
    QByteArray b;
    b.append(cmd);
    b.append(QByteArray::number(id, 16).toUpper().rightJustified(ext ? 8 : 3, '0'));
    b.append(QByteArray::number(slcanDlcCode(data.size()), 16).toUpper());
    b.append(data.toHex().toUpper());
    if (hwMs >= 0) b.append(QByteArray::number(hwMs, 16).toUpper().rightJustified(4, '0'));
    b.append('\r');
    return b;
}

// QSettings into a temporary directory, so tests never touch the user's SavvyCAN settings
inline QTemporaryDir *isolateSettings(int maximumFrames)
{
    auto *dir = new QTemporaryDir();
    QCoreApplication::setOrganizationName("web-savvycan-tests");
    QCoreApplication::setApplicationName("frames");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir->path());
    QSettings s;
    s.setValue("Main/MaximumFrames", maximumFrames);
    s.setValue("Main/TimeClock", false);
    s.sync();
    return dir;
}

// Resident memory of this process in kB (Linux), 0 elsewhere
inline qint64 rssKb()
{
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (line.startsWith("VmRSS:"))
            return line.mid(6).trimmed().split(' ').first().toLongLong();
    return 0;
}

} // namespace tf

#endif
