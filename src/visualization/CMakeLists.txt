add_library(visualization STATIC)

set(CMAKE_AUTOMOC ON)
#set(CMAKE_AUTORCC ON)
#set(CMAKE_AUTOUIC ON)

set(HEADERS
        include/visualization/abstract/abs.h
        include/visualization/abstract/fps.h
        include/visualization/abstract/mixins.h
        include/visualization/charts/bar.h
        include/visualization/charts/line.h
        include/visualization/charts/freq.h

        include/visualization/surfaces/surfacegraph.h
        include/visualization/surfaces/surfacegraphmodifier.h
)

set(SOURCES
        ${public_headers}
        src/abstract/abs.cpp
        src/abstract/fps.cpp
        src/abstract/mixins.cpp
        src/charts/bar.cpp
        src/charts/line.cpp
        src/charts/freq.cpp

        src/surfaces/surfacegraph.cpp
        src/surfaces/surfacegraphmodifier.cpp
)


target_sources(visualization PRIVATE ${HEADERS} ${SOURCES})

target_include_directories(visualization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Qt6 REQUIRED COMPONENTS Charts DataVisualization)

target_link_libraries(visualization Qt::Charts Qt::DataVisualization)
target_link_libraries(visualization runtime_performance)
