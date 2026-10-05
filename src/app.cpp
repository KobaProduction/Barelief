#include "app.h"
#include <cstring>
#include "core/latest_delivery.h"

#include "visualization/charts/line.h"
#include "visualization/charts/bar.h"
#include "visualization/charts/freq.h"

Application::Application(int &argc, char **argv, const int flags) : QApplication(argc, argv, flags) {
    _pipeline = std::make_unique<pipeline::AudioPipeline>();
    if (_cfg.run_analyser) {
        _run_analyzer();
    }

    if (_cfg.run_loopback) {
        _run_loopback();
    }

    if (_cfg.run_serial) {
        _run_serial();
    }

    splitter = new QSplitter;
    splitter->setOrientation(Qt::Vertical);


    if (_cfg.show_serial_samples) {
        serialSamplesView = new LineChartView();
        serialSamplesView->set_range(-100, 100);
        serialSamplesView->chart()->setTitle("Serial Samples");
        serialSamplesView->set_auto_resizing(true);
        serialSamplesView->set_auto_gain(true, 100, -100);
        splitter->addWidget(serialSamplesView);
    }

    if (_cfg.show_serial_audio_spectre) {
        serialSpectreView = new BarChartView();
        //        serialSpectreView->axisX.setTickCount(31);
        //        serialSpectreView->axisX.setLabelFormat("%.0f");
        //        serialSpectreView->axisX.setMinorTickCount(9);
        serialSpectreView->chart()->setTitle("Serial Spectre");
        serialSpectreView->set_auto_resizing(true);
        serialSpectreView->set_auto_gain(true, 255, 0, 2);
        serialSpectreView->set_range(0, 255);

        splitter->addWidget(serialSpectreView);
    }


    if (_cfg.show_serial_fast_amplitudes) {
        serialFastAmplitudesView = new BarChartView();
        serialFastAmplitudesView->chart()->setTitle("Serial Fast Amplitudes");
        serialFastAmplitudesView->set_auto_resizing(false);
        serialFastAmplitudesView->set_range(0, 255);
        splitter->addWidget(serialFastAmplitudesView);
    }


    if (receiver and (_cfg.show_serial_samples or _cfg.show_serial_audio_spectre or _cfg.show_serial_fast_amplitudes)) {
        receiver->set_packet_handler([](BDSP::packet_context_t &packet_context, void *packet_handler_context) {
            const auto &app = *reinterpret_cast<Application *>(packet_handler_context);

            if (packet_context.packet_id == 1 and app.serialSamplesView) {
                size_t samples_size = packet_context.size;
                samples_size /= 2;
                std::vector<float> samples;
                for (int i = 0; i < samples_size; ++i) {
                    int16_t value;
                    std::memcpy(&value, packet_context.data_ptr + i * sizeof(value), sizeof(value));
                    samples.push_back(float(value));
                }
                app.serialSamplesView->update(samples);
                return;
            } else if (packet_context.packet_id == 2 and app.serialSpectreView) {
                size_t spectre_size = packet_context.size;
                spectre_size = std::min<size_t>(spectre_size, 300);
                //                spectre_size /= 2;
                std::vector<float> spectre;
                const auto *packet_samples = reinterpret_cast<uint8_t *>(packet_context.data_ptr);
                for (int i = 0; i < spectre_size; ++i) {
                    spectre.push_back(float(packet_samples[i]));
                }
                app.serialSpectreView->update(spectre);
                return;
            } else if (packet_context.packet_id == 3 and app.serialFastAmplitudesView) {
                std::vector<float> amplitudes;
                amplitudes.resize(packet_context.size);
                const auto *data = reinterpret_cast<uint8_t *>(packet_context.data_ptr);
                for (int i = 0; i < amplitudes.size(); ++i) {
                    amplitudes[i] = static_cast<float>(data[i]);
                }
                app.serialFastAmplitudesView->update(amplitudes);
                return;
            } else if (packet_context.packet_id == 4 and app.serialFastAmplitudesView) {
                std::vector<float> amplitudes;
                amplitudes.resize(packet_context.size / 4);
                for (int i = 0; i < amplitudes.size(); ++i) {
                    std::memcpy(&amplitudes[i], packet_context.data_ptr + i * sizeof(float), sizeof(float));
                }
                app.serialFastAmplitudesView->update(amplitudes);
                return;
            }
            //            static uint32_t j = 0;
            //        qInfo("[%d] Got packet. Packet ID: %d, size: %d", j++, packet_context.packet_id, packet_context.size);
            //        return;
            qWarning("Incorrect serial packet processing");
        }, this);
    }


    if (_cfg.show_raw_samples and _cfg.run_loopback) {
        rawSamplesView = new LineChartView();
        rawSamplesView->set_range(-1, 1);
        rawSamplesView->chart()->setTitle("Raw samples");
        splitter->addWidget(rawSamplesView);
    } else if (_cfg.show_raw_samples) {
        qInfo("Raw samples only can show when loopback running");
    }

    if (_cfg.show_window_signal and _cfg.run_analyser) {
        windowSignalView = new LineChartView();
        windowSignalView->set_range(-1, 1);
        windowSignalView->chart()->setTitle("Window signal");
        splitter->addWidget(windowSignalView);
    } else if (_cfg.show_window_signal) {
        qInfo("Window signal requires the analyzer");
    }

    if (_cfg.show_amplitudes and _cfg.run_analyser) {
        amplitudesView = new FreqChartView();

        amplitudesView->set_range(0, 1);
        amplitudesView->chart()->setTitle("Amplitudes FFT 4-radix");
        amplitudesView->set_auto_resizing(true);
        amplitudesView->set_auto_gain(true, 1, 0);

        splitter->addWidget(amplitudesView);
    } else if (_cfg.show_amplitudes) {
        qInfo("Analyzer amplitudes only can show when analyzer running");
    }

    if (_cfg.show_test_amplitudes and _cfg.run_analyser) {
        testAmplitudesView = new LineChartView();

        testAmplitudesView->set_range(0, 1);
        testAmplitudesView->chart()->setTitle("test amplitudes");
        testAmplitudesView->set_auto_resizing(true);
        testAmplitudesView->set_auto_gain(true, 1, 0);

        splitter->addWidget(testAmplitudesView);
    } else if (_cfg.show_test_amplitudes) {
        qInfo("test Analyzer amplitudes only can show when analyzer running");
    }


    tabWidget.setWindowTitle("qt-visualization");

    const QSize screenSize = tabWidget.screen()->size();
    const QSize minimumGraphSize{screenSize.width() / 2, qRound(screenSize.height() / 1.75)};
    //
    if (_cfg.show_spectrogram) {
        spectrogramView = new SpectrogramGraph(&tabWidget);
        if (not spectrogramView->initialize(minimumGraphSize, screenSize)) {
            qWarning("Couldn't initialize the OpenGL context.");
        }
        tabWidget.addTab(spectrogramView, "spectrogram tab");
    }

    tabWidget.addTab(splitter, "main");
    tabWidget.resize(1200, 800);
    // tabWidget.show();

    _window.setCentralWidget(tabWidget.window());
    //    _window.setCentralWidget(tabWidget);
    // _window.grabGesture(Qt::PanGesture);
    // _window.grabGesture(Qt::PinchGesture);
    _window.show();
}

Application::~Application() {
    // Runtime stops capture, DSP and outputs before any view is destroyed.
    _pipeline.reset();
    delete receiver;
    if (serial) serial->close();
    _window.takeCentralWidget();
}

void Application::_run_analyzer() {
    auto delivery = std::make_shared<LatestDelivery<pipeline::ViewFrame>>(this, [this](pipeline::ViewFrame frame) {
        if (windowSignalView) windowSignalView->update(frame.window_signal);
        if (amplitudesView) {
            static_cast<FreqChartView *>(amplitudesView)->update_freq_step(frame.frequency_step);
            amplitudesView->update(frame.fft);
        }
        if (testAmplitudesView) testAmplitudesView->update(frame.mel);
        if (spectrogramView && frame.history) spectrogramView->update(frame.history->values);
    });
    delivery->start_polling();
    _pipeline->set_spectrum_handler([delivery](pipeline::ViewFrame frame) { delivery->publish(std::move(frame)); });
    _pipeline->start_analyzer();
}

void Application::_run_loopback() {
    auto delivery = std::make_shared<LatestDelivery<pipeline::AudioFrame>>(this, [this](pipeline::AudioFrame frame) {
        if (rawSamplesView) rawSamplesView->update(frame.left);
    });
    delivery->start_polling();
    _pipeline->set_audio_handler([delivery](pipeline::AudioFrame frame) { delivery->publish(std::move(frame)); });
    _pipeline->start_capture();
}

void Application::_run_serial() {
    receiver = new std::remove_pointer<decltype(receiver)>::type();
    auto cfg = receiver->get_read_stream().get_strategy().get_config();
    cfg.delimiter_byte = '\n';
    cfg.size_of_the_sequence_to_be_replaced = 4;
    receiver->get_read_stream().get_strategy().set_config(cfg);

    receiver->set_error_handler([](BDSP::parse_packet_status_t error, void *ctx) {
        qInfo("receiver error: %d", error);
    }, nullptr);

    if (serial) {
        delete serial;
    }
    serial = new QSerialPort(this);

    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setDataBits(QSerialPort::Data8);
    serial->setFlowControl(QSerialPort::NoFlowControl);
    serial->setBaudRate(1000000);
    serial->setReadBufferSize(64);
    serial->setPortName("COM8");


    connect(serial, &QSerialPort::readyRead, this, [this]() {
        if (this->serial) {
            auto data = this->serial->readAll();
            receiver->get_read_stream().read(reinterpret_cast<uint8_t *>(data.data()), data.size());
        };
    });

    receiver->set_packet_handler([](BDSP::packet_context_t &packet, void *ctx) {
        qWarning("Got packet, ID: %d. But packet handler not set...", packet.packet_id);
    }, nullptr);

    if (serial->open(QSerialPort::ReadOnly)) {
        qInfo("port opened");
    } else {
        qWarning("Failed opening port");
    }
}
