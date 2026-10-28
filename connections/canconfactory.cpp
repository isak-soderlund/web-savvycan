#include <QString>
#include "canconfactory.h"
#ifndef Q_OS_WASM // Qt Serial Bus does not build for WebAssembly
#include "serialbusconnection.h"
#endif
#include "gvretserial.h"
#include "mqtt_bus.h"
#include "socketcand.h"
#include "lawicel_serial.h"
#include "canserver.h"
#include "canlogserver.h"

using namespace CANCon;

CANConnection* CanConFactory::create(type pType, QString pPortName, QString pDriverName, int pSerialSpeed, int pBusSpeed, bool pCanFd, int pDataRate, bool pListenOnly, bool pActive)
{
#ifdef Q_OS_WASM
    // web-savvycan: the browser has GVRET and LAWICEL on a Web Serial port only; another type (in settings an
    // earlier build saved) falls back to GVRET on that port.
    if (pType == LAWICEL) return new LAWICELSerial(pPortName, pSerialSpeed, pBusSpeed, pCanFd, pDataRate);
    return new GVRetSerial(pPortName, false);
#endif
    switch(pType) {
#ifndef Q_OS_WASM
    case SERIALBUS:
      return new SerialBusConnection(pPortName, pDriverName, pBusSpeed, pDataRate, pCanFd, pListenOnly, pActive);
#endif
    case GVRET_SERIAL:
        if(pPortName.contains(".") && !pPortName.contains("tty") && !pPortName.contains("serial"))
        return new GVRetSerial(pPortName, true);
        else
        return new GVRetSerial(pPortName, false);
    case REMOTE:
        return new GVRetSerial(pPortName, true);  //it's a special case of GVRET connected over TCP/IP so it uses the same class
    case LAWICEL:
        return new LAWICELSerial(pPortName, pSerialSpeed, pBusSpeed, pCanFd, pDataRate);
    case KAYAK:
        return new SocketCANd(pPortName);
    case MQTT:
        return new MQTT_BUS(pPortName);
    case CANSERVER:
        return new CANserver(pPortName);
    case CANLOGSERVER:
        return new CanLogServer(pPortName);
    default: {}
    }

    return nullptr;
}
