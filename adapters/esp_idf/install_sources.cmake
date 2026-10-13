# Coordinator may include this from the SDK composition root. Source package
# keeps relative topology so the same component manifest remains usable.
get_filename_component(embedded_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(embedded_destination "share/PortableDSP/embedded")
install(DIRECTORY "${embedded_source_root}/adapters/esp_idf/"
    DESTINATION "${embedded_destination}/adapters/esp_idf")
install(DIRECTORY "${embedded_source_root}/examples/esp32_streamer/"
    DESTINATION "${embedded_destination}/examples/esp32_streamer")
foreach(part IN ITEMS libs/signal_contracts libs/bdsp_protocol libs/wled_protocol domain application
    libs/dataflow_runtime libs/dsp_kernels libs/embedded_support)
    install(DIRECTORY "${embedded_source_root}/${part}/include/"
        DESTINATION "${embedded_destination}/${part}/include")
    install(DIRECTORY "${embedded_source_root}/${part}/src/"
        DESTINATION "${embedded_destination}/${part}/src")
endforeach()
install(FILES "${embedded_source_root}/adapters/control/include/control/numeric_processing_message_mapper.h"
    "${embedded_source_root}/adapters/control/include/control/control_session_types.h"
    DESTINATION "${embedded_destination}/adapters/control/include/control")
install(FILES "${embedded_source_root}/adapters/control/src/numeric_processing_message_mapper.cpp"
    DESTINATION "${embedded_destination}/adapters/control/src")
install(FILES "${embedded_source_root}/adapters/desktop/include/control/numeric_control_session.h"
    DESTINATION "${embedded_destination}/adapters/desktop/include/control")
install(FILES "${embedded_source_root}/adapters/desktop/runtime/src/numeric_control_session.cpp"
    DESTINATION "${embedded_destination}/adapters/desktop/runtime/src")
# Original licensed ANSI dependency, used only by the ANSI source profile.
install(DIRECTORY "${embedded_source_root}/libs/bdsp_protocol/generated/"
    DESTINATION "${embedded_destination}/libs/bdsp_protocol/generated")
install(DIRECTORY "${embedded_source_root}/libs/bdsp_protocol/vendor/tinycbor/"
    DESTINATION "${embedded_destination}/libs/bdsp_protocol/vendor/tinycbor")
install(DIRECTORY "${embedded_source_root}/libs/bdsp_protocol/vendor/nanopb/"
    DESTINATION "${embedded_destination}/libs/bdsp_protocol/vendor/nanopb"
    PATTERN "generator" EXCLUDE)
install(DIRECTORY "${embedded_source_root}/libs/dsp_kernels/vendor/esp_dsp/"
    DESTINATION "${embedded_destination}/libs/dsp_kernels/vendor/esp_dsp")
# Keep the documentation topology: the SDK guide links to sibling reference
# and architecture pages, and to the installed examples two levels above it.
install(DIRECTORY "${embedded_source_root}/docs/"
    DESTINATION "${embedded_destination}/docs"
    COMPONENT Documentation
    FILES_MATCHING PATTERN "*.md")
