# somike_apo.dll - the capture stream effect loaded by audiodg.exe.
#
# MODULE (not SHARED): a COM plug-in that nothing links against.
add_library(somike_apo MODULE
    class_factory.cpp
    class_factory.h
    dllmain.cpp
    injector_apo.cpp
    injector_apo.h
    module.cpp
    module.h
    somike_apo.def
    somike_apo.rc
)

target_link_libraries(somike_apo PRIVATE
    somike::dsp
    somike::common
    somike::version
)
set_target_properties(somike_apo PROPERTIES PREFIX "" OUTPUT_NAME somike_apo)
somike_configure_target(somike_apo)
