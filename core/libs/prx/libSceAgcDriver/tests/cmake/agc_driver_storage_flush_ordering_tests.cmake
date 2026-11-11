if(BUILD_TESTING)
    add_test(NAME agc_driver_storage_flush_ordering_no_import COMMAND agc_driver_storage_flush_ordering_tests --stored-again)
    set_tests_properties(agc_driver_storage_flush_ordering_no_import PROPERTIES ENVIRONMENT "APS5_NO_HOST_IMPORT=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME agc_driver_storage_flush_ordering_verify COMMAND agc_driver_storage_flush_ordering_tests)
    set_tests_properties(agc_driver_storage_flush_ordering_verify PROPERTIES ENVIRONMENT "APS5_LABEL_STORE_VERIFY=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME agc_driver_storage_flush_ordering_stamped_watched COMMAND agc_driver_storage_flush_ordering_tests --stamped)
    set_tests_properties(agc_driver_storage_flush_ordering_stamped_watched PROPERTIES ENVIRONMENT "APS5_WRITE_WATCH_IMPORTS=watch" SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME agc_driver_storage_flush_ordering_stamped_verify COMMAND agc_driver_storage_flush_ordering_tests --stamped)
    set_tests_properties(agc_driver_storage_flush_ordering_stamped_verify PROPERTIES ENVIRONMENT "APS5_WRITE_WATCH_IMPORTS=watch;APS5_LABEL_STORE_VERIFY=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_storage_flush_ordering_stamped_watched PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_driver_storage_flush_ordering_stamped_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_driver_storage_flush_ordering_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_driver_storage_flush_ordering_no_import PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
