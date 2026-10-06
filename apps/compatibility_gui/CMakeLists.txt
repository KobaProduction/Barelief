add_executable(qt-application main.cpp app.h app.cpp)
target_link_libraries(qt-application PRIVATE audio_pipeline visualization Qt6::Widgets Qt6::SerialPort BDSP)
deploy_desktop_runtime(qt-application)
