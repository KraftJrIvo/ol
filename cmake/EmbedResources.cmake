function(embed_resources RESOURCE_DIR OUT_VAR)
    cmake_parse_arguments(ARG "" "" "EXCLUDE_EXTENSIONS" ${ARGN})

    file(GLOB_RECURSE RESOURCE_FILES CONFIGURE_DEPENDS RELATIVE "${RESOURCE_DIR}" "${RESOURCE_DIR}/*")

    set(GENERATED_C_FILES)
    set(RESOURCE_DECLS)

    # Build a simple resource tree description alongside the flat symbols.
    set(NODE_COUNT 0)
    set(NODE_PATHS)
    set(NODE_NAMES)
    set(NODE_ISDIRS)
    set(NODE_DATASYMS)
    set(NODE_LENS)
    set(NODE_PARENTS)

    macro(_resource_node_key PATH OUT_KEY)
        if("${PATH}" STREQUAL "")
            set(${OUT_KEY} "ROOT")
        else()
            string(MD5 HASH "${PATH}")
            set(${OUT_KEY} "${HASH}")
        endif()
    endmacro()

    macro(_resource_add_node PATH NAME IS_DIR DATA_SYM LEN_SYM PARENT OUT_INDEX)
        # Ensure list sizes stay aligned even for empty fields.
        set(PATH_VALUE "${PATH}")
        if("${PATH_VALUE}" STREQUAL "")
            set(PATH_VALUE ".")
        endif()
        set(DATA_VALUE "${DATA_SYM}")
        if("${DATA_VALUE}" STREQUAL "")
            set(DATA_VALUE "0")
        endif()
        set(LEN_VALUE "${LEN_SYM}")
        if("${LEN_VALUE}" STREQUAL "")
            set(LEN_VALUE "0")
        endif()
        set(INDEX ${NODE_COUNT})
        math(EXPR NODE_COUNT "${NODE_COUNT} + 1")
        list(APPEND NODE_PATHS "${PATH_VALUE}")
        list(APPEND NODE_NAMES "${NAME}")
        list(APPEND NODE_ISDIRS "${IS_DIR}")
        list(APPEND NODE_DATASYMS "${DATA_VALUE}")
        list(APPEND NODE_LENS "${LEN_VALUE}")
        list(APPEND NODE_PARENTS "${PARENT}")
        set(${OUT_INDEX} ${INDEX})
    endmacro()

    macro(_resource_get_or_add_dir PATH NAME PARENT OUT_INDEX)
        _resource_node_key("${PATH}" KEY)
        if(DEFINED NODE_INDEX_${KEY})
            set(${OUT_INDEX} ${NODE_INDEX_${KEY}})
        else()
            _resource_add_node("${PATH}" "${NAME}" 1 "" "" "${PARENT}" NEW_INDEX)
            set(NODE_INDEX_${KEY} ${NEW_INDEX})
            set(${OUT_INDEX} ${NEW_INDEX})
        endif()
    endmacro()

    macro(_resource_list_set LIST_NAME INDEX VALUE)
        set(_tmp_list "${${LIST_NAME}}")
        list(REMOVE_AT _tmp_list ${INDEX})
        list(INSERT _tmp_list ${INDEX} "${VALUE}")
        set(${LIST_NAME} "${_tmp_list}")
    endmacro()

    get_filename_component(RESOURCE_ROOT_NAME "${RESOURCE_DIR}" NAME)
    _resource_add_node("" "${RESOURCE_ROOT_NAME}" 1 "" "" -1 ROOT_INDEX)
    _resource_node_key("" ROOT_KEY)
    set(NODE_INDEX_${ROOT_KEY} ${ROOT_INDEX})

    foreach(RESOURCE_FILE ${RESOURCE_FILES})
        get_filename_component(FILE_EXT "${RESOURCE_FILE}" EXT)

        # Exclude extensions
        set(SKIP_FILE FALSE)
        foreach(EXCLUDED_EXT ${ARG_EXCLUDE_EXTENSIONS})
            if(FILE_EXT STREQUAL "${EXCLUDED_EXT}")
                set(SKIP_FILE TRUE)
                break()
            endif()
        endforeach()
        if(SKIP_FILE)
            message(STATUS "Skipping resource: ${RESOURCE_FILE} (excluded extension)")
            continue()
        endif()

        # Create symbol name: res_ui_icons_play_png
        string(REPLACE "/" "_" SYMBOL_PATH "${RESOURCE_FILE}")
        string(REPLACE "-" "_" SYMBOL_PATH "${SYMBOL_PATH}")
        string(REPLACE " " "_" SYMBOL_PATH "${SYMBOL_PATH}")
        string(REPLACE "." "_" SYMBOL_NAME "${SYMBOL_PATH}")
        set(SYMBOL_NAME "res_${SYMBOL_NAME}")

        # Build directory nodes (if any)
        get_filename_component(FILE_DIR "${RESOURCE_FILE}" DIRECTORY)
        get_filename_component(FILE_NAME "${RESOURCE_FILE}" NAME)
        if(FILE_DIR STREQUAL ".")
            set(FILE_DIR "")
        endif()
        set(PARENT_INDEX ${ROOT_INDEX})
        if(NOT FILE_DIR STREQUAL "")
            string(REPLACE "/" ";" DIR_PARTS "${FILE_DIR}")
            set(ACCUM_PATH "")
            foreach(PART ${DIR_PARTS})
                if(ACCUM_PATH STREQUAL "")
                    set(ACCUM_PATH "${PART}")
                else()
                    set(ACCUM_PATH "${ACCUM_PATH}/${PART}")
                endif()
                _resource_get_or_add_dir("${ACCUM_PATH}" "${PART}" "${PARENT_INDEX}" DIR_INDEX)
                set(PARENT_INDEX ${DIR_INDEX})
            endforeach()
        endif()

        # Add the file node itself
        _resource_add_node("${RESOURCE_FILE}" "${FILE_NAME}" 0 "${SYMBOL_NAME}" "${SYMBOL_NAME}_len" "${PARENT_INDEX}" FILE_INDEX)

        # Output .c file
        set(OUTPUT_C "${CMAKE_CURRENT_BINARY_DIR}/${SYMBOL_NAME}.c")

        add_custom_command(
            OUTPUT "${OUTPUT_C}"
            COMMAND embedfile "${SYMBOL_NAME}" "${RESOURCE_DIR}/${RESOURCE_FILE}"
            DEPENDS "${RESOURCE_DIR}/${RESOURCE_FILE}"
            COMMENT "Embedding ${RESOURCE_FILE} as ${SYMBOL_NAME}"
            VERBATIM
        )

        # Append to generated files list
        list(APPEND GENERATED_C_FILES "${OUTPUT_C}")

        # Add to extern declarations
        string(APPEND RESOURCE_DECLS "extern const unsigned char ${SYMBOL_NAME}[];\n")
        string(APPEND RESOURCE_DECLS "extern const unsigned long long ${SYMBOL_NAME}_len;\n\n")
    endforeach()

    # Write the resources.c file containing extern declarations
    set(RESOURCES_C "${CMAKE_CURRENT_BINARY_DIR}/resources.c")
    set(RESOURCES_C_CONTENT "// Auto-generated resource declarations\n\n${RESOURCE_DECLS}")
    file(WRITE "${RESOURCES_C}" "${RESOURCES_C_CONTENT}")
    list(APPEND GENERATED_C_FILES "${RESOURCES_C}")

    # Return full list of .c files
    set(${OUT_VAR} "${GENERATED_C_FILES}" PARENT_SCOPE)

    # At the end of embed_resources function, after building RESOURCE_DECLS
    set(RESOURCES_C "${CMAKE_CURRENT_BINARY_DIR}/resources.c")
    set(RESOURCES_H "${CMAKE_CURRENT_BINARY_DIR}/resources.h")

    set(RESOURCES_H_CONTENT "// Auto-generated resource header\n\n")
    string(APPEND RESOURCES_H_CONTENT "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    string(APPEND RESOURCES_H_CONTENT "${RESOURCE_DECLS}")
    string(APPEND RESOURCES_H_CONTENT "#ifdef __cplusplus\n}\n#endif\n")
    file(WRITE "${RESOURCES_H}" "${RESOURCES_H_CONTENT}")

    list(APPEND GENERATED_C_FILES "${RESOURCES_C}")
    # Also expose RESOURCES_H so it can be included in target's include path
    set(${OUT_VAR} "${GENERATED_C_FILES}" PARENT_SCOPE)

    # Optional: export header path so user can include it
    set(RESOURCES_HEADER "${RESOURCES_H}" PARENT_SCOPE)

    # Build child/sibling links for tree nodes.
    set(NODE_FIRST_CHILD)
    set(NODE_NEXT_SIBLING)
    set(NODE_LAST_CHILD)
    math(EXPR LAST_INDEX "${NODE_COUNT} - 1")
    foreach(i RANGE 0 ${LAST_INDEX})
        list(APPEND NODE_FIRST_CHILD -1)
        list(APPEND NODE_NEXT_SIBLING -1)
        list(APPEND NODE_LAST_CHILD -1)
    endforeach()

    foreach(i RANGE 0 ${LAST_INDEX})
        list(GET NODE_PARENTS ${i} PARENT)
        if(NOT PARENT EQUAL -1)
            list(GET NODE_FIRST_CHILD ${PARENT} FC)
            if(FC EQUAL -1)
                _resource_list_set(NODE_FIRST_CHILD ${PARENT} ${i})
            else()
                list(GET NODE_LAST_CHILD ${PARENT} LC)
                _resource_list_set(NODE_NEXT_SIBLING ${LC} ${i})
            endif()
            _resource_list_set(NODE_LAST_CHILD ${PARENT} ${i})
        endif()
    endforeach()

    # Emit tree header and source
    set(RESOURCE_TREE_H "${CMAKE_CURRENT_BINARY_DIR}/resource_tree.h")
    set(RESOURCE_TREE_C "${CMAKE_CURRENT_BINARY_DIR}/resource_tree.c")

    set(RESOURCE_TREE_H_CONTENT "// Auto-generated resource tree\n\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "#ifndef RESOURCE_TREE_H\n#define RESOURCE_TREE_H\n\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "typedef struct ResourceNode {\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  const char* name;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  const char* path;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  const unsigned char* data;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  const unsigned long long* len;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  int parent;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  int first_child;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  int next_sibling;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "  int is_dir;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "} ResourceNode;\n\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "extern const ResourceNode g_resource_nodes[];\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "extern const unsigned int g_resource_nodes_count;\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "extern const int g_resource_root_index;\n\n")
    string(APPEND RESOURCE_TREE_H_CONTENT "#ifdef __cplusplus\n}\n#endif\n\n#endif\n")

    set(RESOURCE_TREE_C_CONTENT "// Auto-generated resource tree\n\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "#include <stddef.h>\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "#include \"resource_tree.h\"\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "#include \"resources.h\"\n\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "const ResourceNode g_resource_nodes[] = {\n")

    foreach(i RANGE 0 ${LAST_INDEX})
        list(GET NODE_NAMES ${i} NODE_NAME)
        list(GET NODE_PATHS ${i} NODE_PATH)
        list(GET NODE_ISDIRS ${i} NODE_ISDIR)
        list(GET NODE_DATASYMS ${i} NODE_DATASYM)
        list(GET NODE_LENS ${i} NODE_LEN)
        list(GET NODE_PARENTS ${i} NODE_PARENT)
        list(GET NODE_FIRST_CHILD ${i} NODE_FIRST)
        list(GET NODE_NEXT_SIBLING ${i} NODE_NEXT)

        string(REPLACE "\\" "\\\\" NODE_NAME_ESC "${NODE_NAME}")
        string(REPLACE "\"" "\\\"" NODE_NAME_ESC "${NODE_NAME_ESC}")
        string(REPLACE "\\" "\\\\" NODE_PATH_ESC "${NODE_PATH}")
        string(REPLACE "\"" "\\\"" NODE_PATH_ESC "${NODE_PATH_ESC}")

        if(NODE_ISDIR)
            set(DATA_PTR "NULL")
            set(LEN_EXPR "NULL")
        else()
            set(DATA_PTR "(const unsigned char*)${NODE_DATASYM}")
            set(LEN_EXPR "&${NODE_LEN}")
        endif()

        string(APPEND RESOURCE_TREE_C_CONTENT
            "  {\"${NODE_NAME_ESC}\", \"${NODE_PATH_ESC}\", ${DATA_PTR}, ${LEN_EXPR}, ${NODE_PARENT}, ${NODE_FIRST}, ${NODE_NEXT}, ${NODE_ISDIR}},\n")
    endforeach()

    string(APPEND RESOURCE_TREE_C_CONTENT "};\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "const unsigned int g_resource_nodes_count = ${NODE_COUNT};\n")
    string(APPEND RESOURCE_TREE_C_CONTENT "const int g_resource_root_index = ${ROOT_INDEX};\n")

    file(WRITE "${RESOURCE_TREE_H}" "${RESOURCE_TREE_H_CONTENT}")
    file(WRITE "${RESOURCE_TREE_C}" "${RESOURCE_TREE_C_CONTENT}")

    list(APPEND GENERATED_C_FILES "${RESOURCE_TREE_C}")

    set(GENERATE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generate_resources.cmake")
    file(WRITE "${GENERATE_SCRIPT}" "file(WRITE \"${RESOURCES_C}\" [=[\n${RESOURCES_C_CONTENT}\n]=])\n")
    file(APPEND "${GENERATE_SCRIPT}" "file(WRITE \"${RESOURCES_H}\" [=[\n${RESOURCES_H_CONTENT}\n]=])\n")
    file(APPEND "${GENERATE_SCRIPT}" "file(WRITE \"${RESOURCE_TREE_H}\" [=[\n${RESOURCE_TREE_H_CONTENT}\n]=])\n")
    file(APPEND "${GENERATE_SCRIPT}" "file(WRITE \"${RESOURCE_TREE_C}\" [=[\n${RESOURCE_TREE_C_CONTENT}\n]=])\n")

    set(GENERATE_DEPENDS)
    foreach(RESOURCE_FILE ${RESOURCE_FILES})
        list(APPEND GENERATE_DEPENDS "${RESOURCE_DIR}/${RESOURCE_FILE}")
    endforeach()

    add_custom_command(
        OUTPUT "${RESOURCES_C}" "${RESOURCES_H}" "${RESOURCE_TREE_C}" "${RESOURCE_TREE_H}"
        COMMAND ${CMAKE_COMMAND} -P "${GENERATE_SCRIPT}"
        DEPENDS ${GENERATE_DEPENDS} "${GENERATE_SCRIPT}"
        COMMENT "Generating embedded resource headers"
        VERBATIM
    )

    # Update caller-visible list after adding tree source.
    set(${OUT_VAR} "${GENERATED_C_FILES}" PARENT_SCOPE)

    set_source_files_properties(${GENERATED_C_FILES} PROPERTIES GENERATED TRUE)
    
endfunction()
