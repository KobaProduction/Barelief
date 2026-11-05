#include "music_archive.h"
#include "archive_blocks.h"
#include "profile_codec.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
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

bool open_collection_part(QFile& part,const QString& name,qsizetype index,QString& error) {
    if(name!=QStringLiteral("music-scan-part-%1.brscan").arg(index+1,6,10,QLatin1Char('0')) ||
       !part.open(QIODevice::ReadOnly) || part.size()<=0 || part.size()>music_archive_limit){
        error=QStringLiteral("Invalid scan part for collection: %1").arg(name);
        return false;
    }
    return true;
}
bool append_collection_part(StoredZipWriter& zip,const QDir& temporary_dir,const QJsonObject& item,
                            qsizetype index,QJsonArray& descriptors,QString& error) {
    const auto name=item.value("name").toString();
    QFile part(temporary_dir.filePath(name));
    if(!open_collection_part(part,name,index,error))return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if(!digest.addData(&part) || !part.seek(0)){error="Cannot calculate source part checksum";return false;}
    if(!zip.add_file("parts/"+name,part,error))return false;
    descriptors.append(QJsonObject{
        {"name",name},{"size",double(part.size())},
        {"sha256",QString::fromLatin1(digest.result().toHex())},
        {"requested_count",item.value("requested_count")},
        {"processed_count",item.value("processed_count")},
        {"failed_count",item.value("failed_count")}
    });
    return true;
}
bool append_collection_parts(StoredZipWriter& zip,const QDir& temporary_dir,const QJsonArray& listed,
                             QJsonArray& descriptors,QString& error) {
    for(qsizetype i=0;i<listed.size();++i)
        if(!append_collection_part(zip,temporary_dir,listed.at(i).toObject(),i,descriptors,error))return false;
    return true;
}
bool append_collection_summary(StoredZipWriter& zip,const QJsonObject& metadata,const QJsonObject& summary,
                               const QByteArray& manifest,const QByteArray& index,QString& error) {
    if(summary.isEmpty())return true;
    if(summary.value("kind").toString()!="barelief.music_scan_summary" ||
       summary.value("processed_count").toDouble()!=metadata.value("processed_count").toDouble()){
        error="Cached scan summary mismatches its corpus";return false;
    }
    auto bound=summary;
    bound["manifest_sha256"]=QString::fromLatin1(QCryptographicHash::hash(manifest,QCryptographicHash::Sha256).toHex());
    bound["index_sha256"]=QString::fromLatin1(QCryptographicHash::hash(index,QCryptographicHash::Sha256).toHex());
    const auto packed=QJsonDocument(bound).toJson(QJsonDocument::Compact);
    return zip.add("summary.json",packed,error);
}
bool append_collection_index(StoredZipWriter& zip,const QJsonObject& metadata,const QJsonObject& summary,
                             const QByteArray& manifest,const QJsonArray& descriptors,QString& error) {
    const QJsonObject index{
        {"kind","barelief.music_scan_pack"},{"version",1},{"reference","manifest.json"},
        {"parts",descriptors},
        {"storage","uncompressed ZIP64 of compressed Barelief .brscan members"},
        {"completed",true}
    };
    const auto bytes=QJsonDocument(index).toJson(QJsonDocument::Compact);
    if(!zip.add("pack-index.json",bytes,error))return false;
    return append_collection_summary(zip,metadata,summary,manifest,bytes,error);
}

bool commit_collection_contents(QSaveFile& file,StoredZipWriter& zip,const QDir& temporary_dir,
                                 const QJsonObject& metadata,const QJsonArray& listed,
                                 const QJsonObject& summary,QString& error) {
    const auto manifest=QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if(!zip.add("manifest.json",manifest,error))return false;
    QJsonArray descriptors;
    if(!append_collection_parts(zip,temporary_dir,listed,descriptors,error))return false;
    if(!append_collection_index(zip,metadata,summary,manifest,descriptors,error))return false;
    if(!zip.finish(error))return false;
    if(!file.commit()){error=file.errorString();return false;}
    return true;
}
}
bool save_music_scan_collection(const QDir& temporary_dir,const QString& output,
                          const QJsonObject& metadata,const QJsonArray& listed,QString& error,
                          const QJsonObject& summary) {
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)){error=file.errorString();return false;}
    StoredZipWriter zip(file);
    if(!zip.open(error,true))return false;
    return commit_collection_contents(file,zip,temporary_dir,metadata,listed,summary,error);
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
