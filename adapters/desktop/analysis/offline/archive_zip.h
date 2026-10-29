#pragma once
#include <QByteArray>
#include <QIODevice>
#include <QStringList>
#include <memory>

namespace desktop_analysis::offline {
inline constexpr qint64 music_archive_limit=1024ll*1024*1024;
inline constexpr qint64 music_manifest_limit=64ll*1024*1024;
class StoredZipReader {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit StoredZipReader(QIODevice&);
    ~StoredZipReader();
    bool open(QString& error,bool large_collection=false);
    QStringList entries() const;
    bool read(const QString&,qint64 maximum,QByteArray& output,QString& error);
    bool extract_to(const QString& name,QIODevice& destination,qint64 maximum,QString& error);
};
class StoredZipWriter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit StoredZipWriter(QIODevice&);
    ~StoredZipWriter();
    bool open(QString& error,bool large_collection=false);
    bool add(const QString&,QByteArrayView,QString& error);
    bool add_file(const QString&,QIODevice& input,QString& error);
    bool finish(QString& error);
};
}
