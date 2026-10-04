add_library(analyzer STATIC)

set(ANALYZER_HEADERS
        include/analyzer/analyze/analyzer.h
        include/analyzer/analyze/audio_window.h
        include/analyzer/analyze/spectrogram.h
        include/analyzer/loopback/windows_loopback.h
)

set(ANALYZER_SOURCES
        src/analyze/analyzer.cpp
        src/analyze/audio_window.cpp
        src/analyze/fft.cpp
        src/analyze/spectrogram.cpp
        src/loopback/windows_loopback.cpp
)

target_sources(analyzer PRIVATE
        ${ANALYZER_HEADERS} ${ANALYZER_SOURCES}
)

target_include_directories(analyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(analyzer PUBLIC
        Qt::Core
        dsp
        portable_dsp
        core
)
