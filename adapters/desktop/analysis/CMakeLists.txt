add_library(analyzer STATIC)

include(${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/sources.cmake)

target_sources(analyzer PRIVATE
        ${DESKTOP_ANALYSIS_HEADERS} ${DESKTOP_ANALYSIS_SOURCES}
)

target_include_directories(analyzer PUBLIC ${DESKTOP_ANALYSIS_INCLUDE_DIR})

target_link_libraries(analyzer PUBLIC
        qt_engine_ports
        Qt::Core
        dsp_kernels
        portable_dsp
        core
)
