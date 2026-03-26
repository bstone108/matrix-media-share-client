if(NOT DEFINED VLC_LIB_DIR OR VLC_LIB_DIR STREQUAL "")
    message(FATAL_ERROR "VLC_LIB_DIR is required")
endif()
if(NOT DEFINED VLC_PLUGIN_DIR OR VLC_PLUGIN_DIR STREQUAL "")
    message(FATAL_ERROR "VLC_PLUGIN_DIR is required")
endif()

function(_fix_install_name)
    execute_process(
        COMMAND /usr/bin/install_name_tool ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
    )
    if(NOT _result EQUAL 0)
        string(STRIP "${_stdout}" _stdout)
        string(STRIP "${_stderr}" _stderr)
        message(FATAL_ERROR
            "install_name_tool failed (${_result}) for args: ${ARGN}\nstdout: ${_stdout}\nstderr: ${_stderr}")
    endif()
endfunction()

function(_ad_hoc_sign _file)
    execute_process(
        COMMAND /usr/bin/xattr -cr "${_file}"
        RESULT_VARIABLE _xattr_result
        OUTPUT_VARIABLE _xattr_stdout
        ERROR_VARIABLE _xattr_stderr
    )
    if(NOT _xattr_result EQUAL 0)
        string(STRIP "${_xattr_stdout}" _xattr_stdout)
        string(STRIP "${_xattr_stderr}" _xattr_stderr)
        message(FATAL_ERROR
            "xattr failed (${_xattr_result}) for ${_file}\nstdout: ${_xattr_stdout}\nstderr: ${_xattr_stderr}")
    endif()

    execute_process(
        COMMAND /usr/bin/codesign --force --sign - "${_file}"
        RESULT_VARIABLE _sign_result
        OUTPUT_VARIABLE _sign_stdout
        ERROR_VARIABLE _sign_stderr
    )
    if(NOT _sign_result EQUAL 0)
        string(STRIP "${_sign_stdout}" _sign_stdout)
        string(STRIP "${_sign_stderr}" _sign_stderr)
        message(FATAL_ERROR
            "codesign failed (${_sign_result}) for ${_file}\nstdout: ${_sign_stdout}\nstderr: ${_sign_stderr}")
    endif()
endfunction()

file(GLOB _vlc_libs "${VLC_LIB_DIR}/*.dylib")
foreach(_file IN LISTS _vlc_libs)
    get_filename_component(_name "${_file}" NAME)
    _fix_install_name(-id "@loader_path/${_name}" "${_file}")
    _fix_install_name(-add_rpath "@loader_path" "${_file}")
    _fix_install_name(-change "@rpath/libvlccore.dylib" "@loader_path/libvlccore.dylib" "${_file}")
    _fix_install_name(-change "@rpath/libvlc.dylib" "@loader_path/libvlc.dylib" "${_file}")
endforeach()

file(GLOB_RECURSE _vlc_plugins "${VLC_PLUGIN_DIR}/*.dylib")
foreach(_file IN LISTS _vlc_plugins)
    get_filename_component(_name "${_file}" NAME)
    get_filename_component(_plugin_dir "${_file}" DIRECTORY)
    file(RELATIVE_PATH _plugin_to_lib "${_plugin_dir}" "${VLC_LIB_DIR}")
    if(_plugin_to_lib STREQUAL "")
        set(_plugin_to_lib ".")
    endif()
    _fix_install_name(-id "@loader_path/${_name}" "${_file}")
    _fix_install_name(-add_rpath "@loader_path/${_plugin_to_lib}" "${_file}")
    _fix_install_name(-change "@rpath/libvlccore.dylib" "@loader_path/${_plugin_to_lib}/libvlccore.dylib" "${_file}")
    _fix_install_name(-change "@rpath/libvlc.dylib" "@loader_path/${_plugin_to_lib}/libvlc.dylib" "${_file}")
endforeach()

foreach(_file IN LISTS _vlc_libs)
    _ad_hoc_sign("${_file}")
endforeach()

foreach(_file IN LISTS _vlc_plugins)
    _ad_hoc_sign("${_file}")
endforeach()
