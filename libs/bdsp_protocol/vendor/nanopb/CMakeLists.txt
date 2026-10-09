# Runtime only; no generator or malloc opt-in in an ordinary build.
add_library(nanopb_runtime STATIC pb_common.c pb_encode.c pb_decode.c)
set_target_properties(nanopb_runtime PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(nanopb_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/nanopb>)
install(TARGETS nanopb_runtime EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(FILES pb.h pb_common.h pb_encode.h pb_decode.h DESTINATION include/nanopb)
install(FILES LICENSE.txt manifest.json DESTINATION share/PortableDSP/licenses/nanopb)
