#include <config/config_document.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
#ifdef _WIN32
#include <windows.h>
#endif

static QByteArray bytes(const QString& path) {
    QFile file(path);assert(file.open(QIODevice::ReadOnly));return file.readAll();
}
static void write(const QString& path,const QByteArray& value) {
    QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(value)==value.size());
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir directory;assert(directory.isValid());
    const auto path=directory.filePath("active.json");
    const QJsonObject graph{{"version",2},{"processing_version",1},{"graph_id","stable"},
        {"nodes",QJsonArray{}},{"connections",QJsonArray{}}};
    const QJsonObject original{{"schema_version",1},{"pipeline_graph",graph},{"scene_graph",false},
        {"ui",QJsonObject{{"theme","dark"},{"unknown",QJsonArray{false,42}}}},
        {"presets",QJsonArray{QJsonObject{{"id","stable-preset"},{"graph",graph}}}},
        {"app-extension",QJsonObject{{"nested",true}}}};
    assert(configuration::save_atomic(path,original));const auto before=bytes(path);
    const auto edited=configuration::overlay(original,{{"ui",QJsonObject{{"theme","light"}}}});
#ifdef _WIN32
    // Actual QSaveFile commit failure: deny destination deletion/replacement.
    // Uses a temp file, no sockets, capture, permissions or user directories.
    const auto native=QDir::toNativeSeparators(path);
    const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    assert(handle!=INVALID_HANDLE_VALUE);
    const auto refused=configuration::save_atomic(path,edited);
    assert(!refused && !refused.error.isEmpty());
    assert(bytes(path)==before);
    assert(CloseHandle(handle));
#endif
    assert(configuration::save_atomic(path,edited));
    const auto saved=configuration::read(path);assert(saved && saved.document==edited);
    assert(saved.document["scene_graph"]==false && saved.document["presets"]==original["presets"]);
    assert(!saved.document.contains("history") && !saved.document.contains("runtime"));
    for(const auto& contents:QList<QByteArray>{QByteArray("{invalid"),QByteArray("{\"schema_version\":2}"),
        QByteArray("{\"schema_version\":1,\"pipeline_graph\":{\"processing_version\":2}}")}) {
        write(path,contents);assert(!configuration::save_atomic(path,edited));assert(bytes(path)==contents);
    }
    write(path,before);
    auto future=edited;future["schema_version"]=2;
    assert(!configuration::save_atomic(path,future) && bytes(path)==before);
    assert(QFileInfo(configuration::active_path()).isAbsolute());
    assert(configuration::active_path(path)==QFileInfo(path).absoluteFilePath());
    assert(configuration::active_path()==QDir(QCoreApplication::applicationDirPath()).filePath("config.json"));
}
