if(BUILD_TESTING)
    foreach(mode IN ITEMS verify disabled)
        set(test agc_driver_guest_buffer_guard_memo_${mode})
        add_test(NAME ${test} COMMAND agc_driver_guest_buffer_guard_memo_tests)
        set_tests_properties(${test} PROPERTIES
                TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
        if(WIN32)
            set_tests_properties(${test} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_driver_guest_buffer_guard_memo_verify APPEND PROPERTY ENVIRONMENT APS5_VERIFY_GUEST_BUFFER_MEMO=1)
    set_property(TEST agc_driver_guest_buffer_guard_memo_disabled APPEND PROPERTY ENVIRONMENT APS5_NO_GUEST_BUFFER_MEMO=1)
endif()
