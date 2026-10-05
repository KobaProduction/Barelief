add_library(visualization STATIC)

set(CMAKE_AUTOMOC ON)
#set(CMAKE_AUTORCC ON)
#set(CMAKE_AUTOUIC ON)

set(HEADERS
        include/visualization/ui_text.h
        include/visualization/abstract/abs.h
        include/visualization/abstract/fps.h
        include/visualization/abstract/mixins.h
        include/visualization/charts/bar.h
        include/visualization/charts/line.h
        include/visualization/charts/freq.h

        include/visualization/spectrograms/spectrogram_graph.h
        include/visualization/spectrograms/spectrogram_graph_modifier.h
)

set(SOURCES
        src/ui_text.cpp
        ${public_headers}
        src/abstract/abs.cpp
        src/abstract/fps.cpp
        src/abstract/mixins.cpp
        src/charts/bar.cpp
        src/charts/line.cpp
        src/charts/freq.cpp

        src/spectrograms/spectrogram_graph.cpp
        src/spectrograms/spectrogram_graph_modifier.cpp
)


target_sources(visualization PRIVATE ${HEADERS} ${SOURCES})

target_include_directories(visualization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Qt6 REQUIRED COMPONENTS Charts DataVisualization)
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
qt_add_lrelease(visualization TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/i18n/ui_ru.ts
    QM_FILES_OUTPUT_VARIABLE ui_qm_files)
qt_add_resources(visualization ui_translations PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${ui_qm_files})

target_link_libraries(visualization PUBLIC Qt::Charts Qt::DataVisualization runtime_performance)
