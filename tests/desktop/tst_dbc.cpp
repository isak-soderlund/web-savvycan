// web-savvycan: the DBC decoder (stage 3) with CAN FD: SavvyCAN's bit numbering for Intel and Motorola signals in
// frames of up to 64 bytes, signals past bit 63, 64-bit integers, floats, value descriptions, a DBC saved and loaded
// back, and DBC signals removed while something keeps their pointers. The DBC is tests/web/fixtures/stage3_test.dbc
// (the browser tests' fixture). Run under ASan/UBSan too (tests/desktop/run.sh address).
#include <QtTest>
#include <QSignalSpy>
#include <cstring>
#include <memory>

#include "dbc/dbchandler.h"
#include "utility.h"
#include "testframes.h"

static const QString kFixture = QStringLiteral(WSC_SOURCE_DIR "/tests/web/fixtures/stage3_test.dbc");

static CommFrame frame(quint32 id, const QByteArray &data, bool ext = false)
{
    CommFrame f(id, data);
    f.setExtendedFrameFormat(ext);
    return f;
}

class TestDbc : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> settingsDir;
    DBCHandler *h = nullptr;
    DBCFile *file = nullptr;

    DBC_SIGNAL *sig(quint32 id, const char *name)
    {
        DBC_MESSAGE *m = file->messageHandler->findMsgByID(id);
        return m ? m->sigHandler->findSignalByName(name) : nullptr;
    }
    QString text(quint32 id, const char *name, const QByteArray &data)
    {
        QString out;
        DBC_SIGNAL *s = sig(id, name);
        if (!s || !s->processAsText(frame(id, data), out)) return QStringLiteral("<none>");
        return out;
    }

private slots:
    void initTestCase()
    {
        settingsDir.reset(tf::isolateSettings(1000));
        h = DBCHandler::getReference();
        file = h->loadDBCFile(kFixture);
        QVERIFY2(file, "the fixture loads");
    }
    void cleanupTestCase() { h->removeAllFiles(); }

    // ---------------- the bits
    void lastByte()
    {
        QCOMPARE(Utility::signalLastByte(0, 8, true), 0);
        QCOMPARE(Utility::signalLastByte(500, 7, true), 63);
        QCOMPARE(Utility::signalLastByte(63, 8, false), 7);   // Motorola in the last byte: fits 8 bytes
        QCOMPARE(Utility::signalLastByte(7, 16, false), 1);
        QCOMPARE(Utility::signalLastByte(0, 2, false), 1);    // Motorola from bit 0 goes on at bit 15
        QCOMPARE(Utility::signalLastByte(487, 32, false), 63);
        QCOMPARE(Utility::signalLastByte(0, 0, true), -1);
        QVERIFY(Utility::signalFits(8, 63, 8, false));
        QVERIFY(!Utility::signalFits(7, 63, 8, false));
        QVERIFY(!Utility::signalFits(64, 504, 16, true));
    }
    void integerSignal()
    {
        QByteArray d(64, 0);
        d[62] = char(0xA0);
        d[63] = char(0x05);
        QCOMPARE(Utility::processIntegerSignal(d, 500, 7, true, false), qint64(90)); // bits 501, 503, 504, 506
        QByteArray w(64, char(0xFF));
        w[48] = char(0xFE);
        QCOMPARE(Utility::processIntegerSignal(w, 384, 64, true, true), qint64(-2));
        QCOMPARE(quint64(Utility::processIntegerSignal(w, 64, 64, true, false)), ~quint64(0));
        QCOMPARE(Utility::processIntegerSignal(w, 64, 65, true, false), Utility::processIntegerSignal(w, 64, 64, true, false)); // > 64 bits: 64
        QByteArray m(64, 0);
        m[60] = 0x12; m[61] = 0x34; m[62] = 0x56; m[63] = 0x78;
        QCOMPARE(Utility::processIntegerSignal(m, 487, 32, false, false), qint64(0x12345678));
        QCOMPARE(Utility::processIntegerSignal(m.left(63), 487, 32, false, false), qint64(0)); // doesn't fit: 0
        QByteArray s(8, 0);
        s[0] = char(0x80); s[1] = char(0x40);            // Motorola 7|11 signed: 1000 0000 010 = -1022
        QCOMPARE(Utility::processIntegerSignal(s, 7, 11, false, true), qint64(-1022));
    }

    // ---------------- the fixture's signals
    void loaded()
    {
        QCOMPARE(file->messageHandler->getCount(), 11);
        DBC_MESSAGE *fd = file->messageHandler->findMsgByID(0x70F);
        QVERIFY(fd);
        QCOMPARE(fd->len, 64u);
        QVERIFY(fd->canFD);
        QVERIFY(fd->bitRateSwitch);
        DBC_MESSAGE *ext = file->messageHandler->findMsgByID(0x1234576);
        QVERIFY(ext && ext->extendedID && ext->canFD);
        QVERIFY(!file->messageHandler->findMsgByID(0x1DB)->canFD);
        // message colours: their own, else the theme's (an invalid colour); SavvyCAN's white/black default is none
        QCOMPARE(file->messageHandler->findMsgByID(0x1DB)->bgColor, QColor("#ffe0e0"));
        QVERIFY(!file->messageHandler->findMsgByID(0x11A)->bgColor.isValid());
        QVERIFY(!file->messageHandler->findMsgByID(0x11A)->fgColor.isValid());
        QCOMPARE(file->messageHandler->findMsgByID(0x1DB)->rowForeground(), QColor(Qt::black));
        QCOMPARE(file->messageHandler->findMsgByID(0x55B)->rowForeground(), QColor("#ffffff"));
    }
    void decodeClassic()
    {
        QByteArray d(8, 0);
        d[7] = 0x50;
        QCOMPARE(text(0x5BC, "Temperature", d), QStringLiteral("Temperature: 40 degC"));
        double v = 0;
        QVERIFY2(sig(0x5BC, "Temperature")->processAsDouble(frame(0x5BC, d), v), "Motorola in the last byte (was refused)");
        QCOMPARE(v, 40.0);
        QByteArray b(8, 0);
        b[0] = char(0x80); b[1] = char(0x40);             // BatteryCurrent -1022 * 0.5
        b[2] = 0x12; b[3] = char(0xC0);                   // BatteryVoltage 23|10: 0001 0010 11 = 75 * 0.5
        b[4] = 0x04;                                      // BatteryStatus 33|2 Intel: 2 = "Fault"
        QCOMPARE(text(0x1DB, "BatteryCurrent", b), QStringLiteral("BatteryCurrent: -511 A"));
        QCOMPARE(text(0x1DB, "BatteryVoltage", b), QStringLiteral("BatteryVoltage: 37.5 V"));
        QCOMPARE(text(0x1DB, "BatteryStatus", b), QStringLiteral("BatteryStatus: Fault "));
        QCOMPARE(text(0x1DB, "BatteryVoltage", b.left(2)), QStringLiteral("<none>")); // too short: not decoded
        int32_t i = 0;
        QVERIFY(!sig(0x1DB, "BatteryVoltage")->processAsInt(frame(0x1DB, b.left(2)), i));
    }
    void decodeFD()
    {
        QByteArray d(64, 0);
        d[62] = char(0xA0);
        d[63] = char(0x05);
        for (int k = 48; k < 56; k++) d[k] = char(0xFF);
        d[48] = char(0xFE);
        d[60] = 0x12; d[61] = 0x34; d[62] = char(0x56 | 0xA0); d[63] = char(0x78 | 0x05);
        const float f = 3.25f;
        const double g = -1234.5678;
        memcpy(d.data() + 20, &f, 4);
        memcpy(d.data() + 32, &g, 8);
        d[40] = 0x34; d[41] = 0x12;
        d[11] = char(0xFF); d[12] = 0x07;                 // Scaled 88|12 signed: 0x7FF = 2047 * 0.25 - 10
        QCOMPARE(text(0x70F, "Byte40Word", d), QStringLiteral("Byte40Word: 4660 "));
        QCOMPARE(text(0x70F, "Wide64", d), QStringLiteral("Wide64: -2 "));
        QCOMPARE(text(0x70F, "Float32", d), QStringLiteral("Float32: 3.25 "));
        QCOMPARE(text(0x70F, "Double64", d), QStringLiteral("Double64: -1234.57 "));
        QCOMPARE(text(0x70F, "Scaled", d), QStringLiteral("Scaled: 501.75 V"));
        double v = 0;
        QVERIFY(sig(0x70F, "Double64")->processAsDouble(frame(0x70F, d), v));
        QCOMPARE(v, g);                                    // Intel as the DBC says (the graph's path read Motorola)
        QVERIFY(sig(0x70F, "HighBits")->processAsDouble(frame(0x70F, d), v));
        QCOMPARE(v, double(Utility::processIntegerSignal(d, 500, 7, true, false)));
        QByteArray e(64, 0);
        for (int k = 8; k < 16; k++) e[k] = char(0xFF);
        e[63] = 0x42;
        QCOMPARE(text(0x1234576, "ExtU64", e), QStringLiteral("ExtU64: 18446744073709551615 ")); // exact, unsigned
        QCOMPARE(text(0x1234576, "ExtLast", e), QStringLiteral("ExtLast: 66 "));
        QCOMPARE(text(0x1234576, "ExtLast", e.left(48)), QStringLiteral("<none>"));
    }
    void multiplexed()
    {
        DBC_SIGNAL *mux = sig(0x5C0, "TempMux");
        QVERIFY(mux && mux->isMultiplexor);
        QByteArray d(8, 0);
        d[0] = 1;
        d[1] = 100;                                        // 15|8 Motorola = byte 1: 100 - 40
        QCOMPARE(mux->processSignalTree(frame(0x5C0, d)), QStringLiteral("TempMin: 60 degC"));
        d[0] = 0;
        QCOMPARE(mux->processSignalTree(frame(0x5C0, d)), QStringLiteral("TempMax: 60 degC"));
        d[0] = 2;
        QVERIFY(mux->processSignalTree(frame(0x5C0, d)).isEmpty());
    }

    // ---------------- save, load back
    void saveAndLoadBack()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("saved.dbc");
        QVERIFY(file->saveFile(path));
        DBCFile *back = h->loadDBCFile(path);
        QVERIFY(back);
        QCOMPARE(back->messageHandler->getCount(), 11);
        for (DBC_MESSAGE *m : file->messageHandler->getMsgsAsList()) {
            DBC_MESSAGE *b = back->messageHandler->findMsgByID(m->ID);
            QVERIFY2(b, qPrintable(m->name));
            QCOMPARE(b->name, m->name);
            QCOMPARE(b->len, m->len);
            QCOMPARE(b->canFD, m->canFD);
            QCOMPARE(b->extendedID, m->extendedID);
            QCOMPARE(b->bgColor, m->bgColor);
            QCOMPARE(b->fgColor, m->fgColor);
            QCOMPARE(b->sigHandler->getCount(), m->sigHandler->getCount());
            for (DBC_SIGNAL *s : m->sigHandler->getSignalsAsList()) {
                DBC_SIGNAL *t = b->sigHandler->findSignalByName(s->name);
                QVERIFY2(t, qPrintable(s->name));
                QCOMPARE(t->startBit, s->startBit);
                QCOMPARE(t->signalSize, s->signalSize);
                QCOMPARE(t->intelByteOrder, s->intelByteOrder);
                QCOMPARE(int(t->valType), int(s->valType));
                QCOMPARE(t->factor, s->factor);
                QCOMPARE(t->bias, s->bias);
                QCOMPARE(t->min, s->min);              // 15 digits (6 changed e.g. 1.84467440737096e+19)
                QCOMPARE(t->max, s->max);
                QCOMPARE(t->valList.count(), s->valList.count());
            }
        }
        h->removeDBCFile(h->getFileCount() - 1);
    }

    // ---------------- removing what windows point at
    void removal()
    {
        DBCFile *extra = h->loadDBCFile(kFixture);
        QVERIFY(extra);
        QSignalSpy spy(h, &DBCHandler::signalsRemoved);
        DBC_MESSAGE *m = extra->messageHandler->findMsgByID(0x5C0);
        DBC_SIGNAL *child = m->sigHandler->findSignalByName("TempMax");
        DBC_SIGNAL *mux = m->sigHandler->findSignalByName("TempMux");
        QVERIFY(h->isLiveSignal(child));
        QVERIFY(m->sigHandler->removeSignal("TempMax"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(!h->isLiveSignal(child));               // compared, never dereferenced
        QVERIFY(!mux->multiplexedChildren.contains(child));
        QByteArray d(8, 0);
        QVERIFY(mux->processSignalTree(frame(0x5C0, d)).isEmpty()); // no freed child left in the tree
        DBC_SIGNAL *s = extra->messageHandler->findMsgByID(0x1DB)->sigHandler->findSignalByName("BatteryVoltage");
        QVERIFY(extra->messageHandler->removeMessage(0x1DB));
        QCOMPARE(spy.count(), 2);
        QVERIFY(!h->isLiveSignal(s));
        DBC_SIGNAL *last = extra->messageHandler->findMsgByID(0x70F)->sigHandler->findSignalByName("HighBits");
        h->removeDBCFile(h->getFileCount() - 1);
        QCOMPARE(spy.count(), 3);
        QVERIFY(!h->isLiveSignal(last));
        QVERIFY(h->isLiveSignal(sig(0x70F, "HighBits")));  // the other file's own
    }
};

QTEST_MAIN(TestDbc)
#include "tst_dbc.moc"
