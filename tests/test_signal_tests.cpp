#include "pipeline/audio_pipeline.h"
#include "pipeline/test_signal.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <QDebug>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> static void wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "TestSignal operation timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    try {
        QString error;
        auto document = pipeline::GraphProgram::test_signal_defaults();
        auto program = pipeline::GraphProgram::compile(document,error);
        require(program && program->capture_source() == 2, "Test source graph did not select the in-memory producer");
        for (auto pair : {std::pair{"tones", 4.0}, {"tones",8.5}, {"amplitude",1.1},
                         {"maximum_hz",30000.0}, {"period_seconds",0.0}, {"sample_rate",44100.5}}) {
            auto invalid = document; auto nodes = invalid["nodes"].toArray(); auto node = nodes[0].toObject();
            auto params = node["internal-data"].toObject(); params[pair.first] = pair.second;
            node["internal-data"] = params; nodes[0] = node; invalid["nodes"] = nodes;
            require(!pipeline::GraphProgram::compile(invalid,error), "Invalid generator setting accepted");
        }
        auto roundtrip = QJsonDocument::fromJson(QJsonDocument(document).toJson()).object();
        require(pipeline::GraphProgram::compile(roundtrip,error)->source_parameters() == program->source_parameters(),
                "Generator graph roundtrip lost parameters");

        pipeline::AudioPipeline engine;
        require(engine.apply_graph(document,error), "Cannot apply generator graph");
        std::atomic<uint64_t> frames{0}, latest_generation{0};
        std::atomic<bool> nonzero{false}, stereo{false}, invalid_pcm{false}, bad_clock{false};
        std::mutex pcm_mutex;
        pipeline::AudioFrame last;
        engine.set_audio_handler([&](pipeline::AudioFrame frame) {
            if (frame.left.empty() || frame.left.size() != frame.right.size()) invalid_pcm = true;
            for (size_t i=0; i<frame.left.size(); ++i) {
                if (!std::isfinite(frame.left[i]) || !std::isfinite(frame.right[i]) ||
                    std::abs(frame.left[i]) > 0.40001 || std::abs(frame.right[i]) > 0.40001) invalid_pcm = true;
                if (std::abs(frame.left[i]) > 1e-6) nonzero = true;
                if (std::abs(frame.left[i] - frame.right[i]) > 1e-6) stereo = true;
            }
            std::lock_guard lock(pcm_mutex);
            if (last.generation == frame.generation && last.sequence && frame.timestamp_ns <= last.timestamp_ns) bad_clock = true;
            latest_generation = frame.generation; last = std::move(frame); ++frames;
        });
        engine.start_analyzer(); engine.start_capture();
        wait_for([&] { return frames >= 30 && engine.processed_frames() >= 6; });
        require(nonzero && stereo && !invalid_pcm && !bad_clock, "Invalid stereo signal, amplitude or sample clock");
        require(engine.capture_diagnostics().backend == "In-memory test generator", "Generator used a physical capture backend");
        const auto before = engine.processed_frames();
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // No GUI/main event processing.
        require(engine.processed_frames() - before >= 25 && engine.history_count() >= 25,
                "Generator/DSP/history depend on GUI event delivery");
        const auto old_generation = engine.capture_generation();
        auto nodes = document["nodes"].toArray(); auto node = nodes[0].toObject();
        auto params = node["internal-data"].toObject(); params["amplitude"] = 0;
        params["sample_rate"] = 44100; node["internal-data"] = params; nodes[0] = node; document["nodes"] = nodes;
        require(engine.apply_graph(document,error), "Cannot reconfigure generator");
        wait_for([&] { return latest_generation > old_generation; });
        {
            std::lock_guard lock(pcm_mutex);
            require(last.sample_rate == 44100 && std::all_of(last.left.begin(),last.left.end(),[](float v){return v==0;}) &&
                std::all_of(last.right.begin(),last.right.end(),[](float v){return v==0;}),
                "Changed generator parameters did not restart the real source");
        }
        engine.stop_capture(); const auto stopped = frames.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(frames == stopped, "Audio callback delivered after Stop returned");
        engine.start_capture(); wait_for([&] { return frames > stopped; }); engine.stop_capture(); engine.stop_analyzer();

        QTemporaryFile file; require(file.open(), "Cannot save test graph");
        file.write(QJsonDocument(roundtrip).toJson()); file.flush();
        QProcess console; auto env = QProcessEnvironment::systemEnvironment(); env.insert("QT_FORCE_STDERR_LOGGING","1");
        console.setProcessEnvironment(env);
        console.start(QCoreApplication::applicationDirPath()+"/pipeline_cli",{"--config",file.fileName(),
            "--capture-diagnostics","--duration","1200"});
        require(console.waitForFinished(5000) && console.exitCode()==0 &&
            console.readAllStandardError().contains("In-memory test generator"), "Console did not execute the same saved generator graph");
        qInfo("Silent TestSignal graph, PCM, clock, lifecycle, GUI independence and console checks passed");
    } catch (const std::exception &error) { qCritical("TestSignal: %s",error.what()); return 1; }
}
