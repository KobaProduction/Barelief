include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
function(configure_portable_targets)
    foreach(portable_target IN ITEMS signal_contracts bdsp_protocol wled_protocol domain_model domain_numeric domain_signal domain_music graph_domain graph_application numeric_array_codec portable_control_mapper portable_numeric_control_session dataflow_runtime embedded_support dsp_kernels dsp BDSP)
        if(TARGET ${portable_target})
            set_target_properties(${portable_target} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
            set_target_properties(${portable_target} PROPERTIES CXX_STANDARD 20)
            if(PORTABLE_DISABLE_EXCEPTIONS_RTTI)
                if(MSVC)
                    target_compile_options(${portable_target} PRIVATE /GR- /EHs-c-)
                else()
                    target_compile_options(${portable_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
                endif()
            endif()
        endif()
    endforeach()
endfunction()
configure_portable_targets()
include("${CMAKE_SOURCE_DIR}/adapters/esp_idf/install_sources.cmake")
foreach(portable_target IN ITEMS domain_model domain_numeric domain_signal domain_music graph_domain graph_application)
    install(TARGETS ${portable_target} EXPORT portable_sdk_targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/domain/include/" "${CMAKE_SOURCE_DIR}/application/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# signal_contracts and protocol install their own headers.
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/PortableDSPConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/PortableDSPConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PortableDSP")
write_basic_package_version_file("${CMAKE_BINARY_DIR}/PortableDSPConfigVersion.cmake"
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(EXPORT portable_sdk_targets NAMESPACE PortableDSP::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PortableDSP")
install(FILES "${CMAKE_BINARY_DIR}/PortableDSPConfig.cmake" "${CMAKE_BINARY_DIR}/PortableDSPConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PortableDSP")
