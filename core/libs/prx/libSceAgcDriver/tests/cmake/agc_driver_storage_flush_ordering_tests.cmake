if(BUILD_TESTING)
    add_test(NAME agc_driver_storage_flush_ordering_label_compare COMMAND agc_driver_storage_flush_ordering_tests)
    set_tests_properties(agc_driver_storage_flush_ordering_label_compare PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_LABEL_COMPARE_SHADOW=any" SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "label-compare\\] mismatch" TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_storage_flush_ordering_label_compare PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
