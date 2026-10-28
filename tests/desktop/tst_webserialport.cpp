// Unit tests of the Web Serial QSerialPort stand-in, on the desktop, with a fake backend (no browser, no device).
#include <QSignalSpy>
#include <QtTest>

#include <QSerialPort>
#include <QSerialPortInfo>

#include "fakebackend.h"

// Hands every queued report over (the event loop does it in the app).
static void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }

class TestWebSerialPort : public QObject
{
    Q_OBJECT

    FakeBackend backend;
    WebSerialHub *hub() { return WebSerialHub::instance(); }

private slots:
    void init()
    {
        backend.reset();
        backend.portList = {FakeBackend::rc1()};
        WebSerialHub::setBackend(&backend);
        pump(); // nothing left over from the last test
    }

    void availablePortsComeFromTheBackend()
    {
        backend.portList.append(QSerialPortInfo("WebSerial #1", "Serial port"));
        const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
        QCOMPARE(ports.size(), 2);
        QCOMPARE(ports[0].portName(), QStringLiteral("WebSerial 1209:0001"));
        QCOMPARE(ports[0].description(), QStringLiteral("resolve-can (rc1)"));
        QCOMPARE(ports[1].portName(), QStringLiteral("WebSerial #1"));

        // As SavvyCAN does: QSerialPort(QSerialPortInfo(name))
        QSerialPortInfo info(QStringLiteral("WebSerial 1209:0001"));
        QCOMPARE(info.description(), QStringLiteral("resolve-can (rc1)"));
        QSerialPort port(info);
        QCOMPARE(port.portName(), QStringLiteral("WebSerial 1209:0001"));
        QSerialPortInfo missing(QStringLiteral("WebSerial 1209:0002"));
        QCOMPARE(missing.portName(), QStringLiteral("WebSerial 1209:0002"));
        QVERIFY(missing.description().isEmpty());
    }

    void openPassesSettingsAndQueuesUntilOpen()
    {
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QVERIFY(port.setBaudRate(1000000));
        QVERIFY(port.setDataBits(QSerialPort::Data8));
        QVERIFY(port.setFlowControl(QSerialPort::NoFlowControl));
        QVERIFY(port.open(QIODevice::ReadWrite));
        QVERIFY(port.isOpen());
        const auto opens = backend.callsOf(QStringLiteral("open"));
        QCOMPARE(opens.size(), 1);
        QCOMPARE(opens[0].name, QStringLiteral("WebSerial 1209:0001"));
        QCOMPARE(opens[0].baudRate, 1000000);
        QCOMPARE(opens[0].hardwareFlowControl, false);
        const int h = opens[0].handle;
        QVERIFY(h > 0);
        QCOMPARE(port.handle(), h);

        // GVRET's ESP32 mode: DTR and RTS low right after open(), then the handshake.
        QVERIFY(port.setDataTerminalReady(false));
        QVERIFY(port.setRequestToSend(false));
        QCOMPARE(port.write(QByteArray("\xE7\xE7\xF1\x0C", 4)), qint64(4));
        QVERIFY(backend.callsOf(QStringLiteral("write")).isEmpty());
        QVERIFY(backend.callsOf(QStringLiteral("signals")).isEmpty());

        WebSerialHub::deliverOpened(h, true, 0, QString());
        pump();
        // Signals first, then the queued bytes, in one write.
        QCOMPARE(backend.calls.size(), 3);
        QCOMPARE(backend.calls[1].what, QStringLiteral("signals"));
        QCOMPARE(backend.calls[1].dtr, false);
        QCOMPARE(backend.calls[1].rts, false);
        QCOMPARE(backend.calls[2].what, QStringLiteral("write"));
        QCOMPARE(backend.calls[2].bytes, QByteArray("\xE7\xE7\xF1\x0C", 4));

        // Open: writes go straight through.
        port.write(QByteArray("\xF1\x09", 2));
        QCOMPARE(backend.callsOf(QStringLiteral("write")).size(), 2);
    }

    void hardwareFlowControl()
    {
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        port.setBaudRate(115200);
        port.setFlowControl(QSerialPort::HardwareControl);
        QVERIFY(port.open(QIODevice::ReadWrite));
        const auto o = backend.callsOf(QStringLiteral("open"))[0];
        QCOMPARE(o.hardwareFlowControl, true);
        QCOMPARE(o.baudRate, 115200);
    }

    void dataBatchesIntoOneReadyRead()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QSignalSpy ready(&port, &QIODevice::readyRead);
        QVERIFY(port.open(QIODevice::ReadWrite));
        pump();
        const int h = port.handle();
        WebSerialHub::deliverData(h, QByteArray("ab"));
        WebSerialHub::deliverData(h, QByteArray("cd"));
        WebSerialHub::deliverData(h, QByteArray());
        QCOMPARE(ready.size(), 0); // never inside the caller (a JS callback): from the event loop
        pump();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(port.bytesAvailable(), qint64(4));
        QCOMPARE(port.readAll(), QByteArray("abcd"));
        QCOMPARE(port.bytesAvailable(), qint64(0));

        WebSerialHub::deliverData(h, QByteArray("O\r\nZ"));
        pump();
        QCOMPARE(ready.size(), 2);
        QCOMPARE(port.readLine(), QByteArray("O\r\n"));
        QCOMPARE(port.read(10), QByteArray("Z"));
    }

    void reportsArriveThroughTheEventLoop()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QSignalSpy ready(&port, &QIODevice::readyRead);
        QVERIFY(port.open(QIODevice::ReadWrite));
        WebSerialHub::deliverData(port.handle(), QByteArray("x"));
        QTRY_COMPARE(ready.size(), 1);
    }

    void openFailureClosesAndReports()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0005"));
        QSignalSpy errors(&port, &QSerialPort::errorOccurred);
        QVERIFY(port.open(QIODevice::ReadWrite)); // asynchronous: true now, the answer later
        port.write("lost");
        pump();
        QVERIFY(!port.isOpen());
        QCOMPARE(port.error(), QSerialPort::DeviceNotFoundError);
        QCOMPARE(errors.last().at(0).value<QSerialPort::SerialPortError>(), QSerialPort::DeviceNotFoundError);
        QVERIFY(port.errorString().contains(QStringLiteral("not available")));
        QVERIFY(backend.callsOf(QStringLiteral("write")).isEmpty());
        QCOMPARE(port.handle(), 0);
        QVERIFY(port.write("x") < 0);
    }

    void closeForgetsTheHandleAndReopens()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QSignalSpy ready(&port, &QIODevice::readyRead);
        QVERIFY(port.open(QIODevice::ReadWrite));
        pump();
        const int h1 = port.handle();
        WebSerialHub::deliverData(h1, QByteArray("late")); // in flight while closing
        port.close();
        QVERIFY(!port.isOpen());
        QCOMPARE(backend.callsOf(QStringLiteral("close")).size(), 1);
        QCOMPARE(backend.callsOf(QStringLiteral("close"))[0].handle, h1);
        pump();
        QCOMPARE(ready.size(), 0);
        QCOMPARE(port.bytesAvailable(), qint64(0));

        QVERIFY(port.open(QIODevice::ReadWrite));
        const int h2 = port.handle();
        QVERIFY(h2 != h1);
        pump();
        WebSerialHub::deliverData(h1, QByteArray("old"));
        WebSerialHub::deliverData(h2, QByteArray("new"));
        pump();
        QCOMPARE(ready.size(), 1);
        QCOMPARE(port.readAll(), QByteArray("new"));

        port.close();
        port.close(); // not open: NotOpenError, as Qt
        QCOMPARE(port.error(), QSerialPort::NotOpenError);
        QVERIFY(!port.setDataTerminalReady(true));
    }

    void deletedPortGetsNothing()
    {
        backend.autoOpen = true;
        auto *port = new QSerialPort(QStringLiteral("WebSerial 1209:0001"));
        QVERIFY(port->open(QIODevice::ReadWrite));
        const int h = port->handle();
        WebSerialHub::deliverData(h, QByteArray("x"));
        delete port; // SavvyCAN deletes the port right after close() in disconnectDevice()
        QCOMPARE(backend.callsOf(QStringLiteral("close")).size(), 1);
        WebSerialHub::deliverError(h, QSerialPort::ResourceError, QStringLiteral("gone"));
        WebSerialHub::deliverData(h, QByteArray("y"));
        pump(); // must not touch the deleted port
    }

    void lostDeviceIsResourceError()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QVERIFY(port.open(QIODevice::ReadWrite));
        pump();
        QSignalSpy errors(&port, &QSerialPort::errorOccurred);
        WebSerialHub::deliverError(port.handle(), QSerialPort::ResourceError, QStringLiteral("The device was unplugged"));
        QCOMPARE(errors.size(), 0);
        pump();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(port.error(), QSerialPort::ResourceError);
        QVERIFY(port.isOpen()); // as Qt: the app closes it
        port.clearError();
        QCOMPARE(port.error(), QSerialPort::NoError);
        port.close();
    }

    void orderIsKept()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QStringList seen;
        connect(&port, &QIODevice::readyRead, this, [&] { seen << QString::fromLatin1(port.readAll()); });
        connect(&port, &QSerialPort::errorOccurred, this, [&](QSerialPort::SerialPortError e) {
            if (e != QSerialPort::NoError) seen << QStringLiteral("error");
        });
        QVERIFY(port.open(QIODevice::ReadWrite));
        WebSerialHub::deliverData(port.handle(), QByteArray("a"));
        WebSerialHub::deliverData(port.handle(), QByteArray("b"));
        WebSerialHub::deliverError(port.handle(), QSerialPort::ResourceError, QString());
        pump();
        QCOMPARE(seen, QStringList({QStringLiteral("ab"), QStringLiteral("error")}));
    }

    void dtrAfterOpenGoesStraightOut()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QVERIFY(port.open(QIODevice::ReadWrite));
        pump();
        QVERIFY(port.setDataTerminalReady(true));
        QVERIFY(port.setDataTerminalReady(false));
        QVERIFY(port.setDataTerminalReady(false)); // repeated: still sent (rc1 reads "both low again")
        QCOMPARE(backend.callsOf(QStringLiteral("signals")).size(), 3);
        QVERIFY(!backend.callsOf(QStringLiteral("signals")).last().dtr);
    }

    void unsupportedBrowser()
    {
        backend.supported = false;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QSignalSpy errors(&port, &QSerialPort::errorOccurred);
        QVERIFY(!port.open(QIODevice::ReadWrite));
        QCOMPARE(port.error(), QSerialPort::UnsupportedOperationError);
        QCOMPARE(errors.size(), 1);

        QSignalSpy chosen(hub(), &WebSerialHub::portChosen);
        WebSerialHub::requestPort(false);
        pump();
        QCOMPARE(chosen.size(), 1);
        QVERIFY(chosen[0][0].toString().isEmpty());
        QVERIFY(chosen[0][1].toString().contains(QStringLiteral("Web Serial")));
        QVERIFY(backend.callsOf(QStringLiteral("request")).isEmpty());
    }

    void requestPortAndPortsChanged()
    {
        QSignalSpy chosen(hub(), &WebSerialHub::portChosen);
        QSignalSpy changed(hub(), &WebSerialHub::portsChanged);
        WebSerialHub::requestPort(true);
        QCOMPARE(backend.callsOf(QStringLiteral("request")).size(), 1);
        QVERIFY(backend.callsOf(QStringLiteral("request"))[0].any);
        WebSerialHub::deliverPortsChanged();
        WebSerialHub::deliverPortChosen(QStringLiteral("WebSerial 1209:0001"), QString());
        QCOMPARE(chosen.size(), 0);
        pump();
        QCOMPARE(changed.size(), 1);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen[0][0].toString(), QStringLiteral("WebSerial 1209:0001"));
    }

    void doubleOpenAndNoName()
    {
        backend.autoOpen = true;
        QSerialPort port(QStringLiteral("WebSerial 1209:0001"));
        QVERIFY(port.open(QIODevice::ReadWrite));
        QVERIFY(!port.open(QIODevice::ReadWrite));
        QCOMPARE(port.error(), QSerialPort::OpenError);
        QSerialPort none{QString()};
        QVERIFY(!none.open(QIODevice::ReadWrite));
        QCOMPARE(none.error(), QSerialPort::DeviceNotFoundError);
    }
};

QTEST_GUILESS_MAIN(TestWebSerialPort)
#include "tst_webserialport.moc"
