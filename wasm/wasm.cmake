# web-savvycan: the WebAssembly build (Qt for WebAssembly, wasm_singlethread). Included from CMakeLists.txt
# when EMSCRIPTEN is set; the desktop build never reads this file.

# Stand-ins found before any Qt header: Qt Serial Bus (wasm/include, empty), Qt Serial Port (the Web Serial
# port, wasm/webserial).
target_include_directories(SavvyCAN BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/webserial/include ${CMAKE_CURRENT_LIST_DIR}/webserial)
target_sources(SavvyCAN PRIVATE
    wasm/webserial/webserialport.cpp wasm/webserial/webserialport.h
    wasm/webserial/webserialbackend_wasm.cpp
    wasm/disklog/webdisklog.cpp wasm/disklog/webdisklog.h   # continuous logging: a download on stop
    wasm/windowtabs.cpp wasm/windowtabs.h                   # non-modal windows as tabs of the main window
    testhook.cpp testhook.h                                 # window.webSavvyCAN for the browser tests
    testhook_filetools.cpp                                  # its file-based RE tools part (stage 4)
    testhook_retools.cpp                                    # ... its live RE tools part (stage 4)
)
# The JS halves go into SavvyCAN.js itself (nothing extra in dist/, no change to the page).
foreach(js wasm/webserial/webserial.js wasm/disklog/disklog.js)
    target_link_options(SavvyCAN PRIVATE "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/${js}")
    set_property(TARGET SavvyCAN APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${js})
endforeach()

# No TLS in Qt for WebAssembly: MQTT over TLS is left out, and the TLS socket's header has no
# QSslSocket to moc against.
target_compile_definitions(SavvyCAN PRIVATE QT_NO_SSL)
set_source_files_properties(mqtt/qmqtt_ssl_socket_p.h PROPERTIES SKIP_AUTOMOC ON)

# Size: our own code optimised for size; Qt's static libraries come prebuilt.
target_compile_options(SavvyCAN PRIVATE -Oz)
target_link_options(SavvyCAN PRIVATE -Oz)
# QDialog::exec() and the other nested event loops need the stack switched out (Asyncify works with
# Qt's prebuilt binaries; JSPI would need a Qt built for it).
target_link_options(SavvyCAN PRIVATE "-sASYNCIFY" "-lembind")
# No eval()/new Function() in the generated JS, so the page runs under a CSP without 'unsafe-eval'
# (only 'wasm-unsafe-eval'); the shop's /resolve-can/savvycan route (website PR #76) needs this.
target_link_options(SavvyCAN PRIVATE "-sDYNAMIC_EXECUTION=0")
