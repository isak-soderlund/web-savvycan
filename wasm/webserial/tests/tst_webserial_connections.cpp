// SavvyCAN's own GVRetSerial and LAWICELSerial, unchanged, on the Web Serial stand-in, against a fake rc1
// (fakerc1.h) behind a fake backend: connect, bus setup, frames both ways, unplug, stop and reconnect.
// On the desktop these classes run in their own QThread; the stand-in routes every report to the port's thread.
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "canconfactory.h"
#include "canconmanager.h"
#include "fakebackend.h"
#include "fakerc1.h"
#include "gvretserial.h"
#include "lawicel_serial.h"

// canconmanager.cpp creates connections through the factory; these tests create them directly.
CANConnection *CanConFactory::create(CANCon::type, QString, QString, int, int, bool, int, bool, bool) { return nullptr; }

Q_DECLARE_METATYPE(CANConStatus)

static const QString kRc1 = QStringLiteral("WebSerial 1209:0001");

class TestConnections : public QObject
{
    Q_OBJECT

    FakeBackend backend;
    FakeRc1 *rc1 = nullptr;

    QList<CommFrame> drain(CANConnection *c)
    {
        QList<CommFrame> out;
        while (CommFrame *f = c->getQueue().peek())
        {
            out.append(*f);
            c->getQueue().dequeue();
        }
        return out;
    }

    bool waitConnected(CANConnection *c, int ms)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
        {
            if (c->getStatus() == CANCon::CONNECTED) return true;
            QTest::qWait(20);
        }
        return false;
    }

    bool waitLog(const QString &needle, int ms = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
        {
            for (const QString &l : rc1->logCopy())
                if (l.contains(needle)) return true;
            QTest::qWait(20);
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("web-savvycan-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_webserial_connections"));
        QSettings().clear();
        qRegisterMetaType<CANConStatus>();
        CANConManager::getInstance();
    }

    void init()
    {
        backend.reset();
        backend.portList = {FakeBackend::rc1(kRc1)};
        backend.autoOpen = true;
        delete rc1;
        rc1 = new FakeRc1();
        backend.device = [this](int h, const QByteArray &b) { rc1->fromHost(h, b); };
        WebSerialHub::setBackend(&backend);
    }

    void cleanupTestCase() { delete rc1; }

    void gvretConnectsAndCarriesFrames()
    {
        auto *c = new GVRetSerial(kRc1, false);
        c->start();
        // SavvyCAN tries ESP32 mode first: DTR/RTS low, 3 s, then the handshake.
        QVERIFY(waitConnected(c, 8000));
        QVERIFY(rc1->isGvret());
        const auto opens = backend.callsOf(QStringLiteral("open"));
        QCOMPARE(opens.size(), 1);
        QCOMPARE(opens[0].options.baudRate, 1000000);
        QCOMPARE(opens[0].options.flowControl, QStringLiteral("none"));
        const auto sig = backend.callsOf(QStringLiteral("signals"));
        QVERIFY(!sig.isEmpty());
        QVERIFY(!sig[0].dtr && !sig[0].rts);

        // Bus setup through GVRET's own commands: 500 kbit/s, CAN FD at 2 Mbit/s, on.
        CANBus bus;
        bus.setSpeed(500000);
        bus.setActive(true);
        bus.setListenOnly(false);
        bus.setCanFD(true);
        bus.setDataRate(2000000);
        c->setBusSettings(0, bus);
        QVERIFY(waitLog(QStringLiteral("setup bus0 enabled=1 listen_only=0 bitrate=500000")));
        QVERIFY(waitLog(QStringLiteral("data_bitrate=2000000")));
        QTRY_VERIFY(rc1->isEnabled());

        // Frames from the bus: classic, extended, CAN FD 64 bytes with BRS.
        rc1->busFrame(0x123, QByteArray::fromHex("0102030405060708"));
        rc1->busFrame(0x18DAF110, QByteArray::fromHex("AABB"), true);
        QByteArray fd64(64, 0);
        for (int i = 0; i < 64; ++i) fd64[i] = char(i);
        rc1->busFrame(0x456, fd64, false, true, true);
        QList<CommFrame> got;
        QTRY_VERIFY_WITH_TIMEOUT((got += drain(c)).size() >= 3, 3000);
        QCOMPARE(got[0].frameId(), 0x123u);
        QCOMPARE(got[0].payload(), QByteArray::fromHex("0102030405060708"));
        QCOMPARE(got[1].frameId(), 0x18DAF110u);
        QVERIFY(got[1].hasExtendedFrameFormat());
        QCOMPARE(got[2].frameId(), 0x456u);
        QVERIFY(got[2].hasFlexibleDataRateFormat());
        QCOMPARE(got[2].payload(), fd64);

        // A frame to the bus.
        CommFrame tx;
        tx.setFrameId(0x321);
        tx.setPayload(QByteArray::fromHex("DEADBEEF"));
        tx.setBus(0);
        QVERIFY(c->sendFrame(tx));
        QVERIFY(waitLog(QStringLiteral("tx 321 deadbeef")));

        // Many frames at once: one batch, nothing lost.
        for (int i = 0; i < 2000; ++i) rc1->busFrame(0x100 + (i % 16), QByteArray(8, char(i)));
        int n = 0;
        QTRY_VERIFY_WITH_TIMEOUT((n += drain(c).size()) >= 2000, 5000);
        QCOMPARE(n, 2000);

        c->stop();
        QTRY_VERIFY(!backend.callsOf(QStringLiteral("close")).isEmpty());
        delete c;
    }

    void gvretUnplugAndReconnect()
    {
        auto *c = new GVRetSerial(kRc1, false);
        c->start();
        QVERIFY(waitConnected(c, 8000));
        const int h = backend.callsOf(QStringLiteral("open")).last().handle;

        // Unplugged: the browser's read loop ends, the bridge reports ResourceError.
        WebSerialHub::deliverError(h, QSerialPort::ResourceError, QStringLiteral("The device was unplugged"));
        QTRY_VERIFY(!backend.callsOf(QStringLiteral("close")).isEmpty());
        QCOMPARE(backend.callsOf(QStringLiteral("close")).last().handle, h);
        c->stop();
        delete c;

        // Plugged in again: a new connection opens the same name with a new handle.
        rc1 = new FakeRc1();
        auto *c2 = new GVRetSerial(kRc1, false);
        c2->start();
        QVERIFY(waitConnected(c2, 8000));
        QVERIFY(backend.callsOf(QStringLiteral("open")).last().handle != h);
        c2->stop();
        delete c2;
    }

    void gvretMissingDevice()
    {
        backend.portList.clear(); // not plugged in
        auto *c = new GVRetSerial(kRc1, false);
        c->start();
        QTest::qWait(300);
        QVERIFY(c->getStatus() != CANCon::CONNECTED);
        QVERIFY(backend.callsOf(QStringLiteral("write")).isEmpty());
        c->stop();
        delete c;
    }

    void lawicelConnectsAndReceives()
    {
        // As the connection dialog: serial speed 1000000, 500 kbit/s, CAN FD 2 Mbit/s.
        auto *c = new LAWICELSerial(kRc1, 1000000, 500000, true, 2000000);
        c->start();
        QVERIFY(waitConnected(c, 3000));
        QVERIFY(waitLog(QStringLiteral("slcan S6")));
        QVERIFY(waitLog(QStringLiteral("slcan Y2")));
        QVERIFY(waitLog(QStringLiteral("slcan O")));
        QVERIFY(!rc1->isGvret());
        QCOMPARE(backend.callsOf(QStringLiteral("open")).last().options.baudRate, 1000000);
        QCOMPARE(backend.callsOf(QStringLiteral("open")).last().options.flowControl, QStringLiteral("hardware"));

        rc1->busFrame(0x7DF, QByteArray::fromHex("0201050000000000"));
        QList<CommFrame> got;
        QTRY_VERIFY_WITH_TIMEOUT((got += drain(c)).size() >= 1, 3000);
        QCOMPARE(got[0].frameId(), 0x7DFu);
        QCOMPARE(got[0].payload(), QByteArray::fromHex("0201050000000000"));

        c->stop();
        QVERIFY(waitLog(QStringLiteral("slcan C")));
        delete c;
    }
};

QTEST_GUILESS_MAIN(TestConnections)
#include "tst_webserial_connections.moc"
