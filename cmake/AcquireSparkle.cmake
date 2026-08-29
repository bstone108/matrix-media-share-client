if(NOT APPLE)
    return()
endif()

set(SPARKLE_VERSION "2.9.6" CACHE STRING "Sparkle framework version to bundle")
set(SPARKLE_PUBLIC_ED_KEY "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc=")

if(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "amd64")
    set(SPARKLE_APPCAST_ARCH "x86_64")
else()
    set(SPARKLE_APPCAST_ARCH "arm64")
endif()

set(SPARKLE_APPCAST_URL
    "https://github.com/bstone108/matrix-media-share-client/releases/latest/download/appcast-macos-${SPARKLE_APPCAST_ARCH}.xml"
)

set(SPARKLE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/Sparkle-${SPARKLE_VERSION}")
set(SPARKLE_FRAMEWORK "${SPARKLE_ROOT}/Sparkle.framework")
set(SPARKLE_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/Sparkle-${SPARKLE_VERSION}.tar.xz")
set(SPARKLE_ARCHIVE_URL
    "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
)

if(NOT EXISTS "${SPARKLE_FRAMEWORK}")
    message(STATUS "Downloading Sparkle ${SPARKLE_VERSION}")
    file(DOWNLOAD
        "${SPARKLE_ARCHIVE_URL}"
        "${SPARKLE_ARCHIVE}"
        SHOW_PROGRESS
        TLS_VERIFY ON
        STATUS _sparkle_status
    )
    list(GET _sparkle_status 0 _sparkle_status_code)
    list(GET _sparkle_status 1 _sparkle_status_message)
    if(NOT _sparkle_status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download Sparkle ${SPARKLE_VERSION}: ${_sparkle_status_message}")
    endif()
    file(MAKE_DIRECTORY "${SPARKLE_ROOT}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xvf "${SPARKLE_ARCHIVE}"
        WORKING_DIRECTORY "${SPARKLE_ROOT}"
        RESULT_VARIABLE _sparkle_extract_result
    )
    if(NOT _sparkle_extract_result EQUAL 0)
        message(FATAL_ERROR "Failed to extract Sparkle ${SPARKLE_VERSION}")
    endif()
    if(NOT EXISTS "${SPARKLE_FRAMEWORK}" AND EXISTS "${SPARKLE_ROOT}/Sparkle.framework")
        # already in place
    elseif(NOT EXISTS "${SPARKLE_FRAMEWORK}")
        file(GLOB_RECURSE _sparkle_candidates LIST_DIRECTORIES true "${SPARKLE_ROOT}/*/Sparkle.framework")
        list(LENGTH _sparkle_candidates _sparkle_candidate_count)
        if(_sparkle_candidate_count GREATER 0)
            list(GET _sparkle_candidates 0 _sparkle_found)
            get_filename_component(_sparkle_found_parent "${_sparkle_found}" DIRECTORY)
            file(COPY "${_sparkle_found}" DESTINATION "${SPARKLE_ROOT}")
        endif()
    endif()
    if(NOT EXISTS "${SPARKLE_FRAMEWORK}")
        message(FATAL_ERROR "Sparkle.framework was not found after extracting Sparkle ${SPARKLE_VERSION}")
    endif()
endif()
