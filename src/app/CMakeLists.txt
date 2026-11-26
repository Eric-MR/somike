# somike.exe - the desktop application (plain Win32, no UI framework).
add_executable(somike WIN32
    app_controller.cpp
    app_controller.h
    main.cpp
    main_window.cpp
    main_window.h
    resource.h
    somike.manifest
    somike.rc
)
target_link_libraries(somike PRIVATE
    somike::core
    somike::version
    comctl32
    shell32
)
# Our manifest carries the UAC level; don't let the linker add a second one.
target_link_options(somike PRIVATE /MANIFESTUAC:NO)
somike_configure_target(somike)

# The app is only useful together with the APO and the installer tool, which
# must sit next to somike.exe.
add_dependencies(somike somike_apo somike_apoctl)
