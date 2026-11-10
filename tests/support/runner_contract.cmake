if(NOT DEFINED RUNNER OR NOT DEFINED CTEST_COMMAND OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "Runner contract requires RUNNER, CTEST_COMMAND and TEST_DIR")
endif()

function(check_exit name expected_exit expect_skip)
    execute_process(COMMAND "${RUNNER}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
    if(NOT "${result}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR "${name}: expected exit ${expected_exit}, got ${result}\n${output}\n${errors}")
    endif()
    if(expect_skip)
        if(NOT errors MATCHES "LILTVIEW_ALL_TESTS_SKIPPED")
            message(FATAL_ERROR "${name}: missing all-skipped marker on stderr")
        endif()
    elseif(errors MATCHES "LILTVIEW_ALL_TESTS_SKIPPED")
        message(FATAL_ERROR "${name}: a non-skipped run must not emit the skip marker")
    endif()
    if(output MATCHES "LILTVIEW_ALL_TESTS_SKIPPED")
        message(FATAL_ERROR "${name}: skip marker polluted reporter stdout")
    endif()
endfunction()

check_exit(skip 0 TRUE "[runner_skip]")
check_exit(skip_xml 0 TRUE "[runner_skip]" --reporter xml)
check_exit(pass 0 FALSE "[runner_pass]")
check_exit(failure 42 FALSE "[runner_fail]")
check_exit(mixed_skip_pass 0 FALSE "[runner_skip],[runner_pass]")
check_exit(mixed_skip_failure 42 FALSE "[runner_skip],[runner_fail]")
check_exit(no_match 2 FALSE "[runner_missing]")
check_exit(unmatched_spec 3 FALSE "[runner_pass],[runner_missing]" --warn UnmatchedTestSpec)
check_exit(bad_argument 1 FALSE --liltview-invalid-argument)
check_exit(bad_spec 5 FALSE "[runner_skip,runner_pass]")

# Verify CTest retains Skipped, rather than turning an all-skipped exit 0 into
# Passed. Isolate this intentional skip from the real test suite.
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/CTestTestfile.cmake"
    "add_test(expected_skip [==[${RUNNER}]==] [==[[runner_skip]]==])\n"
    "set_tests_properties(expected_skip PROPERTIES SKIP_REGULAR_EXPRESSION LILTVIEW_ALL_TESTS_SKIPPED)\n")
execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${TEST_DIR}"
    --output-on-failure --output-junit "${TEST_DIR}/status.xml"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "CTest rejected the skipped run: ${result}\n${output}\n${errors}")
endif()
file(READ "${TEST_DIR}/status.xml" status)
if(NOT status MATCHES "<skipped")
    message(FATAL_ERROR "CTest lost the Skipped status:\n${status}")
endif()
