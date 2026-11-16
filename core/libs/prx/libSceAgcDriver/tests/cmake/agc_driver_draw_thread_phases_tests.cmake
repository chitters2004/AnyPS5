if(BUILD_TESTING)
    add_test(NAME agc_driver_draw_thread_phases_timed COMMAND agc_driver_draw_thread_phases_tests)
    set_tests_properties(agc_driver_draw_thread_phases_timed PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DRAW_THREAD_TIMES=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_draw_thread_phases_timed_draw_pipeline COMMAND agc_driver_draw_thread_phases_tests)
    set_tests_properties(agc_driver_draw_thread_phases_timed_draw_pipeline PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=32;APS5_PIPELINE_DISPATCH=1;APS5_DRAW_THREAD_TIMES=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_draw_thread_phases_timed agc_driver_draw_thread_phases_timed_draw_pipeline PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    agc_add_draw_pipeline_test(agc_driver_draw_thread_phases)
endif()
