#ifndef QT_VISUALIZATION_APP_H
#define QT_VISUALIZATION_APP_H

#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QSerialPort>

#include <BDSP/receivers.h>

#include "pipeline/audio_pipeline.h"
#include "analyzer/analyze/spectrogram.h"
#include "analyzer/loopback/windows_loopback.h"
#include "visualization/abstract/abs.h"
#include "visualization/surfaces/surfacegraph.h"

class Application : public QApplication {
public:
    struct Config {
        bool run_loopback = true;
        bool run_serial = false;
        bool run_analyser = true;
        bool show_serial_samples = false;
        bool show_raw_samples = false;
        bool show_samples = false;
        bool show_amplitudes = true;
        bool show_test_amplitudes = true;
        bool show_serial_fast_amplitudes = false;
        bool show_serial_audio_spectre = false;
        bool show_surface = true;
    };

    Application(int &argc, char **argv, int = ApplicationFlags);
    ~Application() override;
private:
    Config _cfg;

    AbstractChartView *rawSamplesView = nullptr;
    AbstractChartView *serialSamplesView = nullptr;
    AbstractChartView *serialFastAmplitudesView = nullptr;
    AbstractChartView *serialSpectreView = nullptr;
    AbstractChartView *generatorSamplesView = nullptr;
    AbstractChartView *samplesView = nullptr;
    AbstractChartView *amplitudesView = nullptr;
    AbstractChartView *testAmplitudesView = nullptr;
    SurfaceGraph *surfaceView = nullptr;
    std::unique_ptr<pipeline::AudioPipeline> _pipeline;
    QSerialPort *serial{};
    BDSP::COBSZPEReceiver *receiver = nullptr;
    QSplitter *splitter;
    QTabWidget tabWidget;
    QMainWindow _window;

    void _run_analyzer();
    void _run_loopback();
    void _run_serial();
};


#endif //QT_VISUALIZATION_APP_H
