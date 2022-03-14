# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_RESOURCE})
    set(INVADER_RESOURCE true CACHE BOOL "Build invader-resource (builds resource map files)")
endif()

if(${INVADER_RESOURCE})
    add_executable(invader-resource
        src/resource/resource.cpp
    )

    if(MINGW)
        target_sources(invader-resource PRIVATE ${MINGW_CRT_NOGLOB})
    endif()

    target_link_libraries(invader-resource invader)

    set(TARGETS_LIST ${TARGETS_LIST} invader-resource)
    do_windows_rc(invader-resource invader-resource.exe "Invader resource map generation tool")
endif()
