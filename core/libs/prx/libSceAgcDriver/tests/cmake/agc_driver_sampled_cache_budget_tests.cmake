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
    add_test(NAME agc_driver_resource_cache_epoch_gpu_label COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-gpu-label)
    set_tests_properties(agc_driver_resource_cache_epoch_gpu_label PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_WRITE_WATCH_IMPORTS=watch" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "gpu label: a label stored on the GPU over a sampled view of a storage image kept the exit: submit end no, completion no\n.*gpu label: a label stored on the GPU outside the set kept the exit: yes\n.*resource cache epoch GPU label tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    if(WIN32)
        set_tests_properties(agc_driver_resource_cache_epoch_gpu_label PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_proof_log_concurrent COMMAND agc_driver_sampled_cache_budget_tests proof-log-concurrent)
    set_tests_properties(agc_driver_proof_log_concurrent PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log under 4 breaking threads: [1-9][0-9]* windows checked, [0-9]+ cleared, 0 cleared over a window with breaks into the set.*proof log concurrent tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    if(WIN32)
        set_tests_properties(agc_driver_proof_log_concurrent PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_resource_cache_epoch_proof_log COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log: a marked-written break outside the set: the exit was taken\n.*proof log: outside breaks kept the exit: marked written yes, packet yes\n.*missed exits by the inputs that moved: proof 4[;] would hit if only that input were scoped: epoch 0, pending 0, releases 0, proof 4[;] with the proof scoped by the break log and only that input too: epoch 0, pending 0, releases 0\n.*break log: 2 exits taken because every break since the proof was outside the set \\(on\\)[;] over the proof-moved misses: 4 checked, clears 0 \\(0 with nothing else moved\\), a set without recorded ranges 0, a window the log cannot cover 0[;] blocked by an overlapping break: marked written 1, non-draw packet 1[;] by an unranged break: driver store 1, non-draw packet 1\n.*proof log: writeless seals kept the exit: yes, flipped no[;] a storing seal kept it: no\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_driver_resource_cache_epoch_scope_levels COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-scope-levels)
    set_tests_properties(agc_driver_resource_cache_epoch_scope_levels PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "scoped would-hit over 5 misses, cumulative: proof 0, \\+releases 0, \\+pending 3, \\+epoch 4 \\(certifiable [0-9]+\\)[;] pending entry log over the pending-moved misses: checked 4, incomplete 0, blocked by an entering bump in range 1[;] releases moved with an own texture released 0\n[^\n]*collect epochs [1-9][0-9]*, sets first proved in a new epoch [1-9][0-9]*[;] GPU-lock holds off the proof thread [0-9]+, idle [0-9]+ \\(no other proof break or pending bump while held\\)\n.*resource cache epoch scope level tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_proof_break_idle_holds COMMAND agc_driver_sampled_cache_budget_tests proof-break-idle-holds)
    set_tests_properties(agc_proof_break_idle_holds PROPERTIES ENVIRONMENT "APS5_PROFILE_EPOCH_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "idle holds: GPU-lock holds off the proof thread 3, idle 1\n.*idle hold tests passed")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_off COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1;APS5_NO_SCOPED_PROOFS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log: a marked-written break outside the set: the exit was refused\n.*proof log: outside breaks kept the exit: marked written no, packet no\n.*missed exits by the inputs that moved: proof 6[;] would hit if only that input were scoped: epoch 0, pending 0, releases 0, proof 6[;] with the proof scoped by the break log and only that input too: epoch 0, pending 0, releases 0\n.*break log: 0 exits taken because every break since the proof was outside the set \\(off, APS5_NO_SCOPED_PROOFS\\)[;] over the proof-moved misses: 6 checked, clears 2 \\(2 with nothing else moved\\), a set without recorded ranges 0, a window the log cannot cover 0[;] blocked by an overlapping break: marked written 1, non-draw packet 1[;] by an unranged break: driver store 1, non-draw packet 1\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_verify COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_DRAW_PIPELINE=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "verify: 8 reused resource proofs checked, 0 failed, 0 where the full proof stored to guest memory, 0 where only other threads broke proofs meanwhile, 0 where the full proof did work the exit skipped[;] 4 proved builds missed the exit[^\n]*\n.*proof log: outside breaks kept the exit: marked written yes, packet yes\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:|[1-9][0-9]* failed|verify: the full proof behind a reused proof")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_census COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_census PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PROFILE_EPOCH_PROOFS=1;APS5_PROOF_BREAK_CENSUS=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "break census over 4 proof-alone misses: 4 walked, 0 past the ring, 0 with a slot the log could not read[;] clear with the walk at the ring 0, and packets without declared writes skipped 1, and exact-range sites scoped 2, and the off-thread GPU mutex skipped 2, and zero-length packet writes skipped 2[;] past the 1024 cap by breaks: up to 2048 0, up to 4095 0, past the ring 0[;] by breaks since the set's previous check: up to 1024 0, up to 4095 0, more 0[;] blocking \\(in the window/only blocker\\): packet completion no declared writes 1/1, packet other overlapping writes 1/1, marked written overlapping 1/1, driver store unscoped outside the set 1/1\n.*non-draw packet breaks by kind \\(declared writes/none declared/zero-length/known to write nothing\\): other 2/0/0/0, completion 0/1/0/0\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    add_test(NAME agc_driver_resource_cache_epoch_proof_log_writeless_off COMMAND agc_driver_sampled_cache_budget_tests resource-epoch-proof-log)
    set_tests_properties(agc_driver_resource_cache_epoch_proof_log_writeless_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_WRITELESS_PACKET_SCOPE=1" SKIP_RETURN_CODE 77 TIMEOUT 30
        PASS_REGULAR_EXPRESSION "proof log: writeless seals kept the exit: no, flipped yes[;] a storing seal kept it: no\n.*resource cache epoch proof log tests passed"
        FAIL_REGULAR_EXPRESSION "AGC graphics:")
    if(WIN32)
        set_tests_properties(agc_driver_resource_cache_epoch_proof_log agc_driver_resource_cache_epoch_proof_log_off agc_driver_resource_cache_epoch_proof_log_verify agc_driver_resource_cache_epoch_proof_log_census agc_driver_resource_cache_epoch_proof_log_writeless_off agc_driver_resource_cache_epoch_scope_levels agc_proof_break_idle_holds PROPERTIES ENVIRONMENT_MODIFICATION
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
