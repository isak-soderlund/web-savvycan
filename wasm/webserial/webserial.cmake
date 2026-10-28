# web-savvycan: the Web Serial QSerialPort stand-in, for the wasm build only (include() it when EMSCRIPTEN).
# <QSerialPort>, <QSerialPortInfo> (and the QtSerialPort/ forms) resolve to wasm/webserial/include.
set(WEBSERIAL_DIR ${CMAKE_CURRENT_LIST_DIR})

target_include_directories(SavvyCAN BEFORE PRIVATE ${WEBSERIAL_DIR}/include ${WEBSERIAL_DIR})
target_sources(SavvyCAN PRIVATE
    ${WEBSERIAL_DIR}/webserialport.cpp ${WEBSERIAL_DIR}/webserialport.h
    ${WEBSERIAL_DIR}/webserialhub.cpp ${WEBSERIAL_DIR}/webserialhub.h
    ${WEBSERIAL_DIR}/webserialbackend_wasm.cpp
)
# The JS half runs inside the module (Module = this app instance).
target_link_options(SavvyCAN PRIVATE "SHELL:--pre-js ${WEBSERIAL_DIR}/webserial.js")
set_property(TARGET SavvyCAN APPEND PROPERTY LINK_DEPENDS ${WEBSERIAL_DIR}/webserial.js)
