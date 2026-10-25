#pragma once
#include "archive_zip.h"
#include "profile_arrays.h"
#include <QCache>
#include <QJsonArray>
#include <QMap>
#include <QSet>

namespace desktop_analysis::offline {
QByteArray compress_music_block(QByteArrayView raw,unsigned width);
bool decompress_music_block(QByteArrayView packed,unsigned width,qsizetype expected,QByteArray& raw);
class MusicBlockSink final:public ProfileArraySink {
    StoredZipWriter& zip_;
    QString& error_;
    QMap<QString,QJsonObject> previous_;
    QJsonArray blocks_;
    bool valid_=true;
    qsizetype total_=0;
public:
    MusicBlockSink(StoredZipWriter&,QString&,const QJsonArray& previous);
    QJsonObject store(const QString&,QByteArrayView,unsigned width,std::size_t columns) override;
    QJsonArray descriptors() const{return blocks_;}
    bool valid() const{return valid_;}
};
class MusicBlockSource final:public ProfileArraySource {
    StoredZipReader& zip_;
    QString& error_;
    QMap<QString,QJsonObject> blocks_;
    QSet<QString> used_;
    QCache<QString,QByteArray> cache_{32*1024};
    bool add_descriptor(const QJsonObject&,qsizetype& total);
    bool load(const QString&,const QJsonObject&,QByteArray&);
    void remember(const QString&,const QByteArray&,bool sliced);
public:
    MusicBlockSource(StoredZipReader&,QString&);
    bool open(const QJsonArray&);
    bool complete();
    bool read(const QJsonObject&,std::size_t count,unsigned width,QByteArray&) override;
};
}
