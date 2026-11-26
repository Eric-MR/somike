# Header-only protocol shared by the app and the APO.
add_library(somike_protocol INTERFACE)
add_library(somike::protocol ALIAS somike_protocol)

target_include_directories(somike_protocol INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(somike_protocol INTERFACE cxx_std_20)
