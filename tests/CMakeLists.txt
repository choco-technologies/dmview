# ======================================================================
#               test_libdmview
# ======================================================================
# The views the test draws are fixtures/*.dmvs, assembled with todmv by
# run_tests.sh before the test module runs.
set(DMOD_MODULE_NAME        test_libdmview)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         8192)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    libdmview_test.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.4
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmview_if
)

target_compile_definitions(${DMOD_MODULE_NAME} PRIVATE
    LIBDMVIEW_FIXTURES_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures"
)

find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh
            ${DMOD_DMF_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${CMAKE_CURRENT_BINARY_DIR}/fixtures
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()
