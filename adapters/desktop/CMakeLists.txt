add_library(graph_document_adapter STATIC src/project_display_settings.cpp
    src/numeric_project_document.cpp)
target_include_directories(graph_document_adapter PUBLIC include)
target_link_libraries(graph_document_adapter PUBLIC graph_application project_storage Qt6::Core)
