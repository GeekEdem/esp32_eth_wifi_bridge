# portal_i18n(<dir>...): page languages for the setup portal. Call it in the
# app component's CMakeLists.txt after idf_component_register(); it merges the
# i18n/<code>.json files of the given folders and of wifi_setup itself into a
# generated source that defines portal_langs / portal_lang_count (pass them to
# setup_portal_config_t). A new <code>.json in any of the folders is picked up
# on the next build.
function(portal_i18n)
    idf_build_get_property(python PYTHON)
    set(tool ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools/i18n_bundle.py)
    set(dirs ${ARGN} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/i18n)
    set(deps ${tool})
    foreach(d ${dirs})
        file(GLOB files CONFIGURE_DEPENDS ${d}/*.json)
        list(APPEND deps ${files})
    endforeach()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/portal_i18n.c)
    add_custom_command(OUTPUT ${out}
                       COMMAND ${python} ${tool} c ${out} ${dirs}
                       DEPENDS ${deps}
                       COMMENT "Page languages: ${out}"
                       VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE ${out})
endfunction()
