# Small Win32 helpers shared by every binary (app, installer, APO).
# Keep this free of shell32/user32 dependencies: it is linked into the APO,
# which runs inside audiodg.exe.
add_library(somike_common STATIC
    src/guid.cpp
    src/hresult.cpp
    src/log.cpp
    src/mapped_file.cpp
    src/registry.cpp
    src/ring_path.cpp
    src/strings.cpp
)
add_library(somike::common ALIAS somike_common)

target_include_directories(somike_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(somike_common
    PUBLIC
        somike::protocol
        advapi32
        ole32
)
somike_configure_target(somike_common)
