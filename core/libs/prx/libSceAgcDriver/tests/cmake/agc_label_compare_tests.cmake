add_test_executable(agc_label_compare_tests EXCLUDE_FROM_ALL tests/LabelCompare.cpp)
target_include_directories(agc_label_compare_tests PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(agc_label_compare_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_host_test_executable(agc_label_compare_tests)

if(BUILD_TESTING)
    agc_add_default_test(agc_label_compare_tests)
endif()
