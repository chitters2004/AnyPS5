if(BUILD_TESTING)
    add_test(NAME agc_driver_inert_fragment_pipeline_kept COMMAND agc_driver_inert_fragment_pipeline_tests)
    add_test(NAME agc_driver_inert_fragment_pipeline_no_gpl COMMAND agc_driver_inert_fragment_pipeline_tests)
    add_test(NAME agc_driver_inert_fragment_pipeline_gputime COMMAND agc_driver_inert_fragment_pipeline_tests)
    add_test(NAME agc_driver_inert_fragment_pipeline_gputime_kept COMMAND agc_driver_inert_fragment_pipeline_tests)
    set_tests_properties(agc_driver_inert_fragment_pipeline_kept PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_KEEP_INERT_FRAGMENT=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_inert_fragment_pipeline_no_gpl PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_GPL=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_inert_fragment_pipeline_gputime PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_GPU=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_inert_fragment_pipeline_gputime_kept PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_GPU=1;APS5_KEEP_INERT_FRAGMENT=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_inert_fragment_pipeline_kept agc_driver_inert_fragment_pipeline_no_gpl agc_driver_inert_fragment_pipeline_gputime agc_driver_inert_fragment_pipeline_gputime_kept PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
