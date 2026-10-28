#ifndef GVRETSERIAL_H
#define GVRETSERIAL_H

#include <QSerialPort>
#include <QCanBusDevice>
#include <QThread>
#include <QTimer>
#include <QTcpSocket>
#include <QUdpSocket>
#include "canconnection.h"


namespace SERIALSTATE {

enum STATE
{
    IDLE,
    GET_COMMAND,
    BUILD_CAN_FRAME,
    TIME_SYNC,
    GET_DIG_INPUTS,
    GET_ANALOG_INPUTS,
    SET_DIG_OUTPUTS,
    SETUP_CANBUS,
    GET_CANBUS_PARAMS,
    GET_DEVICE_INFO,
    SET_SINGLEWIRE_MODE,
    GET_NUM_BUSES,
    GET_EXT_BUSES,
    BUILD_FD_FRAME,
    GET_FD_SETTINGS
};

}

using namespace SERIALSTATE;
class GVRetSerial : public CANConnection
{
    Q_OBJECT

public:
    GVRetSerial(QString portName, bool useTcp, bool useThread = true);
    virtual ~GVRetSerial();

    /**
     * @brief web-savvycan: parse received bytes (what readSerialData() does with the port's data). Parsing
     * runs in slices of at most rxSliceBudgetMs() per event-loop turn; the rest continues on the next
     * turn, so a single-threaded (wasm) GUI stays responsive on a busy bus. Public for tests.
     */
    void processRxBytes(const QByteArray &data);
    /** @brief 0 = parse everything at once (desktop default: parsing runs in the connection's thread). */
    void setRxSliceBudgetMs(int ms) { mRxSliceMs = ms; }
    int rxSliceBudgetMs() const { return mRxSliceMs; }
    /** @brief bytes received but not parsed yet (waiting for the next slice) */
    qsizetype pendingRxBytes() const { return mRxPending.size() - mRxPos; }

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
    void procRXChar(unsigned char);
    void emitBuiltFrame(CommFrame::FrameType frameType);
    void sendCommValidation();
    void rebuildLocalTimeBasis();
    void sendToSerial(const QByteArray &bytes);
    void sendDebug(const QString debugText);

protected:
    QTimer             mTimer;

    bool doValidation;
    int validationCounter;
    bool continuousTimeSync;
    bool useTcp;
    bool espSerialMode; //special serial mode for ESP32 based boards - no flow control and much slower serial baud speed
    QSerialPort *serial;
    QTcpSocket *tcpClient;
    QUdpSocket *udpClient;
    STATE rx_state;
    int rx_step;
    CommFrame buildFrame;
    qint64 buildTimestamp;
    quint32 buildId;
    QByteArray buildData;
    int can0Baud, can1Baud, swcanBaud, lin1Baud, lin2Baud;
    bool can0Enabled, can1Enabled, swcanEnabled, lin1Enabled, lin2Enabled;
    bool can0ListenOnly, can1ListenOnly, swcanListenOnly;
    int deviceBuildNum;
    int deviceSingleWireMode;
    bool deviceHasFDSetup; //device answered GET_FD (22), so it also understands SETUP_FD (21)
    int fdReplyBuses;
    quint8 fdReplyFlags;
    quint32 fdReplyRate;
    uint32_t buildTimeBasis;
    int32_t timeBasis;
    uint64_t lastSystemTimeBasis;
    uint64_t timeAtGVRETSync;
    QByteArray mRxPending;   //received bytes not parsed yet
    qsizetype mRxPos = 0;    //next byte of mRxPending to parse
    bool mRxScheduled = false;
#ifdef Q_OS_WASM
    int mRxSliceMs = 8;      //single thread: leave the rest of the frame time to the GUI
#else
    int mRxSliceMs = 0;
#endif
};

#endif // GVRETSERIAL_H
