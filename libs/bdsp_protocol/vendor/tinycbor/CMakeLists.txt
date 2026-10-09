# Minimal caller-buffer runtime: excludes allocating string duplication,
# pretty/stdio/JSON tools and all upstream tests.
add_library(tinycbor_runtime STATIC
    src/cborencoder.c src/cborencoder_close_container_checked.c src/cborencoder_float.c
    src/cborparser.c src/cborparser_float.c src/cborerrorstrings.c src/cborvalidation.c)
set_target_properties(tinycbor_runtime PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(tinycbor_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/tinycbor>)
install(TARGETS tinycbor_runtime EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(FILES src/cbor.h include/tinycbor-export.h include/tinycbor-version.h
    DESTINATION include/tinycbor)
install(FILES LICENSE manifest.json DESTINATION share/PortableDSP/licenses/tinycbor)
