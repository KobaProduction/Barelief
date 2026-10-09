get_filename_component(control_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(portable_control_mapper STATIC src/numeric_processing_message_mapper.cpp)
target_include_directories(portable_control_mapper PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(portable_control_mapper PUBLIC cxx_std_20)
target_link_libraries(portable_control_mapper PUBLIC graph_domain bdsp_protocol)
install(TARGETS portable_control_mapper EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(FILES include/control/numeric_processing_message_mapper.h
    include/control/control_session_types.h DESTINATION include/control)

add_library(portable_numeric_control_session STATIC
    "${control_sdk_root}/adapters/desktop/runtime/src/numeric_control_session.cpp")
target_include_directories(portable_numeric_control_session PUBLIC
    $<BUILD_INTERFACE:${control_sdk_root}/adapters/desktop/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(portable_numeric_control_session PUBLIC cxx_std_20)
target_link_libraries(portable_numeric_control_session PUBLIC
    portable_control_mapper graph_application bdsp_protocol)
install(TARGETS portable_numeric_control_session EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(FILES
    "${control_sdk_root}/adapters/desktop/include/control/numeric_control_session.h"
    DESTINATION include/control)
