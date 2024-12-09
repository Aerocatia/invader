# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_DEPENDENCY})
    set(INVADER_DEPENDENCY true CACHE BOOL "Build invader-dependency (finds dependencies of tags)")
endif()

if(${INVADER_DEPENDENCY})
    add_executable(invader-dependency
        src/dependency/dependency.cpp
    )

    target_link_libraries(invader-dependency invader ${INVADER_CRT_NOGLOB})

    set(TARGETS_LIST ${TARGETS_LIST} invader-dependency)

    if(WIN32)
        target_sources(invader-dependency PRIVATE src/dependency/dependency.rc)
    endif()
endif()
