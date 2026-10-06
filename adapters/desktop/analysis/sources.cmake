# Canonical desktop analysis adapter. Target/dependency wiring belongs to the composition root.
set(DESKTOP_ANALYSIS_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")
set(DESKTOP_ANALYSIS_HEADERS
    "${DESKTOP_ANALYSIS_INCLUDE_DIR}/analyzer/analyze/analyzer.h"
    "${DESKTOP_ANALYSIS_INCLUDE_DIR}/analyzer/analyze/audio_window.h"
    "${DESKTOP_ANALYSIS_INCLUDE_DIR}/analyzer/analyze/fft.h"
    "${DESKTOP_ANALYSIS_INCLUDE_DIR}/analyzer/analyze/spectrogram.h"
)
set(DESKTOP_ANALYSIS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/analyze/analyzer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/analyze/audio_window.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/analyze/fft.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/analyze/spectrogram.cpp"
)
