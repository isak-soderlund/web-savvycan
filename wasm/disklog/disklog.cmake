# web-savvycan: continuous logging to disk in the browser (wasm build only), see wasm/disklog/disklog.js.
target_sources(SavvyCAN PRIVATE
    wasm/disklog/webdisklog.cpp wasm/disklog/webdisklog.h
    wasm/disklog/weblogstartdialog.cpp wasm/disklog/weblogstartdialog.h
)
# The JS module goes into SavvyCAN.js itself (no change to the HTML page); the download fallback's
# service worker must be its own file next to the page.
set(WEB_SAVVYCAN_DISKLOG_JS ${CMAKE_CURRENT_SOURCE_DIR}/wasm/disklog/disklog.js)
target_link_options(SavvyCAN PRIVATE "--pre-js=${WEB_SAVVYCAN_DISKLOG_JS}" "-lembind")
set_property(TARGET SavvyCAN APPEND PROPERTY LINK_DEPENDS ${WEB_SAVVYCAN_DISKLOG_JS})
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/wasm/disklog/disklog-sw.js ${CMAKE_CURRENT_BINARY_DIR}/disklog-sw.js COPYONLY)
