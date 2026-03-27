if(NOT DEFINED FFMPEG_OUTPUT_DIR OR FFMPEG_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "FFMPEG_OUTPUT_DIR is required")
endif()
if(NOT DEFINED FFMPEG_OUTPUT_BINARY OR FFMPEG_OUTPUT_BINARY STREQUAL "")
    message(FATAL_ERROR "FFMPEG_OUTPUT_BINARY is required")
endif()
if(NOT DEFINED FFPROBE_OUTPUT_BINARY OR FFPROBE_OUTPUT_BINARY STREQUAL "")
    message(FATAL_ERROR "FFPROBE_OUTPUT_BINARY is required")
endif()
if(NOT DEFINED FFMPEG_OUTPUT_NAME OR FFMPEG_OUTPUT_NAME STREQUAL "")
    message(FATAL_ERROR "FFMPEG_OUTPUT_NAME is required")
endif()
if(NOT DEFINED FFPROBE_OUTPUT_NAME OR FFPROBE_OUTPUT_NAME STREQUAL "")
    message(FATAL_ERROR "FFPROBE_OUTPUT_NAME is required")
endif()
if(NOT DEFINED FFMPEG_STAMP OR FFMPEG_STAMP STREQUAL "")
    message(FATAL_ERROR "FFMPEG_STAMP is required")
endif()

file(MAKE_DIRECTORY "${FFMPEG_OUTPUT_DIR}")

function(_ensure_download url destination)
    if("${url}" STREQUAL "")
        message(FATAL_ERROR "Attempted download with empty URL")
    endif()
    file(DOWNLOAD
        "${url}"
        "${destination}"
        SHOW_PROGRESS
        STATUS _status
        TLS_VERIFY ON
    )
    list(GET _status 0 _status_code)
    list(GET _status 1 _status_message)
    if(NOT _status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download ${url}: ${_status_message}")
    endif()
endfunction()

function(_find_and_copy tool_name extracted_root output_path)
    file(GLOB_RECURSE _matches
        LIST_DIRECTORIES FALSE
        "${extracted_root}/${tool_name}"
        "${extracted_root}/${tool_name}.exe"
        "${extracted_root}/*/${tool_name}"
        "${extracted_root}/*/${tool_name}.exe"
        "${extracted_root}/*/*/${tool_name}"
        "${extracted_root}/*/*/${tool_name}.exe"
        "${extracted_root}/*/*/*/${tool_name}"
        "${extracted_root}/*/*/*/${tool_name}.exe"
    )
    list(LENGTH _matches _match_count)
    if(_match_count EQUAL 0)
        message(FATAL_ERROR "Could not locate ${tool_name} under ${extracted_root}")
    endif()
    list(GET _matches 0 _match)
    file(COPY_FILE "${_match}" "${output_path}" ONLY_IF_DIFFERENT)
endfunction()

function(_copy_tool_from_root root_path tool_name output_path)
    set(_candidates
        "${root_path}/${tool_name}"
        "${root_path}/${tool_name}.exe"
        "${root_path}/bin/${tool_name}"
        "${root_path}/bin/${tool_name}.exe"
    )
    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}")
            file(COPY_FILE "${_candidate}" "${output_path}" ONLY_IF_DIFFERENT)
            return()
        endif()
    endforeach()
    _find_and_copy("${tool_name}" "${root_path}" "${output_path}")
endfunction()

if(DEFINED FFMPEG_SOURCE_ROOT AND NOT FFMPEG_SOURCE_ROOT STREQUAL "")
    if(NOT EXISTS "${FFMPEG_SOURCE_ROOT}")
        message(FATAL_ERROR "Configured FFmpeg source root does not exist: ${FFMPEG_SOURCE_ROOT}")
    endif()
    _copy_tool_from_root("${FFMPEG_SOURCE_ROOT}" "${FFMPEG_OUTPUT_NAME}" "${FFMPEG_OUTPUT_BINARY}")
    _copy_tool_from_root("${FFMPEG_SOURCE_ROOT}" "${FFPROBE_OUTPUT_NAME}" "${FFPROBE_OUTPUT_BINARY}")
elseif(DEFINED FFMPEG_ARCHIVE_URL AND NOT FFMPEG_ARCHIVE_URL STREQUAL "")
    set(_archive_path "${FFMPEG_OUTPUT_DIR}/ffmpeg-archive")
    _ensure_download("${FFMPEG_ARCHIVE_URL}" "${_archive_path}")
    set(_extract_dir "${FFMPEG_OUTPUT_DIR}/archive-extract")
    file(REMOVE_RECURSE "${_extract_dir}")
    file(MAKE_DIRECTORY "${_extract_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_archive_path}" DESTINATION "${_extract_dir}")
    _find_and_copy("${FFMPEG_OUTPUT_NAME}" "${_extract_dir}" "${FFMPEG_OUTPUT_BINARY}")
    _find_and_copy("${FFPROBE_OUTPUT_NAME}" "${_extract_dir}" "${FFPROBE_OUTPUT_BINARY}")
elseif((DEFINED FFMPEG_BINARY_URL AND NOT FFMPEG_BINARY_URL STREQUAL "")
    AND (DEFINED FFPROBE_BINARY_URL AND NOT FFPROBE_BINARY_URL STREQUAL ""))
    foreach(_tool IN ITEMS ffmpeg ffprobe)
        if(_tool STREQUAL "ffmpeg")
            set(_url "${FFMPEG_BINARY_URL}")
            set(_name "${FFMPEG_OUTPUT_NAME}")
            set(_output "${FFMPEG_OUTPUT_BINARY}")
        else()
            set(_url "${FFPROBE_BINARY_URL}")
            set(_name "${FFPROBE_OUTPUT_NAME}")
            set(_output "${FFPROBE_OUTPUT_BINARY}")
        endif()
        set(_archive_path "${FFMPEG_OUTPUT_DIR}/${_tool}-archive")
        _ensure_download("${_url}" "${_archive_path}")
        set(_extract_dir "${FFMPEG_OUTPUT_DIR}/${_tool}-extract")
        file(REMOVE_RECURSE "${_extract_dir}")
        file(MAKE_DIRECTORY "${_extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_archive_path}" DESTINATION "${_extract_dir}")
        _find_and_copy("${_name}" "${_extract_dir}" "${_output}")
    endforeach()
else()
    message(FATAL_ERROR
        "No bundled FFmpeg source was configured. Provide FFMPEG_SOURCE_ROOT, "
        "FFMPEG_ARCHIVE_URL, or both FFMPEG_BINARY_URL and FFPROBE_BINARY_URL."
    )
endif()

foreach(_binary IN ITEMS "${FFMPEG_OUTPUT_BINARY}" "${FFPROBE_OUTPUT_BINARY}")
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "Bundled FFmpeg tool was not produced at ${_binary}")
    endif()
    if(UNIX)
        file(CHMOD "${_binary}"
            PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
        )
    endif()
endforeach()

file(TOUCH "${FFMPEG_STAMP}")
