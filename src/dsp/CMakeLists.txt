# Platform-independent DSP + IPC logic. Used by the APO (consumer side) and
# the app (producer side), and fully unit-testable without audiodg.exe.
add_library(somike_dsp STATIC
    src/canonical_convert.cpp
    src/injector.cpp
    src/linear_resampler.cpp
    src/mixer.cpp
    src/ring_reader.cpp
    src/ring_writer.cpp
    src/voice_mixer.cpp
)
add_library(somike::dsp ALIAS somike_dsp)

target_include_directories(somike_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(somike_dsp PUBLIC somike::protocol)
somike_configure_target(somike_dsp)
