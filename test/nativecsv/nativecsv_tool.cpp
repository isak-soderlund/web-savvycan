// web-savvycan: SavvyCAN's native GVRET CSV (FD rows included), checked with SavvyCAN's own code.
//
//   nativecsv_tool gen <rows.csv> <count>          reference rows (no header), made by FrameFileIO::nativeCSVRow
//   nativecsv_tool check <log.csv> [--exact N | --min N] [--max N] [--first I]
//                                                  load the file as SavvyCAN does (autoDetectLoadFile) and compare
//                                                  frame i with reference frame I+i; fails on any difference
//   nativecsv_tool continuous <log.csv> <count>    desktop continuous logging (open, write in batches, flush,
//                                                  close), then check --exact count
//   nativecsv_tool roundtrip <log.csv> <count>     saveNativeCSVFile, then check --exact count
//
// The reference frames mix classic CAN (standard/extended, 0-8 bytes, Rx/Tx, buses 0-2) and CAN FD (0-64
// bytes, with/without BRS and ESI), with GVRET-style (µs) and system-clock (seconds + µs) time stamps.
// Exit 0 when everything matches.

#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <cstdio>

#include "framefileio.h"

static const int fdLengths[] = {0, 1, 8, 12, 16, 20, 24, 32, 48, 64};

static CommFrame referenceFrame(int i)
{
    CommFrame f;
    f.setFrameType(CommFrame::CANDataFrame);
    const int kind = i % 8;
    const bool ext = (kind == 1 || kind == 3 || kind == 6);
    quint32 id = ext ? 0x800u + (quint32(i) * 7919u) % (0x1FFFFFFFu - 0x800u) : (quint32(i) * 7u) % 0x800u;
    f.setExtendedFrameFormat(ext);
    f.setFrameId(id);
    f.setBus(i % 3);
    f.setReceived(kind != 1 && kind != 5);
    int len;
    bool fd = false;
    switch (kind)
    {
    case 0: case 1: case 7: len = (i / 8) % 9; break;      // classic, 0..8 bytes
    default: fd = true; len = fdLengths[(i / 8 + kind) % 10]; break;
    }
    QByteArray data(len, 0);
    for (int j = 0; j < len; j++) data[j] = char((i * 31 + j * 17) & 0xFF);
    f.setPayload(data);
    if (fd)
    {
        f.setFlexibleDataRateFormat(true);
        f.setBitrateSwitch(kind == 2 || kind == 3 || kind == 6);
        f.setErrorStateIndicator(kind == 3 || kind == 5);
    }
    // every 16th frame stamped from the system clock (seconds + µs), the rest as GVRET does (µs)
    const qint64 us = 1000000LL + qint64(i) * 137;
    if (i % 16 == 15) f.setTimeStamp(CommFrame::TimeStamp::fromMicroSeconds(1761660000000000LL + us));
    else f.setTimeStamp(CommFrame::TimeStamp(0, us));
    return f;
}

static qint64 micros(const CommFrame &f)
{
    return f.timeStamp().seconds() * 1000000LL + f.timeStamp().microSeconds();
}

static QString describe(const CommFrame &f)
{
    return QString("t=%1 id=%2 ext=%3 rx=%4 bus=%5 fd=%6 brs=%7 esi=%8 data=%9")
        .arg(micros(f)).arg(f.frameId(), 0, 16).arg(f.hasExtendedFrameFormat()).arg(f.isReceived())
        .arg(f.getBus()).arg(f.hasFlexibleDataRateFormat()).arg(f.hasBitrateSwitch())
        .arg(f.hasErrorStateIndicator()).arg(QString(f.payload().toHex()));
}

static bool same(const CommFrame &a, const CommFrame &b)
{
    return micros(a) == micros(b) && a.frameId() == b.frameId()
        && a.hasExtendedFrameFormat() == b.hasExtendedFrameFormat() && a.isReceived() == b.isReceived()
        && a.getBus() == b.getBus() && a.payload() == b.payload()
        && a.hasFlexibleDataRateFormat() == b.hasFlexibleDataRateFormat()
        && a.hasBitrateSwitch() == b.hasBitrateSwitch()
        && a.hasErrorStateIndicator() == b.hasErrorStateIndicator();
}

static int check(const QString &file, long exact, long min, long max, long first = 0)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) { fprintf(stderr, "cannot open %s\n", qPrintable(file)); return 2; }
    const qint64 size = f.size();
    QByteArray tail;
    if (size > 0) { f.seek(size - 1); tail = f.read(1); }
    f.close();

    if (!FrameFileIO::isNativeCSVFile(file)) { fprintf(stderr, "FAIL: not detected as native CSV\n"); return 1; }
    QVector<CommFrame> frames;
    if (!FrameFileIO::autoDetectLoadFile(file, &frames)) { fprintf(stderr, "FAIL: SavvyCAN could not load it\n"); return 1; }
    int bad = 0, fd = 0;
    for (int i = 0; i < frames.count(); i++)
    {
        const CommFrame ref = referenceFrame(int(first) + i);
        if (frames[i].hasFlexibleDataRateFormat()) fd++;
        if (!same(frames[i], ref))
        {
            if (bad < 5)
                fprintf(stderr, "frame %d differs:\n  file %s\n  want %s\n", i,
                        qPrintable(describe(frames[i])), qPrintable(describe(ref)));
            bad++;
        }
    }
    const long n = frames.count();
    printf("%s: %lld bytes, %ld frames (%d CAN FD), last byte %s, %d differing\n", qPrintable(file), size, n, fd,
           tail == "\n" ? "newline" : "NOT a newline", bad);
    if (bad) return 1;
    if (size > 0 && tail != "\n") { fprintf(stderr, "FAIL: the file ends inside a row\n"); return 1; }
    if (exact >= 0 && n != exact) { fprintf(stderr, "FAIL: %ld frames, expected %ld\n", n, exact); return 1; }
    if (min >= 0 && n < min) { fprintf(stderr, "FAIL: %ld frames, expected at least %ld\n", n, min); return 1; }
    if (max >= 0 && n > max) { fprintf(stderr, "FAIL: %ld frames, expected at most %ld\n", n, max); return 1; }
    printf("OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 3)
    {
        fprintf(stderr, "usage: %s gen|check|continuous|roundtrip <file> [count | --exact N | --min N --max N]\n",
                argv[0]);
        return 2;
    }
    const QString cmd = a[1], file = a[2];

    if (cmd == "gen" && a.size() >= 4)
    {
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly)) return 2;
        const int count = a[3].toInt();
        for (int i = 0; i < count; i++) out.write(FrameFileIO::nativeCSVRow(referenceFrame(i)));
        printf("%s: %d reference rows, %lld bytes\n", qPrintable(file), count, out.size());
        return 0;
    }
    if (cmd == "check")
    {
        long exact = -1, min = -1, max = -1, first = 0;
        for (int i = 3; i + 1 < a.size(); i += 2)
        {
            if (a[i] == "--exact") exact = a[i + 1].toLong();
            else if (a[i] == "--min") min = a[i + 1].toLong();
            else if (a[i] == "--max") max = a[i + 1].toLong();
            else if (a[i] == "--first") first = a[i + 1].toLong();
        }
        return check(file, exact, min, max, first);
    }
    if ((cmd == "continuous" || cmd == "roundtrip") && a.size() >= 4)
    {
        const int count = a[3].toInt();
        QVector<CommFrame> all;
        for (int i = 0; i < count; i++) all.append(referenceFrame(i));
        if (cmd == "roundtrip")
        {
            if (!FrameFileIO::saveNativeCSVFile(file, &all)) { fprintf(stderr, "save failed\n"); return 1; }
        }
        else
        {
            if (!FrameFileIO::openContinuousNative(file)) { fprintf(stderr, "open failed\n"); return 1; }
            // batches of varying size, as frames come from a connection, with a flush now and then
            int i = 0, batch = 0;
            while (i < count)
            {
                QVector<CommFrame> b;
                for (int k = 0; k < 1 + (batch * 13) % 97 && i < count; k++) b.append(all[i++]);
                FrameFileIO::writeContinuousNative(&b, 0);
                if (++batch % 9 == 0) FrameFileIO::flushContinuousNative();
            }
            FrameFileIO::closeContinuousNative();
        }
        return check(file, count, -1, -1);
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
