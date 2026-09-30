# Berry script runtime. The Berry sources are a git submodule (berry/);
# their constant tables are generated at build time with Berry's own tool
# (tools/coc) from the sources and port/berry_conf.h.
set(BERRY_DIR ${CMAKE_CURRENT_LIST_DIR}/berry)

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS ${BERRY_DIR}/src/berry.h)
    message(FATAL_ERROR "Berry sources missing: run `git submodule update --init` in the repository")
endif()

# Berry includes its generated headers as "../generate/x.h" relative to
# src/. With <gen>/anchor on the include path, that resolves to <gen>/generate.
set(GEN_ROOT ${CMAKE_CURRENT_BINARY_DIR}/berry_gen)
set(GEN_DIR ${GEN_ROOT}/generate)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(MAKE_DIRECTORY ${GEN_ROOT}/anchor ${GEN_DIR})
endif()

file(GLOB BERRY_SRCS ${BERRY_DIR}/src/*.c)
set(PORT_SRCS port/be_port.c port/be_modtab.c)

idf_component_register(
    SRCS ${BERRY_SRCS} ${PORT_SRCS} script_runtime.c script_store.c script_web.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS . berry/src port ${GEN_ROOT}/anchor
    EMBED_TXTFILES prelude.be script.js
    REQUIRES esp_http_server
    PRIV_REQUIRES wifi_setup esp_timer esp_partition nvs_flash esp_rom)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT ${GEN_DIR}/be_const_strtab.h
    COMMAND ${python} ${BERRY_DIR}/tools/coc/coc -o ${GEN_DIR} ${BERRY_DIR}/src ${CMAKE_CURRENT_LIST_DIR}/port
            -c ${CMAKE_CURRENT_LIST_DIR}/port/berry_conf.h
    DEPENDS ${BERRY_SRCS} ${PORT_SRCS} port/berry_conf.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Generating Berry constant tables"
    VERBATIM)
add_custom_target(berry_coc DEPENDS ${GEN_DIR}/be_const_strtab.h)
add_dependencies(${COMPONENT_LIB} berry_coc)

# Upstream code: keep its known harmless warnings from failing the build.
set_source_files_properties(${BERRY_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable")
