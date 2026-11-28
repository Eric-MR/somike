include(FetchContent)
include(GoogleTest)

# Pinned release tarball + hash: reproducible and tamper-evident.
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6E3191C1455468B3FC35A417FB565C1C5071AEE1B7E7F85E30CF48A98D37D8B5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)  # we link the static CRT everywhere
FetchContent_MakeAvailable(googletest)

add_executable(somike_tests
    apo/apo_com_test.cpp
    app/app_state_test.cpp
    app/sound_catalog_test.cpp
    app/soundboard_library_test.cpp
    common/registry_codec_test.cpp
    core/audio_file_test.cpp
    core/process_tree_test.cpp
    core/settings_test.cpp
    dsp/canonical_convert_test.cpp
    dsp/frame_pacer_test.cpp
    dsp/injector_test.cpp
    dsp/linear_resampler_test.cpp
    dsp/mixer_test.cpp
    dsp/pump_scheduler_test.cpp
    dsp/ring_test.cpp
    dsp/voice_mixer_test.cpp
    installer/endpoint_id_test.cpp
    installer/install_plan_test.cpp
    support/test_region.h
)
target_include_directories(somike_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(somike_tests PRIVATE
    somike::common
    somike::core
    somike::dsp
    somike::installer
    somike_app_core
    GTest::gtest_main
)
somike_configure_target(somike_tests)
# The APO COM tests load somike_apo.dll from the same output directory.
add_dependencies(somike_tests somike_apo)

# PRE_TEST: enumerate tests when ctest runs, not as a post-build step, so the
# build itself never has to execute a freshly built (unsigned) binary.
gtest_discover_tests(somike_tests DISCOVERY_MODE PRE_TEST)
