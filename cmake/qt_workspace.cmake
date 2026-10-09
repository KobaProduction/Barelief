# A shared LGPL library keeps the docking implementation independently replaceable.
function(load_workspace_dependency)
    set(BUILD_STATIC OFF)
    set(BUILD_EXAMPLES OFF)
    FetchContent_Declare(QtADS
        GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
        GIT_TAG 023ce95934fecfd4cf5c672c9c404fabe0f54923)
    FetchContent_MakeAvailable(QtADS)
    set(ADS_SOURCE "${qtads_SOURCE_DIR}")
    include("${CMAKE_SOURCE_DIR}/cmake/patch_ads_drop_zones.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/patch_ads_drag_positions.cmake")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses")
    configure_file("${qtads_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/QtADS-LICENSE.txt" COPYONLY)
    install(FILES "${qtads_SOURCE_DIR}/LICENSE" DESTINATION share/licenses/QtADS)
endfunction()
load_workspace_dependency()
set_target_properties(qtadvanceddocking-qt6 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_library(qt_workspace STATIC
    ${CMAKE_SOURCE_DIR}/presentation/qt/include/core_app/workspace/workspace_window.h
    ${CMAKE_SOURCE_DIR}/presentation/qt/src/workspace/workspace_window.cpp)
target_include_directories(qt_workspace PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)
target_link_libraries(qt_workspace PUBLIC ads::qtadvanceddocking Qt6::Widgets)
if(WIN32)
    target_link_libraries(qt_workspace PRIVATE dwmapi)
endif()
install(TARGETS qtadvanceddocking-qt6 RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
