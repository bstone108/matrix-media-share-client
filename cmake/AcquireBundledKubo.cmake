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

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            GO111MODULE=on
            CGO_ENABLED=0
            GOOS=${KUBO_GOOS}
            GOARCH=${KUBO_GOARCH}
            GOBIN=${KUBO_OUTPUT_DIR}
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
