# ======================================================================
#               hello - a small application that lives in a rent
# ======================================================================
# A test of the screen rent and of the icons of modules: it has a view (converted from views/ at build time and packaged as the module's
# views), an icon (icons/: the default one, and one for the 1-bit e-paper), and it takes the part of the screen it is given - the rent
# named by `--display NAME` - or the whole display when it is not given one. See docs/rent.md.
set(DMOD_MODULE_NAME        hello)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           1)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/hello.dmr)
set(DMOD_ASSETS_PATHS       ${CMAKE_CURRENT_SOURCE_DIR}/views)
# (the views of every module of this project that has some are converted into one directory by default: this one's are its own)
set(DMOD_VIEWS_DIR          ${CMAKE_BINARY_DIR}/views-hello)
set(DMOD_ICON_PATH          ${CMAKE_CURRENT_SOURCE_DIR}/icons)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/main.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.5
    dmosi
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmview_if
)
