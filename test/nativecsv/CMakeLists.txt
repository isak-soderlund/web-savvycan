# web-savvycan: desktop tool for SavvyCAN's native GVRET CSV (FD rows), used by the disk-log tests.
#   cmake -S test/nativecsv -B build/nativecsv -G Ninja -DCMAKE_PREFIX_PATH=$QT_DESKTOP && cmake --build build/nativecsv
cmake_minimum_required(VERSION 3.21)
project(nativecsv_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 CONFIG REQUIRED COMPONENTS Core Gui Widgets SerialBus)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(nativecsv_tool
    nativecsv_tool.cpp
    ${SRC}/framefileio.cpp ${SRC}/framefileio.h
    ${SRC}/blfhandler.cpp
    ${SRC}/pcaplite.cpp
    ${SRC}/utility.cpp
)
target_include_directories(nativecsv_tool PRIVATE ${SRC} ${SRC}/connections ${SRC}/utils)
target_link_libraries(nativecsv_tool PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialBus)
