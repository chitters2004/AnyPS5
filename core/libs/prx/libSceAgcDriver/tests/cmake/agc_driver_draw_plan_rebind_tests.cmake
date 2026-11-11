if(BUILD_TESTING)
    add_test(NAME agc_driver_draw_plan_rebind_verify COMMAND agc_driver_draw_plan_rebind_tests)
    set_tests_properties(agc_driver_draw_plan_rebind_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_DRAW_PLANS=1" SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "FATAL:")
    if(WIN32)
        set_tests_properties(agc_driver_draw_plan_rebind_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
