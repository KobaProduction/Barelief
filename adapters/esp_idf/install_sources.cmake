# Coordinator may include this from the SDK composition root. Source package
# keeps relative topology so the same component manifest remains usable.
get_filename_component(embedded_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(embedded_destination "share/PortableDSP/embedded")
install(DIRECTORY "${embedded_source_root}/adapters/esp_idf/"
    DESTINATION "${embedded_destination}/adapters/esp_idf")
install(DIRECTORY "${embedded_source_root}/examples/esp32_streamer/"
    DESTINATION "${embedded_destination}/examples/esp32_streamer")
foreach(part IN ITEMS libs/signal_contracts libs/bdsp_protocol domain application
    libs/dataflow_runtime libs/dsp_kernels libs/embedded_support)
    install(DIRECTORY "${embedded_source_root}/${part}/include/"
        DESTINATION "${embedded_destination}/${part}/include")
    install(DIRECTORY "${embedded_source_root}/${part}/src/"
        DESTINATION "${embedded_destination}/${part}/src")
endforeach()
# Original licensed ANSI dependency, used only by the ANSI source profile.
install(DIRECTORY "${embedded_source_root}/libs/dsp_kernels/vendor/esp_dsp/"
    DESTINATION "${embedded_destination}/libs/dsp_kernels/vendor/esp_dsp")
install(FILES "${embedded_source_root}/docs/embedded-sdk.md" DESTINATION "${embedded_destination}/docs")
