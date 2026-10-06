#include "pipeline/graph.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <stdexcept>
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        QTcpServer probe;
        require(probe.listen(QHostAddress::LocalHost, 0), "probe listen");
        auto graph = pipeline::GraphProgram::test_signal_defaults();
        QJsonObject config{
            {"unknown", QJsonObject{{"preserve", 42}}},
            {"pipeline_graph", graph},
            {"aggregator", QJsonObject{{"enabled", true}, {"port", int(probe.serverPort())}}},
            {"presets", QJsonArray{QJsonObject{{"id", "silent-id"}, {"name", "Silent tones"}, {"graph", graph}}}}};
        auto path = dir.filePath("settings.json");
        QFile f(path);
        require(f.open(QIODevice::WriteOnly), "write fixture");
        auto original = QJsonDocument(config).toJson();
        f.write(original);
        f.close();
        auto run = [&](QStringList args, int code) {
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(QCoreApplication::applicationDirPath() + "/pipeline_cli.exe", args);
            require(p.waitForFinished(5000), "CLI timeout");
            require(p.exitStatus() == QProcess::NormalExit && p.exitCode() == code, "CLI selection exit code");
            QCoreApplication::processEvents();
            require(!probe.hasPendingConnections(), "local graph opened registry connection");
        };
        run({"--config", path, "--preset", "silent-id", "--duration", "200"}, 0);
        run({"--config", path, "--preset", "Silent tones", "--duration", "200"}, 0);
        run({"--config", path, "--preset", "missing", "--duration", "200"}, 2);
        run({"--preset", "silent-id", "--duration", "200"}, 2);
        require(f.open(QIODevice::ReadOnly), "read settings");
        require(f.readAll() == original, "CLI changed document");
        f.close();
        auto raw = dir.filePath("raw.json");
        QFile r(raw);
        require(r.open(QIODevice::WriteOnly), "write raw");
        r.write(QJsonDocument(graph).toJson());
        r.close();
        run({"--config", raw, "--duration", "200"}, 0);
        run({"--serial-port","__codex_missing_uart__","--duration","100"},2);
        run({"--serial-port","__codex_missing_uart__","--serial-input-node","0","--serial-baud","invalid"},2);
        run({"--remote","127.0.0.1","--serial-port","__codex_missing_uart__"},2);
        const auto uart_path=dir.filePath("uart.json");
        QFile uart(uart_path);
        const auto uart_graph=QJsonObject{{"version",2},{"unknown-uart",42},{"nodes",QJsonArray{
            QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","uart"}}}},
            QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","MelView"}}}}}},
            {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}}}};
        const auto uart_original=QJsonDocument(uart_graph).toJson();
        require(uart.open(QIODevice::WriteOnly),"UART config fixture");uart.write(uart_original);uart.close();
        // Endpoint failure is recoverable; no COM device or audio capture is used.
        run({"--config",uart_path,"--serial-port","__codex_missing_uart__","--serial-input-node","0","--duration","100"},0);
        require(uart.open(QIODevice::ReadOnly) && uart.readAll()==uart_original,"Serial CLI changed JSON document");uart.close();
        config["pipeline_graph"] = QJsonObject{{"version", 2}, {"nodes", QJsonArray{}}};
        require(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "invalid fixture");
        f.write(QJsonDocument(config).toJson());
        f.close();
        run({"--config", path, "--duration", "200"}, 2);
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 1;
    }
}
