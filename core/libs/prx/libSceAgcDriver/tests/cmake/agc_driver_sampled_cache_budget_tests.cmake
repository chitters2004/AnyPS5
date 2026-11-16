if(BUILD_TESTING)
    add_test(NAME agc_driver_sampled_cache_budget_memory COMMAND agc_driver_sampled_cache_budget_tests memory)
    set_tests_properties(agc_driver_sampled_cache_budget_memory PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77)
    add_test(NAME agc_driver_sampled_cache_budget_without_storage_image_multisample COMMAND agc_driver_sampled_cache_budget_tests without-storage-image-multisample)
    set_tests_properties(agc_driver_sampled_cache_budget_without_storage_image_multisample PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_sampled_cache_device_selection COMMAND agc_driver_sampled_cache_budget_tests device-selection-cpu)
    set_tests_properties(agc_sampled_cache_device_selection PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_driver_sampled_cache_device_selection COMMAND agc_driver_sampled_cache_budget_tests device-selection)
    set_tests_properties(agc_driver_sampled_cache_device_selection PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_sampled_cache_device_selection agc_driver_sampled_cache_device_selection PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    foreach(imageRecoveryMode IN ITEMS sampled storage srgb repack)
        add_test(NAME agc_driver_recover_${imageRecoveryMode}_allocation COMMAND agc_driver_sampled_cache_budget_tests recover-${imageRecoveryMode})
        set_tests_properties(agc_driver_recover_${imageRecoveryMode}_allocation PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_IMAGE_SLABS=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
        if(WIN32)
            set_tests_properties(agc_driver_recover_${imageRecoveryMode}_allocation PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test(NAME agc_driver_sampled_cache_budget_report_age COMMAND agc_driver_sampled_cache_budget_tests budget-report-age)
    set_tests_properties(agc_driver_sampled_cache_budget_report_age PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TEXTURE_CACHE_MIB=" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "active storage image cache budget 4096 MiB.*storage_limit_age_ms=[1-9][0-9]*"
        FAIL_REGULAR_EXPRESSION "FAIL:|VUID-")
    if(WIN32)
        set_tests_properties(agc_driver_sampled_cache_budget_report_age PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_property(TEST agc_driver_recover_srgb_allocation APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_SRGB_SHADER_DECODE=set:copy")
    add_test(NAME agc_driver_resource_cache_storage_admission COMMAND agc_driver_sampled_cache_budget_tests resource-admission)
    add_test(NAME agc_driver_resource_cache_oversized_storage_admission COMMAND agc_driver_sampled_cache_budget_tests resource-oversized)
    add_test(NAME agc_driver_resource_cache_storage_clear COMMAND agc_driver_sampled_cache_budget_tests resource-clear)
    add_test(NAME agc_driver_resource_cache_replaced_texture COMMAND agc_driver_sampled_cache_budget_tests resource-replaced)
    add_test(NAME agc_driver_resource_cache_released_epoch COMMAND agc_driver_sampled_cache_budget_tests resource-released-epoch)
    add_test(NAME agc_driver_resource_cache_unwatched_epoch COMMAND agc_driver_sampled_cache_budget_tests resource-unwatched-epoch)
    add_test(NAME agc_driver_resource_cache_epoch_reuse COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-reuse)
    add_test(NAME agc_driver_resource_cache_epoch_verify_work COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-verify-work)
    set_tests_properties(agc_driver_resource_cache_epoch_verify_work PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_DRAW_PIPELINE=1;APS5_WRITE_WATCH_IMPORTS=watch" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "verify: the full proof behind a reused proof did work the exit skipped: 0 CPU write syncs, 1 storage image uploads[^\n]*\n.*verify: 2 reused resource proofs checked, 0 failed, 0 where the full proof stored to guest memory, 0 where only other threads broke proofs meanwhile, 1 where the full proof did work the exit skipped.*resource cache epoch verify work tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    if(WIN32)
        set_tests_properties(agc_driver_resource_cache_epoch_verify_work PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_resource_cache_epoch_proof_log COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log: a marked-written break outside the set: the exit was taken\n.*proof log: outside breaks kept the exit: marked written yes, packet yes\n.*missed exits by the inputs that moved: proof 4; would hit if only that input were scoped: epoch 0, pending 0, releases 0, proof 4; with the proof scoped by the break log and only that input too: epoch 0, pending 0, releases 0\n[^\n]*break log: 2 exits taken because every break since the proof was outside the set \\(on\\); over the proof-moved misses: 4 checked, clears 0 \\(0 with nothing else moved\\), a set without recorded ranges 0, a window the log cannot cover 0; blocked by an overlapping break: marked written 1, non-draw packet 1; by an unranged break: driver store 1, non-draw packet 1\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_off COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1;APS5_NO_SCOPED_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log: a marked-written break outside the set: the exit was refused\n.*proof log: outside breaks kept the exit: marked written no, packet no\n.*missed exits by the inputs that moved: proof 6; would hit if only that input were scoped: epoch 0, pending 0, releases 0, proof 6; with the proof scoped by the break log and only that input too: epoch 0, pending 0, releases 0\n[^\n]*break log: 0 exits taken because every break since the proof was outside the set \\(off, APS5_NO_SCOPED_PROOFS\\); over the proof-moved misses: 6 checked, clears 2 \\(2 with nothing else moved\\), a set without recorded ranges 0, a window the log cannot cover 0; blocked by an overlapping break: marked written 1, non-draw packet 1; by an unranged break: driver store 1, non-draw packet 1\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_verify COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_DRAW_PIPELINE=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "verify: 8 reused resource proofs checked, 0 failed, 0 where the full proof stored to guest memory, 0 where only other threads broke proofs meanwhile, 0 where the full proof did work the exit skipped; 4 proved builds missed the exit[^\n]*\n.*proof log: outside breaks kept the exit: marked written yes, packet yes\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:|[1-9][0-9]* failed|verify: the full proof behind a reused proof")
    if(WIN32)
        set_tests_properties(agc_driver_resource_cache_epoch_proof_log agc_driver_resource_cache_epoch_proof_log_off agc_driver_resource_cache_epoch_proof_log_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_resource_cache_lookup_changed COMMAND agc_driver_sampled_cache_budget_tests resource-lookup-changed)
    add_test(NAME agc_driver_resource_cache_collect_refusals COMMAND agc_driver_sampled_cache_budget_tests collect-refusals)
    add_test(NAME agc_driver_resource_cache_collect_refusals_index COMMAND agc_driver_sampled_cache_budget_tests collect-refusals)
    add_test(NAME agc_driver_resource_cache_collect_refusals_narrow COMMAND agc_driver_sampled_cache_budget_tests collect-refusals)
    add_test(NAME agc_driver_resource_cache_pending_fail_count COMMAND agc_driver_sampled_cache_budget_tests pending-fail-count)
    add_test(NAME agc_driver_resource_cache_pending_fail_count_index COMMAND agc_driver_sampled_cache_budget_tests pending-fail-count)
    set_tests_properties(agc_driver_resource_cache_storage_admission agc_driver_resource_cache_oversized_storage_admission agc_driver_resource_cache_storage_clear agc_driver_resource_cache_replaced_texture agc_driver_resource_cache_released_epoch agc_driver_resource_cache_unwatched_epoch agc_driver_resource_cache_epoch_reuse agc_driver_resource_cache_lookup_changed agc_driver_resource_cache_collect_refusals PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    set_tests_properties(agc_driver_resource_cache_collect_refusals_index PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TABLE_INDEX=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    set_tests_properties(agc_driver_resource_cache_collect_refusals_narrow PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_EXTENDED_FAST_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    set_tests_properties(agc_driver_resource_cache_pending_fail_count PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_WRITE_WATCH_IMPORTS=watch" SKIP_RETURN_CODE 77 TIMEOUT 30)
    set_tests_properties(agc_driver_resource_cache_pending_fail_count_index PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_WRITE_WATCH_IMPORTS=watch;APS5_TABLE_INDEX=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_sampled_cache_budget_memory agc_driver_sampled_cache_budget_without_storage_image_multisample agc_driver_resource_cache_storage_admission agc_driver_resource_cache_oversized_storage_admission agc_driver_resource_cache_storage_clear agc_driver_resource_cache_replaced_texture agc_driver_resource_cache_released_epoch agc_driver_resource_cache_epoch_reuse agc_driver_resource_cache_lookup_changed agc_driver_resource_cache_collect_refusals agc_driver_resource_cache_collect_refusals_index agc_driver_resource_cache_collect_refusals_narrow agc_driver_resource_cache_pending_fail_count agc_driver_resource_cache_pending_fail_count_index PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
