if(BUILD_TESTING AND TARGET agc_driver_guest_buffer_counters_tests)
    add_test(NAME agc_driver_guest_buffer_counters_unpipelined COMMAND agc_driver_guest_buffer_counters_tests)
    set_tests_properties(agc_driver_guest_buffer_counters_unpipelined PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINED_DRAWS=0" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_guest_buffer_counters_unpipelined PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
