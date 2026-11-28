# App-side engine: device/session discovery, process-loopback capture, the
# router that feeds the shared ring, settings and injector status.
# No UI code lives here, so a tray app, CLI or service can reuse it.
add_library(somike_core STATIC
    src/audio_devices.cpp
    src/audio_file.cpp
    src/audio_sessions.cpp
    src/elevated_tool.cpp
    src/injector_status.cpp
    src/loopback_capture.cpp
    src/process_tree.cpp
    src/render_stream.cpp
    src/router.cpp
    src/settings.cpp
)
add_library(somike::core ALIAS somike_core)

target_include_directories(somike_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(somike_core
    PUBLIC
        somike::common
        somike::dsp
        somike::installer
    PRIVATE
        avrt
        version
        mfplat
        mfreadwrite
        mfuuid
        mmdevapi
        ole32
        propsys
        shell32
)
somike_configure_target(somike_core)
