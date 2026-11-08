foreach(package IN LISTS CPACK_PACKAGE_FILES)
    get_filename_component(package_name "${package}" NAME)
    file(COPY_FILE "${CPACK_BARELIEF_IDENTITY_FILE}" "${CPACK_PACKAGE_DIRECTORY}/${package_name}.identity.json")
endforeach()
