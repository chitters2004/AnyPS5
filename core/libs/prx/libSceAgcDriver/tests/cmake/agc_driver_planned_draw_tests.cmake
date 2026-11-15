if(BUILD_TESTING)
    add_test(NAME agc_driver_planned_draw_plan_verify COMMAND agc_driver_planned_draw_tests)
    set_tests_properties(agc_driver_planned_draw_plan_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_DRAW_PLAN=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_draw_sequence_committer_prepare COMMAND agc_driver_draw_sequence_tests)
    set_tests_properties(agc_driver_draw_sequence_committer_prepare PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        ENVIRONMENT "APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=8;APS5_RING_PREPARERS=3;APS5_PIPELINE_STATS=1;APS5_RING_COMMITTER_PREPARE=1")
    add_test(NAME agc_driver_draw_cache_reuse_committer_prepare COMMAND agc_driver_draw_cache_reuse_tests)
    set_tests_properties(agc_driver_draw_cache_reuse_committer_prepare PROPERTIES SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "FATAL:" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=32;APS5_PIPELINE_DISPATCH=1;APS5_RING_PREPARERS=3;APS5_RING_COMMITTER_PREPARE=1;APS5_VERIFY_DRAW_PLAN=1;APS5_VERIFY_DRAW_RECIPE=1;APS5_VERIFY_REGISTER_KEYS=1")
    if(WIN32)
        set_tests_properties(agc_driver_planned_draw_plan_verify agc_driver_draw_cache_reuse_committer_prepare PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_driver_draw_sequence_committer_prepare PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAgcDriver>")
    endif()
endif()
