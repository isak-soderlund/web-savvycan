# web-savvycan: tests of the frame store, the trace model, the GVRET/LAWICEL receive path and a
# full-rate replay load test. Standalone (like unit_tests/), desktop only:
#
#   cmake -S test/frames -B build-tests -G Ninja -DCMAKE_PREFIX_PATH=$QT_DESKTOP [-DFRAMES_SANITIZER=address|thread]
#   ninja -C build-tests && (cd build-tests && QT_QPA_PLATFORM=offscreen ctest --output-on-failure)
#
# FRAMES_SANITIZER=address builds with ASan + UBSan, =thread with TSan.
cmake_minimum_required(VERSION 3.21)
project(web_savvycan_frame_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(FRAMES_SANITIZER "" CACHE STRING "Sanitizer: empty, address (ASan+UBSan) or thread (TSan)")

find_package(Qt6 6.4 CONFIG REQUIRED COMPONENTS Core Gui Widgets Test SerialBus SerialPort Network)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(FRAMES_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address -fsanitize=undefined)
elseif(FRAMES_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# The SavvyCAN code under test, as one library
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
    ${SRC} ${SRC}/connections ${SRC}/utils ${SRC}/dbc ${SRC}/bus_protocols
)
target_link_libraries(frames_under_test PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialBus Qt6::SerialPort Qt6::Network Qt6::Test
)

enable_testing()

foreach(t tst_framestore tst_parsers tst_ingest_load)
    add_executable(${t} ${t}.cpp testframes.h)
    target_link_libraries(${t} PRIVATE frames_under_test)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)
endforeach()
