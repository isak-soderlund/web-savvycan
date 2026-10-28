# web-savvycan's desktop C++ tests (no browser, no device). Run them with tests/desktop/run.sh.
#   tst_framestore, tst_parsers, tst_ingest_load   the frame store, the trace model, the GVRET/LAWICEL receive path,
#                                                  a full-rate replay load test
#   tst_webserialport                              the Web Serial QSerialPort stand-in, with a fake backend
#   nativecsv_tool                                 SavvyCAN's native GVRET CSV with its own code: desktop continuous
#                                                  logging and save/load (tests here), the reference rows and the
#                                                  load check for the browser's download (tests/web download.spec)
# WSC_SANITIZER=address builds with ASan + UBSan, =thread with TSan.
cmake_minimum_required(VERSION 3.21)
project(web_savvycan_desktop_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(WSC_SANITIZER "" CACHE STRING "Sanitizer: empty, address (ASan+UBSan) or thread (TSan)")
if(WSC_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address -fsanitize=undefined)
elseif(WSC_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Qt6 6.4 CONFIG REQUIRED COMPONENTS Core Gui Widgets Test SerialBus SerialPort Network)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
enable_testing()

# The frame store and the receive path, as one library
add_library(frames_under_test STATIC
    ${SRC}/framestore.cpp ${SRC}/framestore.h
    ${SRC}/canframemodel.cpp ${SRC}/canframemodel.h
    ${SRC}/can_structs.cpp ${SRC}/can_structs.h
    ${SRC}/utility.cpp ${SRC}/utility.h
    ${SRC}/dbc/dbchandler.cpp ${SRC}/dbc/dbchandler.h
    ${SRC}/dbc/dbc_classes.cpp ${SRC}/dbc/dbc_classes.h
    ${SRC}/connections/canconnection.cpp ${SRC}/connections/canconnection.h
    ${SRC}/connections/canconmanager.cpp ${SRC}/connections/canconmanager.h
    ${SRC}/connections/canbus.cpp ${SRC}/connections/canbus.h
    ${SRC}/connections/gvretserial.cpp ${SRC}/connections/gvretserial.h
    ${SRC}/connections/lawicel_serial.cpp ${SRC}/connections/lawicel_serial.h
)
target_include_directories(frames_under_test PUBLIC
    ${SRC} ${SRC}/connections ${SRC}/utils ${SRC}/dbc ${SRC}/bus_protocols)
target_link_libraries(frames_under_test PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialBus Qt6::SerialPort Qt6::Network Qt6::Test)

foreach(t tst_framestore tst_parsers tst_ingest_load)
    add_executable(${t} ${t}.cpp testframes.h)
    target_link_libraries(${t} PRIVATE frames_under_test)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The Web Serial stand-in: no Qt6::SerialPort here, <QSerialPort> must be the stand-in's.
set(WEBSERIAL ${SRC}/wasm/webserial)
add_executable(tst_webserialport tst_webserialport.cpp fakebackend.h
    ${WEBSERIAL}/webserialport.cpp ${WEBSERIAL}/webserialport.h
    ${WEBSERIAL}/webserialhub.cpp ${WEBSERIAL}/webserialhub.h)
target_include_directories(tst_webserialport BEFORE PRIVATE ${WEBSERIAL}/include ${WEBSERIAL})
target_link_libraries(tst_webserialport PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_webserialport COMMAND tst_webserialport)

add_executable(nativecsv_tool nativecsv_tool.cpp
    ${SRC}/framefileio.cpp ${SRC}/framefileio.h ${SRC}/blfhandler.cpp ${SRC}/pcaplite.cpp ${SRC}/utility.cpp)
target_include_directories(nativecsv_tool PRIVATE ${SRC} ${SRC}/connections ${SRC}/utils)
target_link_libraries(nativecsv_tool PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialBus)
add_test(NAME nativecsv_continuous COMMAND nativecsv_tool continuous ${CMAKE_CURRENT_BINARY_DIR}/continuous.csv 20000)
add_test(NAME nativecsv_roundtrip COMMAND nativecsv_tool roundtrip ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.csv 20000)

get_property(ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ALL_TESTS} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)
