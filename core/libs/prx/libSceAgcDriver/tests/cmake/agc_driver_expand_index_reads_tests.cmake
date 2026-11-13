if(BUILD_TESTING AND TARGET agc_driver_pixel_barycentric_emulation_tests)
    add_test(NAME agc_driver_pixel_barycentric_emulation_expand_verify COMMAND agc_driver_pixel_barycentric_emulation_tests)
    set_tests_properties(agc_driver_pixel_barycentric_emulation_expand_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_EXPAND_READS=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_pixel_barycentric_emulation_expand_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
