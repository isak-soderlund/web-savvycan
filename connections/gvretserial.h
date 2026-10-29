#ifndef GVRETSERIAL_H
#define GVRETSERIAL_H

#include <QSerialPort>
#include <QCanBusDevice>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
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

private:
    void readSettings();
    void procRXChar(unsigned char);
    void processRxByte(unsigned char c) override { procRXChar(c); }
    void emitBuiltFrame(CommFrame::FrameType frameType);
    void sendCommValidation();
    void rebuildLocalTimeBasis();
    qint64 unwrapDeviceTime(quint32 raw);
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
    qint64 timeBasis;           //added to the device's (unwrapped) time: µs since the app's time basis
    uint64_t lastSystemTimeBasis;
    uint64_t timeAtGVRETSync;
    //web-savvycan: GVRET sends 32-bit µs, which wrap every 71.6 min; unwrapDeviceTime() makes them 64-bit (one device
    //clock for all its buses). The anchor is the session's first device time and the host's monotonic clock then.
    qint64 anchorDeviceUs;
    QElapsedTimer anchorClock;  //invalid until the session's first device time
    qint64 syncDeviceUs;        //the time sync's device time, unwrapped
    bool haveTimeSync;          //the device answered the time sync (GVRET command 1) in this session
};

#endif // GVRETSERIAL_H
