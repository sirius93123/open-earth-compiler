
configure_lit_site_cfg(
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
        MAIN_CONFIG
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

set(OEC_OPT_TEST_DEPENDS
        FileCheck count not
        oec-opt
        )

add_lit_testsuite(check-oec-opt "Running the oec-opt regression tests"
        ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${OEC_OPT_TEST_DEPENDS}
        )
set_target_properties(check-oec-opt PROPERTIES FOLDER "Tests")

add_lit_testsuites(OEC_OPT ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS ${STANDALONE_OPT_TEST_DEPENDS})


