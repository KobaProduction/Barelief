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
    bool open(QString& error);
    QStringList entries() const;
    bool read(const QString&,qint64 maximum,QByteArray& output,QString& error);
};
class StoredZipWriter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit StoredZipWriter(QIODevice&);
    ~StoredZipWriter();
    bool open(QString& error);
    bool add(const QString&,QByteArrayView,QString& error);
    bool finish(QString& error);
};
}
