# ======================================================================
#               dmview - the display service
# ======================================================================
# One instance per display, started by the device rule in
# configs/dmview.rules from configs/dmview@.ini.
set(DMOD_MODULE_NAME        dmview)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         4096)
set(DMOD_PRIORITY           1)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmview.dmr)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/main.c
)

# dmdrvi:     the display, input and device file system ioctls (headers).
# dmosi:      tick count, the stop semaphore.
# dmvfs:      (+ dmfsi, its headers) flush of the display after drawing (the driver cleans the data cache).
# libsystemd: graceful stop.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.5
    dmfsi
    dmosi
    dmvfs
    libsystemd
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmview_if
)
