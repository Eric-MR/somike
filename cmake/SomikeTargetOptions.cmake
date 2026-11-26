# Common compiler/linker settings for every first-party target.
#
#   somike_configure_target(<target>)
#
# Third-party targets (e.g. GoogleTest) intentionally do not get these.
function(somike_configure_target target)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /sdl
            /guard:cf
        )
        if(SOMIKE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()

        get_target_property(type ${target} TYPE)
        if(type MATCHES "EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY")
            # CFG + CET shadow-stack compatibility: required for loading into
            # processes that enforce them (audiodg.exe may).
            target_link_options(${target} PRIVATE
                /guard:cf
                /CETCOMPAT
                /DYNAMICBASE
                /NXCOMPAT
                "$<$<NOT:$<CONFIG:Debug>>:/DEBUG;/OPT:REF;/OPT:ICF>"
            )
            if(SOMIKE_WARNINGS_AS_ERRORS)
                target_link_options(${target} PRIVATE /WX)
            endif()
        endif()
    endif()
endfunction()
