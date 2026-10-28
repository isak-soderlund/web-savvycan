# web-savvycan: settings for the WebAssembly build (Qt for WebAssembly, wasm_singlethread).
# Included from CMakeLists.txt when EMSCRIPTEN is set; the desktop build never reads this file.

# Stand-ins for Qt Serial Bus (wasm/include), found before any Qt header.
target_include_directories(SavvyCAN BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/wasm/include)
# Qt Serial Port: the Web Serial stand-in (wasm/webserial: QSerialPort, QSerialPortInfo, the JS bridge).
include(${CMAKE_CURRENT_LIST_DIR}/webserial/webserial.cmake)

# No TLS in Qt for WebAssembly: MQTT over TLS is left out, and the TLS socket's header has no
# QSslSocket to moc against.
target_compile_definitions(SavvyCAN PRIVATE QT_NO_SSL)
set_source_files_properties(mqtt/qmqtt_ssl_socket_p.h PROPERTIES SKIP_AUTOMOC ON)

# Size: optimise our own code for size; Qt's static libraries come prebuilt.
target_compile_options(SavvyCAN PRIVATE ${WEB_SAVVYCAN_WASM_LINK_OPT})

# QDialog::exec() and the other nested event loops need the stack switched out:
# Asyncify (works with Qt's prebuilt binaries) or JSPI (Chrome 137+; Qt built with JSPI).
if(WEB_SAVVYCAN_JSPI)
    target_link_options(SavvyCAN PRIVATE "-sJSPI")
else()
    target_link_options(SavvyCAN PRIVATE "-sASYNCIFY")
endif()
target_link_options(SavvyCAN PRIVATE ${WEB_SAVVYCAN_WASM_LINK_OPT})
