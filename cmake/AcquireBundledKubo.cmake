if(NOT DEFINED KUBO_OUTPUT_DIR OR KUBO_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "KUBO_OUTPUT_DIR is required")
endif()
if(NOT DEFINED KUBO_OUTPUT_BINARY OR KUBO_OUTPUT_BINARY STREQUAL "")
    message(FATAL_ERROR "KUBO_OUTPUT_BINARY is required")
endif()
if(NOT DEFINED KUBO_OUTPUT_NAME OR KUBO_OUTPUT_NAME STREQUAL "")
    message(FATAL_ERROR "KUBO_OUTPUT_NAME is required")
endif()
if(NOT DEFINED KUBO_VERSION OR KUBO_VERSION STREQUAL "")
    message(FATAL_ERROR "KUBO_VERSION is required")
endif()
if(NOT DEFINED KUBO_GOOS OR KUBO_GOOS STREQUAL "")
    message(FATAL_ERROR "KUBO_GOOS is required")
endif()
if(NOT DEFINED KUBO_GOARCH OR KUBO_GOARCH STREQUAL "")
    message(FATAL_ERROR "KUBO_GOARCH is required")
endif()
if(NOT DEFINED KUBO_STAMP OR KUBO_STAMP STREQUAL "")
    message(FATAL_ERROR "KUBO_STAMP is required")
endif()

file(MAKE_DIRECTORY "${KUBO_OUTPUT_DIR}")

if(DEFINED KUBO_SOURCE AND NOT KUBO_SOURCE STREQUAL "")
    if(NOT EXISTS "${KUBO_SOURCE}")
        message(FATAL_ERROR "Configured Kubo source binary does not exist: ${KUBO_SOURCE}")
    endif()
    file(COPY_FILE "${KUBO_SOURCE}" "${KUBO_OUTPUT_BINARY}" ONLY_IF_DIFFERENT)
else()
    if(NOT DEFINED GO_EXECUTABLE OR GO_EXECUTABLE STREQUAL "")
        message(FATAL_ERROR "Go is required to build the bundled Kubo binary from source.")
    endif()

    set(_kubo_gopath "${KUBO_OUTPUT_DIR}/go-work")
    set(_kubo_install_candidate
        "${_kubo_gopath}/bin/${KUBO_GOOS}_${KUBO_GOARCH}/${KUBO_OUTPUT_NAME}"
    )

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            GO111MODULE=on
            CGO_ENABLED=0
            GOOS=${KUBO_GOOS}
            GOARCH=${KUBO_GOARCH}
            GOPATH=${_kubo_gopath}
            "${GO_EXECUTABLE}" install github.com/ipfs/kubo/cmd/ipfs@${KUBO_VERSION}
        RESULT_VARIABLE kubo_result
        OUTPUT_VARIABLE kubo_stdout
        ERROR_VARIABLE kubo_stderr
    )
    if(NOT kubo_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to build bundled Kubo ${KUBO_VERSION} for ${KUBO_GOOS}/${KUBO_GOARCH}.\n"
            "stdout:\n${kubo_stdout}\n"
            "stderr:\n${kubo_stderr}"
        )
    endif()

    if(NOT EXISTS "${_kubo_install_candidate}")
        set(_kubo_install_candidate "${_kubo_gopath}/bin/${KUBO_OUTPUT_NAME}")
    endif()

    if(NOT EXISTS "${_kubo_install_candidate}")
        file(GLOB_RECURSE _kubo_built_candidates
            LIST_DIRECTORIES FALSE
            "${_kubo_gopath}/bin/${KUBO_OUTPUT_NAME}"
            "${_kubo_gopath}/bin/*/${KUBO_OUTPUT_NAME}"
        )
        list(LENGTH _kubo_built_candidates _kubo_candidate_count)
        if(_kubo_candidate_count GREATER 0)
            list(GET _kubo_built_candidates 0 _kubo_install_candidate)
        endif()
    endif()

    if(NOT EXISTS "${_kubo_install_candidate}")
        message(FATAL_ERROR
            "Bundled Kubo build succeeded but the output binary was not found.\n"
            "expected:\n${_kubo_gopath}/bin/${KUBO_OUTPUT_NAME}\n"
            "or:\n${_kubo_gopath}/bin/${KUBO_GOOS}_${KUBO_GOARCH}/${KUBO_OUTPUT_NAME}"
        )
    endif()

    file(COPY_FILE "${_kubo_install_candidate}" "${KUBO_OUTPUT_BINARY}" ONLY_IF_DIFFERENT)
endif()

if(NOT EXISTS "${KUBO_OUTPUT_BINARY}")
    message(FATAL_ERROR "Bundled Kubo binary was not produced at ${KUBO_OUTPUT_BINARY}")
endif()

if(UNIX)
    file(CHMOD "${KUBO_OUTPUT_BINARY}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()

file(TOUCH "${KUBO_STAMP}")
