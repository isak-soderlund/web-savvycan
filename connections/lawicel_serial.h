#ifndef LAWICELSERIAL_H
#define LAWICELSERIAL_H

#include <QSerialPort>
#include <QCanBusDevice>
#include <QTimer>
#include "canconnection.h"


class LAWICELSerial : public CANConnection
{
    Q_OBJECT

public:
    LAWICELSerial(QString portName, int serialSpeed, int lawicelSpeed, bool canFd, int dataRate, bool useThread = true);
    virtual ~LAWICELSerial();

    /**
     * @brief web-savvycan: parse received bytes (slcan lines), in slices of at most rxSliceBudgetMs() per
     * event-loop turn (see GVRetSerial::processRxBytes). Public for tests.
     */
    void processRxBytes(const QByteArray &data);
    void setRxSliceBudgetMs(int ms) { mRxSliceMs = ms; }
    qsizetype pendingRxBytes() const { return mRxPending.size() - mRxPos; }
    static constexpr int MaxLineLength = 160; //longest slcan line: D + 8 id + 1 dlc + 128 data + 4 time + CR

protected:

    virtual void piStarted();
    virtual void piStop();
    virtual void piSetBusSettings(int pBusIdx, CANBus pBus);
    virtual bool piGetBusSettings(int pBusIdx, CANBus& pBus);
    virtual void piSuspend(bool pSuspend);
    virtual bool piSendFrame(const CommFrame&) ;

    void disconnectDevice();

public slots:
    void debugInput(QByteArray bytes);

private slots:
    void connectDevice();
    void connectionTimeout();
    void readSerialData();
    void serialError(QSerialPort::SerialPortError err);
    void deviceConnected();
    void handleTick();
    void processPendingRx();

private:
    void readSettings();
    void rebuildLocalTimeBasis();
    void sendToSerial(const QByteArray &bytes);
    void sendDebug(const QString debugText);
    uint8_t dlc_code_to_bytes(int dlc_code);
    uint8_t bytes_to_dlc_code(uint8_t bytes);
    void processLine();

protected:
    QTimer             mTimer;
    QString            mBuildLine;

    QSerialPort *serial;
    int framesRapid;
    qint64 timeBasis;
    qint64 lastHWTimestamp;
    qint64 wrapAdder;
    bool can0Enabled;
    bool can0ListenOnly;
    QByteArray mRxPending;
    qsizetype mRxPos = 0;
    bool mRxScheduled = false;
#ifdef Q_OS_WASM
    int mRxSliceMs = 8;
#else
    int mRxSliceMs = 0;
#endif
};

#endif // LAWICELSERIAL_H
