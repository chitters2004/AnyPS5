if(BUILD_TESTING)
    add_test_executable(agc_driver_pipeline_scoping_tests tests/DrawPipelineScoping.cpp)
    target_include_directories(agc_driver_pipeline_scoping_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_driver_pipeline_scoping_tests PRIVATE libSceAgcDriver)
    set_target_properties(agc_driver_pipeline_scoping_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_pipeline_scoping_tests)
    add_test(NAME agc_driver_pipeline_scoping COMMAND agc_driver_pipeline_scoping_tests)
    set_tests_properties(agc_driver_pipeline_scoping PROPERTIES TIMEOUT 20
        ENVIRONMENT "APS5_PIPELINED_DRAWS=8")
    if(WIN32)
        set_tests_properties(agc_driver_pipeline_scoping PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAgcDriver>")
    endif()
endif()
