if(BUILD_TESTING)
    set_tests_properties(agc_driver_storage_format_alias PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_DRAW=1" SKIP_RETURN_CODE 77 TIMEOUT 60
        PASS_REGULAR_EXPRESSION "format aliases \\(10 s, APS5_PROFILE_DRAW\\): 0 guest ranges under storage images of different formats now \\(0 images[^\n]*top range 0x0\\+0x0 formats none[^\n]*\n.*format aliases \\(10 s, APS5_PROFILE_DRAW\\): 1 guest ranges under storage images of different formats now \\(2 images, 0\\.1 MiB. 1 ranges with every format the same texel size\\). [0-9]+ uploads[^\n]*since start 3 uploads \\(0\\.2 MiB\\), 2 after a write-back[^\n]*\n.*storage format alias census tests passed \\(counted\\)"
        FAIL_REGULAR_EXPRESSION "VUID-")
    add_test(NAME agc_driver_storage_format_alias_unprofiled COMMAND agc_driver_storage_format_alias_tests)
    set_tests_properties(agc_driver_storage_format_alias_unprofiled PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 60
        PASS_REGULAR_EXPRESSION "storage format alias census tests passed \\(not counted\\)"
        FAIL_REGULAR_EXPRESSION "format aliases|VUID-")
    if(WIN32)
        set_tests_properties(agc_driver_storage_format_alias_unprofiled PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
