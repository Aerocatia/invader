# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_SOUND})
    set(INVADER_SOUND true CACHE BOOL "Build invader-sound (builds sound tags)")
endif()

if(${INVADER_SOUND})
    add_executable(invader-sound
        src/sound/sound.cpp
    )

    target_link_libraries(invader-sound invader ${INVADER_CRT_NOGLOB})

    set(TARGETS_LIST ${TARGETS_LIST} invader-sound)

    if(WIN32)
        target_sources(invader-sound PRIVATE src/sound/sound.rc)
    endif()
endif()
