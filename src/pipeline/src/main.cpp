#include "pipeline/audio_pipeline.h"
#include "pipeline/engine_client.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QElapsedTimer>
#include <algorithm>
#include <chrono>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include "core/utils.h"
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("pipeline_cli");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"duration", "Stop after milliseconds (0: continuous)", "ms", "0"});
    parser.addOption({"config", "Settings file or standalone audio graph", "file"});
    parser.addOption({"synthetic", "Use a synthetic stereo 750 Hz signal instead of WASAPI"});
    parser.addOption({"microphone", "Capture the default system microphone"});
    parser.addOption({"listen", "Serve BDSP clients at explicit address (default localhost)", "address", "127.0.0.1"});
    parser.addOption({"port", "Engine BDSP TCP port", "port", "8213"});
    parser.addOption({"server", "Enable engine BDSP TCP server"});
    parser.addOption({"allow-mutations", "Allow remote engine commands"});
    parser.addOption({"remote", "Connect to an existing engine instead of capturing locally", "host"});
    parser.process(app);
    if (parser.isSet("synthetic") && parser.isSet("microphone")) {
        qCritical("Choose either synthetic or microphone source");
        return 2;
    }
    bool ok;
    int duration = parser.value("duration").toInt(&ok);
    if (!ok || duration < 0) {
        qCritical("Invalid duration");
        return 2;
    }
    bool port_ok = false;
    const int port = parser.value("port").toInt(&port_ok);
    if (!port_ok || port < 1 || port > 65535) {
        qCritical("Invalid engine TCP port");
        return 2;
    }
    if (parser.isSet("remote")) {
        pipeline::EngineClient client(parser.value("remote"), static_cast<quint16>(port));
        client.set_snapshot_handler([](pipeline::EngineSnapshot s) {
            qInfo() << "Engine snapshot" << s.epoch << s.revision << s.graph_revision;
        });
        client.set_reply_handler([](pipeline::EngineReply r) {
            if (!r.accepted)
                qWarning().noquote() << r.error;
        });
        if (duration)
            QTimer::singleShot(duration, &app, &QCoreApplication::quit);
        return app.exec();
    }
    pipeline::AudioPipeline engine;
    std::unique_ptr<pipeline::EngineServer> server;
    QJsonObject settings;
    if (parser.isSet("config")) {
        QFile file(parser.value("config"));
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical("Cannot open settings");
            return 2;
        }
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
        if (!document.isObject() || parse_error.error != QJsonParseError::NoError) {
            qCritical("Invalid settings document");
            return 2;
        }
        settings = document.object();
        auto graph = settings.contains("nodes") ? settings : settings.value("pipeline_graph").toObject();
        if (graph.isEmpty())
            graph = settings.value("scene_graph").toObject();
        if (graph.isEmpty()) {
            const auto core = settings.value("core").toObject();
            const auto merge = core.value("spectrum_merge").toString("energy");
            graph = pipeline::GraphProgram::defaults(core.value("spectrum_channel").toInt(0),
                                                     merge == "mean"      ? SpectrumMerge::Mean
                                                     : merge == "maximum" ? SpectrumMerge::Maximum
                                                                          : SpectrumMerge::Energy,
                                                     core.value("capture_source").toInt(0));
        }
        QString error;
        if (!engine.apply_graph(graph, error)) {
            qCritical().noquote() << error;
            return 2;
        }
    }
    if (parser.isSet("microphone")) {
        auto graph = engine.graph();
        auto nodes = graph.value("nodes").toArray();
        for (int i = 0; i < nodes.size(); ++i) {
            auto node = nodes[i].toObject();
            auto data = node["internal-data"].toObject();
            if (data["model-name"] == "AudioSource")
                data["source"] = 1;
            node["internal-data"] = data;
            nodes[i] = node;
        }
        graph["nodes"] = nodes;
        QString error;
        if (!engine.apply_graph(graph, error))
            return 2;
    }
    engine.set_status_handler([](QString component, pipeline::ComponentStatus status) {
        qInfo().noquote() << component << static_cast<int>(status.state) << status.error;
    });
    if (parser.isSet("server") || parser.isSet("listen")) {
        const QHostAddress address(parser.value("listen"));
        if (address.isNull()) {
            qCritical("Invalid listen address");
            return 2;
        }
        server = std::make_unique<pipeline::EngineServer>(engine);
        if (!server->listen(address, static_cast<quint16>(port),
                            (address.isLoopback() || parser.isSet("allow-mutations")))) {
            qCritical().noquote() << server->error();
            return 2;
        }
        qInfo() << "Engine listening" << address.toString() << server->port();
    }
    const auto core = settings.value("core").toObject();
    if (core.value("run_analyser").toBool(true))
        engine.start_analyzer();
    QTimer source;
    uint64_t offset = 0, sequence = 0;
    QElapsedTimer source_clock;
    int64_t stream_start_ns = 0;
    if (parser.isSet("synthetic")) {
        source.setTimerType(Qt::PreciseTimer);
        source.setInterval(5);
        QObject::connect(&source, &QTimer::timeout, &app, [&] {
            constexpr uint64_t rate = 48000, chunk_limit = 4096, catch_up_limit = 8192;
            const auto elapsed = static_cast<uint64_t>(source_clock.nsecsElapsed());
            // Count PCM from elapsed audio time, including a fractional sample
            // remainder. Timer precision does not determine the audio rate.
            const auto due = (elapsed / 1000000000) * rate + (elapsed % 1000000000) * rate / 1000000000;
            bool gap = due > offset && due - offset > catch_up_limit;
            if (gap) offset = due - catch_up_limit;
            while (offset < due) {
                const auto count = static_cast<size_t>(std::min(due - offset, chunk_limit));
                pipeline::AudioFrame frame;
                frame.sample_rate = rate;
                frame.sequence = ++sequence;
                frame.discontinuity = gap;
                gap = false;
                frame.left.resize(count);
                frame.right.resize(count);
                for (size_t i = 0; i < count; ++i)
                    frame.left[i] = frame.right[i] = std::sin(2 * 3.141592653589793 * 750 * ((offset + i) % rate) / rate);
                offset += count;
                frame.timestamp_ns = stream_start_ns + static_cast<int64_t>((offset / rate) * 1000000000 +
                                                                          (offset % rate) * 1000000000 / rate);
                engine.feed(frame);
            }
        });
        source_clock.start();
        stream_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        source.start();
    } else if (core.value("run_loopback").toBool(true))
        engine.start_capture();
    const auto wled = settings.value("wled_ar_server").toObject();
    if (wled.value("enabled").toBool(false)) {
        const auto port = wled.value("port").toInt(11988);
        if (port < 1 || port > 65535) {
            qCritical("Invalid WLED port");
            return 2;
        }
        engine.start_wled(get_network_interface_address(wled.value("network_interface_name").toString()),
                          static_cast<quint16>(port));
    }
    if (duration)
        QTimer::singleShot(duration, &app, &QCoreApplication::quit);
    auto result = app.exec();
    qInfo("Processed %llu spectrum frames", static_cast<unsigned long long>(engine.processed_frames()));
    return result;
}
