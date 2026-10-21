#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstdio>
#include <stdexcept>
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Run> static void cli_selection_cases(const Run& run,const QString& path) {
        run({"--config", path, "--duration", "200"}, 0);
        run({"--config", path, "--analysis-interval-ms", "25", "--duration", "200"}, 0);
        run({"--config", path, "--analysis-rate", "90", "--duration", "200"}, 0);
        run({"--config", path, "--analysis-rate", "120000/1001", "--duration", "200"}, 0);
        for(const auto rate:{"0","-1","120/0","1/0","1001","invalid"})
            run({"--config", path, "--analysis-rate", rate, "--duration", "200"}, 2);
        run({"--config", path, "--analysis-rate", "120", "--analysis-interval-ms", "25"}, 2);
        run({"--config", path, "--analysis-interval-ms", "0", "--duration", "200"}, 2);
        run({"--config", path, "--analysis-interval-ms", "60001", "--duration", "200"}, 2);
        run({"--config", path, "--analysis-interval-ms", "invalid", "--duration", "200"}, 2);
        // Stable identity takes precedence over another preset's display name.
        run({"--config", path, "--preset", "silent-id", "--duration", "200"}, 0);
        run({"--config", path, "--preset", "second-id", "--duration", "200"}, 3);
        run({"--config", path, "--preset", "Silent tones", "--duration", "200"}, 0);
        run({"--config", path, "--preset", "Ambiguous", "--duration", "200"}, 2);
        run({"--config", path, "--preset", "missing", "--duration", "200"}, 2);
        run({"--preset", "silent-id", "--duration", "200"}, 2);
        // The explicit UART processing endpoint is valid configuration even
        // without a NetworkInput override; unavailable hardware fails at runtime.
        run({"--serial-port", "__codex_missing_uart__", "--duration", "100"}, 3);
        run({"--serial-port", "__codex_missing_uart__", "--serial-input-node", "0", "--serial-baud", "invalid"}, 2);
        run({"--remote", "127.0.0.1", "--serial-port", "__codex_missing_uart__"}, 2);
}
template<class Run> static void rejected_documents(const Run& run,const QTemporaryDir& directory,
    const QByteArray& graph_bytes,const QByteArray& original) {
        auto write = [](const QString& destination, const QByteArray& bytes) {
            QFile target(destination);
            require(target.open(QIODevice::WriteOnly | QIODevice::Truncate), "write rejected fixture");
            require(target.write(bytes) == bytes.size(), "write complete fixture");
        };
        const auto invalid = directory.filePath("invalid.json");
        // A typed graph is an editor snapshot, not a whole project document.
        write(invalid, graph_bytes);
        run({"--config", invalid, "--duration", "200"}, 2);
        auto future = original;
        require(future.contains("\"schema_version\":4"), "project edition marker");
        future.replace("\"schema_version\":4", "\"schema_version\":99");
        write(invalid, future);
        run({"--config", invalid, "--duration", "200"}, 2);
        write(invalid, QByteArray("{\"schema_version\":2,\"scene_graph\":null}"));
        run({"--config", invalid, "--duration", "200"}, 2);
        auto malformed=QJsonDocument::fromJson(original).object();auto ui=malformed["ui"].toObject();auto presets=ui["pipeline_presets"].toArray();auto broken=presets[0].toObject();broken["graph"]=42;presets[0]=broken;ui["pipeline_presets"]=presets;malformed["ui"]=ui;
        write(invalid,QJsonDocument(malformed).toJson(QJsonDocument::Compact));
        run({"--config",invalid,"--preset","silent-id","--duration","200"},2);
        malformed=QJsonDocument::fromJson(original).object();malformed["pipeline_graph"]=false;
        write(invalid,QJsonDocument(malformed).toJson(QJsonDocument::Compact));
        run({"--config",invalid,"--duration","200"},2);
}
static desktop::NumericProjectDocument console_project(const domain::NumericProcessingGraph& graph,
    const QJsonObject& snapshot,QTcpServer& probe) {
        // Structurally canonical but inadmissible: Window parameter/port mismatch.
        // Selecting this distinct graph is observably rejected by Apply (exit 3).
        auto collision_graph=graph;
        std::get<domain::TransformProcessingParameters>(collision_graph.nodes[1].parameters).values.front().value=signal_contracts::NumericLiteral{std::uint64_t{2048}};
        QByteArray collision_bytes;
        require(bool(desktop::encode_numeric_graph(collision_graph,collision_bytes)),"collision graph snapshot");
        const auto collision_snapshot=QJsonDocument::fromJson(collision_bytes).object();
        desktop::NumericDraftPresentation presentation;
        presentation.application_settings = {
            {"ui", QJsonObject{{"pipeline_presets", QJsonArray{
                QJsonObject{{"version", 1}, {"id", "second-id"}, {"name", "silent-id"}, {"graph", collision_snapshot}},
                QJsonObject{{"version", 1}, {"id", "silent-id"}, {"name", "Silent tones"}, {"graph", snapshot}},
                QJsonObject{{"version", 1}, {"id", "third-id"}, {"name", "Ambiguous"}, {"graph", snapshot}},
                QJsonObject{{"version", 1}, {"id", "fourth-id"}, {"name", "Ambiguous"}, {"graph", snapshot}}}}}}};
        desktop::NumericProjectDocument source;
        require(bool(desktop::create_numeric_project(graph, presentation, source)), "canonical project");
        // Unknown source sections are admitted by document reading, not by the
        // registered application-settings writer. Keep this read-only oracle.
        const auto source_bytes = source.source.bytes();
        auto fixture = QJsonDocument::fromJson(QByteArray(source_bytes.data(), source_bytes.size())).object();
        fixture["aggregator"] = QJsonObject{{"enabled", true}, {"port", int(probe.serverPort())}};
        require(bool(desktop::read_numeric_project(QJsonDocument(fixture).toJson(QJsonDocument::Compact), source)),
                "project with unknown registry settings");
        return source;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        QTcpServer probe;
        require(probe.listen(QHostAddress::LocalHost, 0), "registry probe listen");
        const auto graph = numeric_test_graph();
        QByteArray graph_bytes;
        require(bool(desktop::encode_numeric_graph(graph, graph_bytes)), "canonical graph");
        const auto snapshot = QJsonDocument::fromJson(graph_bytes).object();
        auto source=console_project(graph,snapshot,probe);
        const auto path = directory.filePath("settings.json");
        require(bool(desktop::save_numeric_project(source, path)), "save fixture");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "read fixture");
        const auto original = file.readAll(); file.close();
        auto run = [&](const QStringList& arguments, int expected) {
            const auto config_index=arguments.indexOf("--config");QByteArray input_before;QString input_path;
            if(config_index>=0){input_path=arguments.value(config_index+1);QFile input(input_path);require(input.open(QIODevice::ReadOnly),"read CLI input");input_before=input.readAll();}
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(QCoreApplication::applicationDirPath() + "/pipeline_cli.exe", arguments);
            require(process.waitForFinished(10000), "CLI timeout");
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected) {
                const auto output = process.readAll();
                const auto command = arguments.join(' ').toUtf8();
                std::fprintf(stderr, "CLI args: %s\nExpected exit %d, actual %d, status %d\n%s\n",
                             command.constData(), expected, process.exitCode(), int(process.exitStatus()), output.constData());
                require(false, "CLI exit code");
            }
            QCoreApplication::processEvents();
            require(!probe.hasPendingConnections(), "local project implicitly opened registry connection");
            require(file.open(QIODevice::ReadOnly), "read project after CLI");
            const auto unchanged = file.readAll(); file.close();
            require(unchanged == original, "CLI changed project");
            if(!input_path.isEmpty()){QFile input(input_path);require(input.open(QIODevice::ReadOnly)&&input.readAll()==input_before,"CLI changed rejected input");}
        };
        cli_selection_cases(run,path);
        rejected_documents(run,directory,graph_bytes,original);
        std::puts("canonical CLI preset identity/ambiguity/read-only/configuration PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "console_config_tests FAIL: %s\n", error.what());
        return 1;
    }
}
