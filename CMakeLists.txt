cmake_minimum_required(VERSION 3.30)
set(PROJECT_NAME "qt-visualization")
project(${PROJECT_NAME} LANGUAGES C CXX)
option(BUILD_DSP_ONLY "Build the portable computation library without Qt" OFF)
option(BUILD_PORTABLE_SDK "Build portable contracts, DSP, runtime and protocol without desktop dependencies" OFF)
option(PORTABLE_DISABLE_EXCEPTIONS_RTTI "Compile portable targets without exceptions and RTTI" OFF)
option(BUILD_GUI "Build desktop visualization applications" ON)
option(BUILD_LEGACY_APP "Build the compatibility visualization application" ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
set(BDSP_SHARED_LIBS OFF)
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
include(CTest)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(FetchContent)
set(BDSP_INSTALL ON CACHE BOOL "Install transport-neutral BDSP dependency" FORCE)
FetchContent_Declare(BDSP
    GIT_REPOSITORY https://github.com/KobaProduction/BDSP.git
    GIT_TAG 2c68056dfaeffba2f34fc64eee9bc9e331e83c7c)
FetchContent_MakeAvailable(BDSP)
add_subdirectory(libs/signal_contracts)
add_subdirectory(libs/bdsp_protocol)
add_subdirectory(domain)
add_subdirectory(application)
add_subdirectory(libs/dsp_kernels)
add_subdirectory(libs/dataflow_runtime)
add_subdirectory(libs/embedded_support)
if(BUILD_DSP_ONLY OR BUILD_PORTABLE_SDK)
    set(CMAKE_CXX_STANDARD 20)
endif()
include(cmake/portable_sdk.cmake)
if(BUILD_DSP_ONLY OR BUILD_PORTABLE_SDK)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
    return()
endif()
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network)
add_subdirectory(adapters/desktop)
if(BUILD_GUI)
    find_package(Qt6 6.7 REQUIRED COMPONENTS Gui Widgets Charts DataVisualization)
    if(BUILD_LEGACY_APP)
        find_package(Qt6 6.7 REQUIRED COMPONENTS SerialPort)
    endif()
endif()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
include(FetchContent)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR capture-only)
FetchContent_MakeAvailable(miniaudio)


FetchContent_Declare(
        QtNodes
        EXCLUDE_FROM_ALL
        GIT_REPOSITORY https://github.com/paceholder/nodeeditor.git
        GIT_TAG bcb0b35f21a0cbec95dd1e5f7dc6a9a7a7530252
)

option(DEPLOY_BUILD_RUNTIME "Deploy Windows Qt runtime beside built applications" ON)
include(cmake/deploy_desktop_runtime.cmake)
set(CMAKE_AUTOMOC ON)
add_subdirectory(adapters/desktop/platform)
if(BUILD_GUI)
    add_subdirectory(presentation/qt)
    if(BUILD_LEGACY_APP)
        add_subdirectory(apps/compatibility_gui)
    endif()
endif()
configure_portable_targets()
if(NOT PORTABLE_DSP_MAX_FFT_SIZE STREQUAL "4096")
    message(FATAL_ERROR "Desktop Analyzer requires FFT size 4096; use BUILD_DSP_ONLY=ON for a smaller kernel profile")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
include(GNUInstallDirs)
install(CODE [[get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)]])
foreach(app IN ITEMS core_app qt-application pipeline_cli aggregator_server_app aggregator_client_app)
    if(NOT TARGET ${app})
        continue()
    endif()
    install(TARGETS ${app} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    qt_generate_deploy_app_script(TARGET ${app} OUTPUT_SCRIPT deploy_script)
    install(SCRIPT ${deploy_script})
endforeach()
if(MINGW)
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(FILES
        "${compiler_bin}/libgcc_s_seh-1.dll"
        "${compiler_bin}/libstdc++-6.dll"
        "${compiler_bin}/libwinpthread-1.dll"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME qt-visualization)
set(CPACK_PACKAGE_VERSION 1.0.0)
include(CPack)
