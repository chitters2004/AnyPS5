if(BUILD_TESTING)
    add_test_executable(agc_descriptor_recycle_budget_tests
        tests/DescriptorRecycleBudget.cpp
        $<FILTER:$<TARGET_OBJECTS:libSceAgcDriver>,INCLUDE,/Graphics/src/ShaderResources\\.cpp\\.>)
    target_include_directories(agc_descriptor_recycle_budget_tests PRIVATE
        $<TARGET_PROPERTY:libSceAgcDriver,INCLUDE_DIRECTORIES>)
    target_link_libraries(agc_descriptor_recycle_budget_tests PRIVATE libSceAgcDriver shader_recompiler_agc_driver libc libkernel)
    configure_windows_unwind(agc_descriptor_recycle_budget_tests)
    add_test(NAME agc_descriptor_recycle_budget COMMAND agc_descriptor_recycle_budget_tests)
    set_tests_properties(agc_descriptor_recycle_budget PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_descriptor_recycle_budget PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
