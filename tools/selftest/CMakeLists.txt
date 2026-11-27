# somike-selftest.exe - end-to-end check of capture -> ring -> APO -> microphone
# on real hardware. Diagnostic tool, shipped in the release zip.
add_executable(somike_selftest main.cpp)
set_target_properties(somike_selftest PROPERTIES OUTPUT_NAME somike-selftest)
target_link_libraries(somike_selftest PRIVATE somike::core ole32)
somike_configure_target(somike_selftest)
