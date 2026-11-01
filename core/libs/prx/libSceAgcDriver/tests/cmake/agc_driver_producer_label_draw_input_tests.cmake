if(BUILD_TESTING)
    set_tests_properties(agc_driver_producer_label_draw_input PROPERTIES TIMEOUT 60)
    add_test(NAME agc_driver_producer_label_draw_input_copied COMMAND agc_driver_producer_label_draw_input_tests)
    set_tests_properties(agc_driver_producer_label_draw_input_copied PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_HOST_IMPORT=1" TIMEOUT 60 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_producer_label_draw_input_copied PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
