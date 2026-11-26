# somike-apoctl.exe - command-line installer/diagnostics for the APO.
add_executable(somike_apoctl
    main.cpp
    somike-apoctl.manifest
    somike-apoctl.rc
)
set_target_properties(somike_apoctl PROPERTIES OUTPUT_NAME somike-apoctl)
target_link_libraries(somike_apoctl PRIVATE
    somike::core
    somike::installer
    somike::version
)
# Our manifest carries the UAC level; don't let the linker add a second one.
target_link_options(somike_apoctl PRIVATE /MANIFESTUAC:NO)
somike_configure_target(somike_apoctl)
