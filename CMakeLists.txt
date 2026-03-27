cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" MATRIX_MEDIA_ARCHIVER_APP_VERSION)
string(STRIP "${MATRIX_MEDIA_ARCHIVER_APP_VERSION}" MATRIX_MEDIA_ARCHIVER_APP_VERSION)

if(NOT MATRIX_MEDIA_ARCHIVER_APP_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "VERSION.txt must contain a version in year.month.day.build format")
endif()

project(MatrixMediaShareClientQt VERSION ${MATRIX_MEDIA_ARCHIVER_APP_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MATRIX_MEDIA_ARCHIVER_ENABLE_DEPLOYMENT "Run Qt deployment tooling during install" OFF)
option(MATRIX_MEDIA_ARCHIVER_BUILD_TESTS "Build and register Qt test targets" ON)
set(MATRIX_MEDIA_ARCHIVER_BACKEND_RUST_TARGET "" CACHE STRING
    "Optional Rust target triple for cross-compiling the backend sidecar"
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE "" CACHE FILEPATH
    "Path to the Kubo/IPFS binary to bundle inside the app package"
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT "" CACHE PATH
    "Path to a VLC runtime root containing lib/ and plugins/ to bundle with the app"
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT "" CACHE PATH
    "Path to an FFmpeg runtime root containing ffmpeg and ffprobe binaries to bundle with the app"
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(MATRIX_MEDIA_ARCHIVER_MIN_QT_VERSION 6.2)

if(WIN32)
    # Cross-host Qt builds can spend excessive time in moc compiler predefines parsing.
    set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)
endif()

include(GNUInstallDirs)

if(WIN32 AND MINGW)
    find_package(
        Qt6 ${MATRIX_MEDIA_ARCHIVER_MIN_QT_VERSION}
        REQUIRED
        COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets Test
        OPTIONAL_COMPONENTS Sql
    )

    if(NOT TARGET Qt6::Sql)
        get_filename_component(MATRIX_MEDIA_ARCHIVER_QT_PREFIX
            "${Qt6_DIR}/../../.." ABSOLUTE
        )
        set(MATRIX_MEDIA_ARCHIVER_QTSQL_IMPLIB
            "${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/lib/libQt6Sql.a"
        )
        set(MATRIX_MEDIA_ARCHIVER_QTSQL_DLL
            "${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/bin/Qt6Sql.dll"
        )

        foreach(_qt_sql_path
            "${MATRIX_MEDIA_ARCHIVER_QTSQL_IMPLIB}"
            "${MATRIX_MEDIA_ARCHIVER_QTSQL_DLL}"
            "${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/include"
            "${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/include/QtSql"
        )
            if(NOT EXISTS "${_qt_sql_path}")
                message(FATAL_ERROR "Required Qt Sql artifact not found: ${_qt_sql_path}")
            endif()
        endforeach()
        unset(_qt_sql_path)

        add_library(Qt6::Sql SHARED IMPORTED)
        set_target_properties(Qt6::Sql PROPERTIES
            IMPORTED_IMPLIB "${MATRIX_MEDIA_ARCHIVER_QTSQL_IMPLIB}"
            IMPORTED_LOCATION "${MATRIX_MEDIA_ARCHIVER_QTSQL_DLL}"
            INTERFACE_COMPILE_DEFINITIONS QT_SQL_LIB
            INTERFACE_INCLUDE_DIRECTORIES
                "${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/include;${MATRIX_MEDIA_ARCHIVER_QT_PREFIX}/include/QtSql"
            INTERFACE_LINK_LIBRARIES Qt6::Core
        )
    endif()
else()
    find_package(Qt6 ${MATRIX_MEDIA_ARCHIVER_MIN_QT_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network Sql Multimedia MultimediaWidgets Test)
endif()

find_program(CARGO_EXECUTABLE NAMES cargo HINTS /opt/homebrew/opt/rustup/bin)
find_program(GO_EXECUTABLE NAMES go)

if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR "cargo is required to build the Rust Matrix backend")
endif()

get_filename_component(CARGO_BIN_DIR "${CARGO_EXECUTABLE}" DIRECTORY)

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup(REQUIRES ${MATRIX_MEDIA_ARCHIVER_MIN_QT_VERSION})
endif()

set(MATRIX_MEDIA_ARCHIVER_BACKEND_SOURCES
    backend/Cargo.lock
    backend/Cargo.toml
    backend/src/app_paths.rs
    backend/src/database.rs
    backend/src/domain.rs
    backend/src/folder_naming.rs
    backend/src/gateway_health.rs
    backend/src/gateway_registry.rs
    backend/src/ipfs_service.rs
    backend/src/kubo_manager.rs
    backend/src/landing_page.rs
    backend/src/main.rs
    backend/src/media_classification.rs
    backend/src/protocol.rs
    backend/src/room_catalog.rs
    backend/src/secret_store.rs
    backend/src/service.rs
)

set(MATRIX_MEDIA_ARCHIVER_APP_ICON_PNG
    packaging/icons/matrix-media-archiver.png
)
set(MATRIX_MEDIA_ARCHIVER_APP_ICON_QRC
    packaging/icons/MatrixMediaArchiverQt.qrc
)
set(MATRIX_MEDIA_ARCHIVER_APP_ICON_ICNS
    packaging/macos/MatrixMediaArchiverQt.icns
)
set(MATRIX_MEDIA_ARCHIVER_APP_ICON_RC
    packaging/windows/MatrixMediaArchiverQt.rc
)

set(MATRIX_MEDIA_ARCHIVER_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backend)
set(MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_NAME matrix_media_share_client_backend)
if(WIN32)
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_NAME matrix_media_share_client_backend.exe)
endif()

if(DEFINED ENV{CARGO_TARGET_DIR} AND NOT "$ENV{CARGO_TARGET_DIR}" STREQUAL "")
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_TARGET_ROOT "$ENV{CARGO_TARGET_DIR}")
else()
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_TARGET_ROOT
        ${MATRIX_MEDIA_ARCHIVER_BACKEND_DIR}/target
    )
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_PROFILE release)
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS --release)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release"
    OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
    OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_PROFILE release)
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS --release)
else()
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_PROFILE debug)
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS)
endif()

if(MATRIX_MEDIA_ARCHIVER_BACKEND_RUST_TARGET)
    list(APPEND MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS --target ${MATRIX_MEDIA_ARCHIVER_BACKEND_RUST_TARGET})
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_DIR
        ${MATRIX_MEDIA_ARCHIVER_BACKEND_TARGET_ROOT}/${MATRIX_MEDIA_ARCHIVER_BACKEND_RUST_TARGET}/${MATRIX_MEDIA_ARCHIVER_BACKEND_PROFILE}
    )
else()
    set(MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_DIR
        ${MATRIX_MEDIA_ARCHIVER_BACKEND_TARGET_ROOT}/${MATRIX_MEDIA_ARCHIVER_BACKEND_PROFILE}
    )
endif()

set(MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY
    ${MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_DIR}/${MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_NAME}
)

if(NOT MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE)
    if(APPLE)
        set(_matrix_media_share_client_default_kubo
            "/Applications/IPFS Desktop.app/Contents/Resources/app.asar.unpacked/node_modules/kubo/kubo/ipfs"
        )
        if(EXISTS "${_matrix_media_share_client_default_kubo}")
            set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE
                "${_matrix_media_share_client_default_kubo}"
                CACHE FILEPATH "Path to the Kubo/IPFS binary to bundle inside the app package" FORCE
            )
        endif()
        unset(_matrix_media_share_client_default_kubo)
    endif()
endif()

if(NOT MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT)
    if(APPLE)
        set(_matrix_media_share_client_default_vlc
            "/Applications/VLC.app/Contents/MacOS"
        )
        if(EXISTS "${_matrix_media_share_client_default_vlc}")
            set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
                "${_matrix_media_share_client_default_vlc}"
                CACHE PATH "Path to a VLC runtime root containing lib/ and plugins/ to bundle with the app" FORCE
            )
        endif()
        unset(_matrix_media_share_client_default_vlc)
    endif()
endif()

if(APPLE
    AND MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
    AND NOT EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}"
    AND EXISTS "/Applications/VLC.app/Contents/MacOS")
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
        "/Applications/VLC.app/Contents/MacOS"
        CACHE PATH "Path to a VLC runtime root containing lib/ and plugins/ to bundle with the app" FORCE
    )
endif()

set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_VERSION "v0.38.2" CACHE STRING
    "Pinned Kubo version to bundle when building from source"
)

if(NOT MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE AND NOT GO_EXECUTABLE)
    message(FATAL_ERROR
        "A bundled Kubo binary is required at build time. "
        "Provide MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE or install Go so the build can compile "
        "github.com/ipfs/kubo/cmd/ipfs@${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_VERSION}."
    )
endif()

set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME ipfs)
if(WIN32)
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME ipfs.exe)
endif()

set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/bundled-kubo
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_BINARY
    ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_DIR}/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME}
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_STAMP
    ${CMAKE_CURRENT_BINARY_DIR}/matrix_media_share_client_bundled_kubo.stamp
)

if(CMAKE_GENERATOR_PLATFORM)
    set(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH)
elseif(CMAKE_SYSTEM_PROCESSOR)
    set(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH}" MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOOS darwin)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOOS linux)
elseif(WIN32)
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOOS windows)
else()
    message(FATAL_ERROR "Unsupported platform for bundled Kubo acquisition: ${CMAKE_SYSTEM_NAME}")
endif()

if(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER STREQUAL "x86_64"
    OR MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER STREQUAL "amd64"
    OR MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER STREQUAL "x64")
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOARCH amd64)
    set(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED amd64)
elseif(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER STREQUAL "arm64"
    OR MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_LOWER STREQUAL "aarch64")
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOARCH arm64)
    set(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED arm64)
else()
    message(FATAL_ERROR
        "Unsupported architecture for bundled Kubo acquisition: ${MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH}"
    )
endif()

add_custom_command(
    OUTPUT ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_STAMP}
    COMMAND ${CMAKE_COMMAND}
        "-DGO_EXECUTABLE=${GO_EXECUTABLE}"
        "-DKUBO_SOURCE=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_SOURCE}"
        "-DKUBO_OUTPUT_DIR=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_DIR}"
        "-DKUBO_OUTPUT_BINARY=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_BINARY}"
        "-DKUBO_OUTPUT_NAME=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME}"
        "-DKUBO_VERSION=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_VERSION}"
        "-DKUBO_GOOS=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOOS}"
        "-DKUBO_GOARCH=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_GOARCH}"
        "-DKUBO_STAMP=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_STAMP}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AcquireBundledKubo.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AcquireBundledKubo.cmake
    COMMENT "Bundling Kubo/IPFS binary into Matrix Media Share Client"
    VERBATIM
)

add_custom_target(MatrixMediaShareClientQtBundledKubo ALL
    DEPENDS ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_STAMP}
)

if(WIN32)
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME ffmpeg.exe)
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME ffprobe.exe)
else()
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME ffmpeg)
    set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME ffprobe)
endif()

set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/bundled-ffmpeg
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY
    ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_DIR}/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME}
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY
    ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_DIR}/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME}
)
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_STAMP
    ${CMAKE_CURRENT_BINARY_DIR}/matrix_media_share_client_bundled_ffmpeg.stamp
)

set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL "")
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY_URL "")
set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY_URL "")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "amd64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY_URL
            "https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffmpeg.zip"
        )
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY_URL
            "https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffprobe.zip"
        )
    elseif(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "arm64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY_URL
            "https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffmpeg.zip"
        )
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY_URL
            "https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffprobe.zip"
        )
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "amd64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz"
        )
    elseif(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "arm64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz"
        )
    endif()
elseif(WIN32)
    if(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "amd64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
        )
    elseif(MATRIX_MEDIA_SHARE_CLIENT_TARGET_ARCH_NORMALIZED STREQUAL "arm64")
        set(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL
            "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-winarm64-gpl.zip"
        )
    endif()
endif()

add_custom_command(
    OUTPUT ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_STAMP}
    COMMAND ${CMAKE_COMMAND}
        "-DFFMPEG_SOURCE_ROOT=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT}"
        "-DFFMPEG_ARCHIVE_URL=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ARCHIVE_URL}"
        "-DFFMPEG_BINARY_URL=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY_URL}"
        "-DFFPROBE_BINARY_URL=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY_URL}"
        "-DFFMPEG_OUTPUT_DIR=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_DIR}"
        "-DFFMPEG_OUTPUT_BINARY=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY}"
        "-DFFPROBE_OUTPUT_BINARY=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY}"
        "-DFFMPEG_OUTPUT_NAME=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME}"
        "-DFFPROBE_OUTPUT_NAME=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME}"
        "-DFFMPEG_STAMP=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_STAMP}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AcquireBundledFfmpeg.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AcquireBundledFfmpeg.cmake
    COMMENT "Bundling FFmpeg tools into Matrix Media Share Client"
    VERBATIM
)

add_custom_target(MatrixMediaShareClientQtBundledFfmpeg ALL
    DEPENDS ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_STAMP}
)
set(MATRIX_MEDIA_ARCHIVER_BACKEND_STAMP
    ${CMAKE_CURRENT_BINARY_DIR}/matrix_media_archiver_backend.stamp
)
string(JOIN "__MATRIX_MEDIA_ARCHIVER_ARGSEP__" MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS_SERIALIZED ${MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS})

add_custom_command(
    OUTPUT ${MATRIX_MEDIA_ARCHIVER_BACKEND_STAMP}
    COMMAND ${CMAKE_COMMAND}
        "-DCARGO_EXECUTABLE=${CARGO_EXECUTABLE}"
        "-DCARGO_BIN_DIR=${CARGO_BIN_DIR}"
        "-DCARGO_MANIFEST_PATH=${MATRIX_MEDIA_ARCHIVER_BACKEND_DIR}/Cargo.toml"
        "-DCARGO_WORKING_DIRECTORY=${MATRIX_MEDIA_ARCHIVER_BACKEND_DIR}"
        "-DCARGO_STAMP_FILE=${MATRIX_MEDIA_ARCHIVER_BACKEND_STAMP}"
        "-DCARGO_ARGS=${MATRIX_MEDIA_ARCHIVER_BACKEND_CARGO_ARGS_SERIALIZED}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRustBackend.cmake
    DEPENDS ${MATRIX_MEDIA_ARCHIVER_BACKEND_SOURCES}
    WORKING_DIRECTORY ${MATRIX_MEDIA_ARCHIVER_BACKEND_DIR}
    COMMENT "Building Rust Matrix backend"
    VERBATIM
)

add_custom_target(MatrixMediaShareClientQtBackend ALL
    DEPENDS ${MATRIX_MEDIA_ARCHIVER_BACKEND_STAMP}
)

set(MATRIX_MEDIA_ARCHIVER_QT_SOURCES
    src/AppController.cpp
    src/AppController.h
    src/AppDatabase.cpp
    src/AppDatabase.h
    src/AppPaths.cpp
    src/AppPaths.h
    src/Domain.h
    src/FolderNaming.cpp
    src/FolderNaming.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MatrixClientBackend.h
    src/MediaClassification.cpp
    src/MediaClassification.h
    src/ProcessMatrixClientBackend.cpp
    src/ProcessMatrixClientBackend.h
    src/SecretStore.cpp
    src/SecretStore.h
    src/VideoFrameWidget.cpp
    src/VideoFrameWidget.h
    src/VlcPlayerWidget.cpp
    src/VlcPlayerWidget.h
    src/WebVideoWidget.h
    src/main.cpp
    ${MATRIX_MEDIA_ARCHIVER_APP_ICON_QRC}
)

if(APPLE)
    list(APPEND MATRIX_MEDIA_ARCHIVER_QT_SOURCES
        src/WebVideoWidget.mm
        ${MATRIX_MEDIA_ARCHIVER_APP_ICON_ICNS}
    )
    set_source_files_properties(${MATRIX_MEDIA_ARCHIVER_APP_ICON_ICNS}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_source_files_properties(src/WebVideoWidget.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    list(APPEND MATRIX_MEDIA_ARCHIVER_QT_SOURCES
        src/WebVideoWidget.cpp
    )
endif()

if(WIN32)
    list(APPEND MATRIX_MEDIA_ARCHIVER_QT_SOURCES
        ${MATRIX_MEDIA_ARCHIVER_APP_ICON_RC}
    )
endif()

qt_add_executable(MatrixMediaShareClientQt
    WIN32
    MACOSX_BUNDLE
    ${MATRIX_MEDIA_ARCHIVER_QT_SOURCES}
)

target_include_directories(MatrixMediaShareClientQt PRIVATE src)
add_dependencies(MatrixMediaShareClientQt MatrixMediaShareClientQtBackend)
add_dependencies(MatrixMediaShareClientQt MatrixMediaShareClientQtBundledKubo)
add_dependencies(MatrixMediaShareClientQt MatrixMediaShareClientQtBundledFfmpeg)

target_link_libraries(MatrixMediaShareClientQt
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Sql
        Qt6::Multimedia
        Qt6::MultimediaWidgets
)

target_compile_definitions(MatrixMediaShareClientQt
    PRIVATE MATRIX_MEDIA_ARCHIVER_VERSION_STRING="${MATRIX_MEDIA_ARCHIVER_APP_VERSION}"
)

if(APPLE)
    find_library(MATRIX_MEDIA_ARCHIVER_WEBKIT_FRAMEWORK WebKit)
    if(MATRIX_MEDIA_ARCHIVER_WEBKIT_FRAMEWORK)
        target_link_libraries(MatrixMediaShareClientQt PRIVATE ${MATRIX_MEDIA_ARCHIVER_WEBKIT_FRAMEWORK})
    endif()
    set_target_properties(MatrixMediaShareClientQt PROPERTIES
        MACOSX_BUNDLE_ICON_FILE MatrixMediaArchiverQt.icns
    )
endif()

add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY}
        $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/${MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY_NAME}
    COMMENT "Copying Rust Matrix backend next to MatrixMediaShareClientQt"
)

if(APPLE)
    add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/kubo
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/kubo/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME}
        COMMENT "Copying bundled Kubo into MatrixMediaShareClientQt.app"
    )
    add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/ffmpeg
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/ffmpeg/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/ffmpeg/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME}
        COMMENT "Copying bundled FFmpeg tools into MatrixMediaShareClientQt.app"
    )
    if(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins")
        add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc/lib
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins"
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc/plugins
            COMMAND ${CMAKE_COMMAND}
                -DVLC_LIB_DIR=$<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc/lib
                -DVLC_PLUGIN_DIR=$<TARGET_FILE_DIR:MatrixMediaShareClientQt>/../Resources/vlc/plugins
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FixBundledVlcMac.cmake
            COMMENT "Copying bundled VLC runtime into MatrixMediaShareClientQt.app"
        )
    endif()
else()
    add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/kubo
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/kubo/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_NAME}
        COMMENT "Copying bundled Kubo next to MatrixMediaShareClientQt"
    )
    add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/ffmpeg
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/ffmpeg/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY}
            $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/ffmpeg/${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_NAME}
        COMMENT "Copying bundled FFmpeg tools next to MatrixMediaShareClientQt"
    )
    if(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins")
        add_custom_command(TARGET MatrixMediaShareClientQt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/vlc
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/vlc
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/vlc/lib
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins"
                $<TARGET_FILE_DIR:MatrixMediaShareClientQt>/vlc/plugins
            COMMENT "Copying bundled VLC runtime next to MatrixMediaShareClientQt"
        )
    endif()
endif()

install(TARGETS MatrixMediaShareClientQt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(NOT APPLE)
    install(PROGRAMS ${MATRIX_MEDIA_ARCHIVER_BACKEND_BINARY}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(PROGRAMS ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_KUBO_BINARY}
        DESTINATION ${CMAKE_INSTALL_BINDIR}/kubo
    )
    install(PROGRAMS
        ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_BINARY}
        ${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFPROBE_BINARY}
        DESTINATION ${CMAKE_INSTALL_BINDIR}/ffmpeg
    )
    if(MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
        AND EXISTS "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins")
        install(DIRECTORY "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/lib"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/vlc
        )
        install(DIRECTORY "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}/plugins"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/vlc
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/MatrixMediaShareClientQt.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES ${MATRIX_MEDIA_ARCHIVER_APP_ICON_PNG}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
    )
endif()

if(MATRIX_MEDIA_ARCHIVER_ENABLE_DEPLOYMENT)
    qt_generate_deploy_app_script(
        TARGET MatrixMediaShareClientQt
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
endif()

if(MATRIX_MEDIA_ARCHIVER_BUILD_TESTS)
    qt_add_executable(MatrixMediaArchiverQtTests
        tests/PortUtilityTests.cpp
        src/Domain.h
        src/FolderNaming.cpp
        src/FolderNaming.h
        src/MediaClassification.cpp
        src/MediaClassification.h
    )

    target_include_directories(MatrixMediaArchiverQtTests PRIVATE src)

    target_link_libraries(MatrixMediaArchiverQtTests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    enable_testing()
    add_test(NAME MatrixMediaArchiverQtTests COMMAND MatrixMediaArchiverQtTests)
endif()
