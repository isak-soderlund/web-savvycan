# Desktop test of the Web Serial QSerialPort stand-in, with a fake backend (no browser, no device):
#   . ~/.local/share/web-savvycan/toolchain.env
#   cmake -S wasm/webserial/tests -B build/webserial-tests -G Ninja -DCMAKE_PREFIX_PATH=$QT_DESKTOP
#   cmake --build build/webserial-tests && ctest --test-dir build/webserial-tests --output-on-failure
cmake_minimum_required(VERSION 3.21)
project(webserial_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# No Qt6::SerialPort here: <QSerialPort> must be the stand-in.
find_package(Qt6 REQUIRED COMPONENTS Core Test)
enable_testing()

set(WEBSERIAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(tst_webserialport tst_webserialport.cpp fakebackend.h
    ${WEBSERIAL_DIR}/webserialport.cpp ${WEBSERIAL_DIR}/webserialport.h)
target_include_directories(tst_webserialport BEFORE PRIVATE ${WEBSERIAL_DIR}/include ${WEBSERIAL_DIR})
target_link_libraries(tst_webserialport PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_webserialport COMMAND tst_webserialport)
