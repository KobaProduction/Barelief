#include "music_archive.h"
#include "archive_blocks.h"
#include "profile_codec.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace desktop_analysis::offline {
namespace {
QString content_name(MusicArchiveKind kind){return kind==MusicArchiveKind::Scan?QStringLiteral("scan"):QStringLiteral("profile");}
QJsonObject archive_manifest(const ProfileDocument& document,MusicArchiveKind kind,MusicBlockSink& sink) {
    auto root=document.archive_preserved;
    root["kind"]="barelief.music_archive";root["version"]=1;root["content"]=content_name(kind);
    root["compression"]="blosc2-zstd5-shuffle-v1";
    root["document"]=kind==MusicArchiveKind::Scan?encode_scan_object(document,&sink):encode_profile_object(document,&sink);
    root["blocks"]=sink.descriptors();return root;
}
bool read_manifest(StoredZipReader& zip,MusicArchiveKind kind,QJsonObject& manifest,QString& error) {
    QByteArray bytes;if(!zip.read("manifest.json",music_manifest_limit,bytes,error))return false;
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);manifest=document.object();
    if(parse.error!=QJsonParseError::NoError || !document.isObject() || manifest.value("kind").toString()!="barelief.music_archive" || manifest.value("version").toDouble()!=1){error="Unsupported or malformed music archive manifest";return false;}
    if(manifest.value("content").toString()!=content_name(kind)){error="Music archive contains a different document type";return false;}
    if(manifest.value("compression").toString()!="blosc2-zstd5-shuffle-v1" || !manifest.value("document").isObject() || !manifest.value("blocks").isArray()){error="Unsupported music archive compression or structure";return false;}
    return true;
}
bool decode_archive(const QJsonObject& manifest,MusicArchiveKind kind,MusicBlockSource& source,ProfileDocument& candidate,QString& error,ProfileReadDiagnostic& diagnostic) {
    const auto document=manifest.value("document").toObject();QString codec_error;
    const auto ok=kind==MusicArchiveKind::Scan?decode_scan_object(document,candidate,codec_error,&source):decode_profile_object(document,candidate,codec_error,diagnostic,&source);
    if(ok && source.complete())return true;
    if(error.isEmpty())error=codec_error;
    if(diagnostic.reason==ProfileReadIssue::None)diagnostic={ProfileReadIssue::Encoding,QStringLiteral("archive")};
    return false;
}
bool load_archive_document(StoredZipReader& zip,MusicArchiveKind kind,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic) {
    QJsonObject manifest;if(!read_manifest(zip,kind,manifest,error))return false;
    MusicBlockSource source(zip,error);if(!source.open(manifest.value("blocks").toArray()))return false;
    ProfileDocument candidate;
    if(!decode_archive(manifest,kind,source,candidate,error,diagnostic))return false;
    candidate.archive_preserved=manifest;output=std::move(candidate);diagnostic={};return true;
}
bool write_archive_document(StoredZipWriter& zip,const ProfileDocument& document,MusicArchiveKind kind,QString& error) {
    MusicBlockSink sink(zip,error,document.archive_preserved.value("blocks").toArray());
    const auto manifest=archive_manifest(document,kind,sink);
    if(!sink.valid())return false;
    const auto bytes=QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if(bytes.size()>music_manifest_limit){error="Music archive manifest exceeds 64 MiB limit";return false;}
    return zip.add("manifest.json",bytes,error) && zip.finish(error);
}
}
bool save_music_archive(const QString& path,const ProfileDocument& document,MusicArchiveKind kind,QString& error) {
    error.clear();QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)){error=file.errorString();return false;}
    StoredZipWriter zip(file);if(!zip.open(error) || !write_archive_document(zip,document,kind,error))return false;
    if(!file.commit()){error=file.errorString();return false;}
    return true;
}
bool load_music_archive(const QString& path,MusicArchiveKind kind,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic) {
    error.clear();diagnostic={ProfileReadIssue::Encoding,QStringLiteral("archive")};QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    StoredZipReader zip(file);if(!zip.open(error))return false;
    return load_archive_document(zip,kind,output,error,diagnostic);
}
}
