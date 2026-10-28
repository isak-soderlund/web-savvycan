# Desktop tests of the Web Serial QSerialPort stand-in (no browser, no device):
#   tst_webserialport: the stand-in itself, with a fake backend
#   tst_webserial_connections: SavvyCAN's own GVRetSerial and LAWICELSerial on the stand-in, against a fake rc1
#
#   . ~/.local/share/web-savvycan/toolchain.env
#   cmake -S wasm/webserial/tests -B build/webserial-tests -G Ninja -DCMAKE_PREFIX_PATH=$QT_DESKTOP
#   cmake --build build/webserial-tests && ctest --test-dir build/webserial-tests --output-on-failure
cmake_minimum_required(VERSION 3.21)
project(webserial_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# No Qt6::SerialPort here: <QSerialPort> must be the stand-in.
find_package(Qt6 REQUIRED COMPONENTS Core Test Network Widgets SerialBus)
enable_testing()

set(WEBSERIAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ROOT ${WEBSERIAL_DIR}/../..)
set(STANDIN
    ${WEBSERIAL_DIR}/webserialport.cpp ${WEBSERIAL_DIR}/webserialport.h
    ${WEBSERIAL_DIR}/webserialhub.cpp ${WEBSERIAL_DIR}/webserialhub.h
)

add_executable(tst_webserialport tst_webserialport.cpp fakebackend.h ${STANDIN})
target_include_directories(tst_webserialport BEFORE PRIVATE ${WEBSERIAL_DIR}/include ${WEBSERIAL_DIR})
target_link_libraries(tst_webserialport PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_webserialport COMMAND tst_webserialport)

# SavvyCAN's connection classes, unchanged, on the stand-in. Qt6::SerialBus only for <QCanBusFrame> (can_structs.h);
# its include path must not bring Qt's <QSerialPort> before ours (BEFORE below).
add_executable(tst_webserial_connections
    tst_webserial_connections.cpp fakebackend.h fakerc1.h ${STANDIN}
    ${ROOT}/connections/gvretserial.cpp ${ROOT}/connections/gvretserial.h
    ${ROOT}/connections/lawicel_serial.cpp ${ROOT}/connections/lawicel_serial.h
    ${ROOT}/connections/canconnection.cpp ${ROOT}/connections/canconnection.h
    ${ROOT}/connections/canbus.cpp ${ROOT}/connections/canbus.h
    ${ROOT}/connections/canconmanager.cpp ${ROOT}/connections/canconmanager.h
    ${ROOT}/can_structs.cpp ${ROOT}/can_structs.h
    ${ROOT}/utility.cpp ${ROOT}/utility.h
)
target_include_directories(tst_webserial_connections BEFORE PRIVATE
    ${WEBSERIAL_DIR}/include ${WEBSERIAL_DIR} ${ROOT} ${ROOT}/connections)
target_link_libraries(tst_webserial_connections PRIVATE Qt6::Core Qt6::Test Qt6::Network Qt6::Widgets Qt6::SerialBus)
add_test(NAME tst_webserial_connections COMMAND tst_webserial_connections)
set_tests_properties(tst_webserial_connections PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
