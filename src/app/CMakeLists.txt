# somike.exe - the desktop application: a Win32 window hosting an HTML UI in
# WebView2 (the runtime ships with Windows 11; the loader is linked statically,
# so there is no extra DLL to deploy).
include(FetchContent)

# Pinned packages + hashes: reproducible and tamper-evident.
FetchContent_Declare(webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4370.26
    URL_HASH SHA256=1C0DACAC84B70109B3FD42F90C53A0BA942ABC3A87590D7A4E0CCD86E4A06CF4
    DOWNLOAD_NAME microsoft.web.webview2.1.0.4370.26.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(webview2 nlohmann_json)

add_library(webview2_loader STATIC IMPORTED)
set_target_properties(webview2_loader PROPERTIES
    IMPORTED_LOCATION "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${webview2_SOURCE_DIR}/build/native/include"
)

# UI-independent logic + the UI contract, shared with the unit tests.
add_library(somike_app_core STATIC
    app_controller.cpp
    app_controller.h
    app_state.cpp
    app_state.h
    http_client.cpp
    http_client.h
    icon_cache.cpp
    icon_cache.h
    sound_catalog.cpp
    sound_catalog.h
    soundboard.cpp
    soundboard.h
    soundboard_library.cpp
    soundboard_library.h
    task_runner.cpp
    task_runner.h
)
target_include_directories(somike_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(somike_app_core
    PUBLIC
        somike::core
        nlohmann_json::nlohmann_json
    PRIVATE
        somike::version
        crypt32
        shell32
        shlwapi
        windowscodecs
        winhttp
)
somike_configure_target(somike_app_core)

set(SOMIKE_UI_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/app.css
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/soundboard.css
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/soundboard.js
    ${CMAKE_CURRENT_SOURCE_DIR}/somike.ico
)
# The UI is embedded via the .rc file; rebuild it when a UI file changes.
set_source_files_properties(somike.rc PROPERTIES OBJECT_DEPENDS "${SOMIKE_UI_FILES}")

add_executable(somike WIN32
    main.cpp
    resource.h
    somike.manifest
    somike.rc
    soundboard_bridge.cpp
    soundboard_bridge.h
    web_window.cpp
    web_window.h
    ${SOMIKE_UI_FILES}
)
set_source_files_properties(${SOMIKE_UI_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
target_link_libraries(somike PRIVATE
    somike_app_core
    somike::version
    webview2_loader
    advapi32
    dwmapi
    ole32
    shell32
    shlwapi
    user32
    version
)
# Our manifest carries the UAC level; don't let the linker add a second one.
target_link_options(somike PRIVATE /MANIFESTUAC:NO)
somike_configure_target(somike)

# The app is only useful together with the APO and the installer tool, which
# must sit next to somike.exe.
add_dependencies(somike somike_apo somike_apoctl)
