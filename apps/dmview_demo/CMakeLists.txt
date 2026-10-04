# ======================================================================
#               dmview_demo - the dmodOS example
# ======================================================================
# Shows examples/dmodos on a display while it runs. The view and its fonts
# are converted from examples/dmodos at build time (DMOD_ASSETS_PATHS) and
# packaged as the module's views.
set(DMOD_MODULE_NAME        dmview_demo)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           1)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmview_demo.dmr)
set(DMOD_ASSETS_PATHS       ${PROJECT_SOURCE_DIR}/examples/dmodos ${PROJECT_SOURCE_DIR}/examples/dmodos/fonts)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/main.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.5
    dmosi
    libsystemd
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmview_if
)
