# Lossless document parsing is an outer adapter; it is not a DSP SDK dependency.
add_library(numeric_array_codec STATIC src/numeric_array_codec.cpp)
target_compile_features(numeric_array_codec PUBLIC cxx_std_20)
set_target_properties(numeric_array_codec PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(numeric_array_codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(numeric_array_codec PUBLIC domain_model)
install(TARGETS numeric_array_codec EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(FILES include/project_storage/numeric_array_codec.h DESTINATION include/project_storage)

add_library(project_storage STATIC src/project_source.cpp)
target_compile_features(project_storage PUBLIC cxx_std_20)
set_target_properties(project_storage PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 20)
target_include_directories(project_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(project_storage PRIVATE /GR- /EHs-c-)
else()
    target_compile_options(project_storage PRIVATE -fno-exceptions -fno-rtti)
endif()
