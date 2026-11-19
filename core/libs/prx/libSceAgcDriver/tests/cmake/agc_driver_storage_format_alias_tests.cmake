if(BUILD_TESTING)
    set_tests_properties(agc_driver_storage_format_alias PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_DRAW=1" SKIP_RETURN_CODE 77 TIMEOUT 60
        PASS_REGULAR_EXPRESSION "format aliases \\(10 s, APS5_PROFILE_DRAW\\): 0 guest ranges under storage images of different formats now \\(0 images[^\n]*top range 0x0\\+0x0 formats none[^\n]*\n.*format aliases \\(10 s, APS5_PROFILE_DRAW\\): 1 guest ranges under storage images of different formats now \\(3 images, 0\\.1 MiB. 0 ranges with every format the same texel size\\). 4 uploads[^\n]*since start 4 uploads \\(0\\.[23] MiB\\), 3 after a write-back[^\n]*\n\\[textures\\] format alias range 0 0x[0-9a-f]+\\+0x10000: 3 images, 4 uploads \\([^)]*\\) this window. apart by: 0-1 texel size, 0-2 texel size, 1-2 same layout\n\\[textures\\]   range 0 image 0 [^\n]* format 1/[0-9]+,[^\n]*: 1 uploads [^\n]*\n\\[textures\\]   range 0 image 1 [^\n]* format 20/[0-9]+,[^\n]*: 0 uploads [^\n]*, 2 write-backs this window\n\\[textures\\]   range 0 image 2 [^\n]* format 22/[0-9]+,[^\n]*: 3 uploads [^\n]*, 0 write-backs this window\n.*storage format alias census tests passed \\(counted\\)"
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
