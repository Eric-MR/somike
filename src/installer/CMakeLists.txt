# Installation logic for the APO: endpoint inspection (read-only, used by the
# app) and system changes (elevated, used by somike-apoctl).
add_library(somike_installer STATIC
    src/apo_registration.cpp
    src/audio_service.cpp
    src/endpoint_id.cpp
    src/endpoint_install.cpp
    src/fx_config.cpp
    src/install_plan.cpp
    src/ring_file.cpp
)
add_library(somike::installer ALIAS somike_installer)

target_include_directories(somike_installer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(somike_installer
    PUBLIC
        somike::common
    PRIVATE
        somike::dsp
        shell32
)
somike_configure_target(somike_installer)
