# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_INDEX})
    set(INVADER_INDEX true CACHE BOOL "Build invader-index (indexes cache files)")
endif()

if(${INVADER_INDEX})
    add_executable(invader-index
        src/index/index.cpp
    )

    target_link_libraries(invader-index invader ${INVADER_CRT_NOGLOB})

    set(TARGETS_LIST ${TARGETS_LIST} invader-index)

    do_windows_rc(invader-index invader-index "Cache file indexing tool")
endif()
