#include "archive_zip.h"
#include <miniz.h>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <limits>

namespace desktop_analysis::offline {
namespace {
size_t read_at(void* opaque,mz_uint64 offset,void* destination,size_t size) {
    auto& device=*static_cast<QIODevice*>(opaque);
    if(offset>mz_uint64(std::numeric_limits<qint64>::max()) || size>size_t(music_archive_limit) || !device.seek(qint64(offset)))return 0;
    return size_t(std::max(qint64(0),device.read(static_cast<char*>(destination),qint64(size))));
}
size_t write_at(void* opaque,mz_uint64 offset,const void* source,size_t size) {
    auto& device=*static_cast<QIODevice*>(opaque);
    if(offset>mz_uint64(music_archive_limit) || size>size_t(music_archive_limit)-size_t(offset) || !device.seek(qint64(offset)))return 0;
    return size_t(std::max(qint64(0),device.write(static_cast<const char*>(source),qint64(size))));
}
bool safe_name(const QString& name) {
    if(name.isEmpty() || name.size()>240 || name.startsWith('/') || name.contains('\\') || name.contains(':') || name.contains(QChar::Null))return false;
    const auto parts=name.split('/');
    return std::ranges::none_of(parts,[](const QString& p){return p.isEmpty() || p=="." || p=="..";});
}
bool zip_error(mz_zip_archive& zip,QString& error) {
    error=QStringLiteral("Invalid music archive: %1").arg(QString::fromLatin1(mz_zip_get_error_string(mz_zip_get_last_error(&zip))));return false;
}
}
struct StoredZipReader::Impl {
    QIODevice& device;
    mz_zip_archive zip{};
    QMap<QString,mz_uint> index;
    bool active=false;
    explicit Impl(QIODevice& d):device(d){zip.m_pRead=read_at;zip.m_pIO_opaque=&device;}
    ~Impl(){if(active)mz_zip_reader_end(&zip);}
    bool directory(QString& error) {
        const auto count=mz_zip_reader_get_num_files(&zip);
        if(count>50000){error="Too many music archive entries";return false;}
        for(mz_uint i=0;i<count;++i)if(!entry(i,error))return false;
        return true;
    }
    bool entry(mz_uint i,QString& error) {
        mz_zip_archive_file_stat stat{};
        if(!mz_zip_reader_file_stat(&zip,i,&stat))return zip_error(zip,error);
        const auto name=QString::fromUtf8(stat.m_filename);
        const auto length=mz_zip_reader_get_filename(&zip,i,nullptr,0);
        if(!safe_name(name) || length!=mz_uint(name.toUtf8().size()+1) || name.toUtf8()!=QByteArray(stat.m_filename) || index.contains(name) || stat.m_method!=0 || stat.m_is_encrypted || stat.m_is_directory || stat.m_uncomp_size!=stat.m_comp_size || stat.m_uncomp_size>mz_uint64(music_archive_limit)){
            error="Unsupported, duplicate or unsafe music archive entry";return false;
        }
        index.insert(name,i);return true;
    }
};
StoredZipReader::StoredZipReader(QIODevice& d):impl_(std::make_unique<Impl>(d)){}
StoredZipReader::~StoredZipReader()=default;
bool StoredZipReader::open(QString& error) {
    if(impl_->device.size()<0 || impl_->device.size()>music_archive_limit){error="Music archive exceeds 1 GiB limit";return false;}
    if(!mz_zip_reader_init(&impl_->zip,mz_uint64(impl_->device.size()),0))return zip_error(impl_->zip,error);
    impl_->active=true;
    return impl_->directory(error);
}
QStringList StoredZipReader::entries()const{return impl_->index.keys();}
bool StoredZipReader::read(const QString& name,qint64 maximum,QByteArray& output,QString& error) {
    const auto at=impl_->index.constFind(name);
    if(at==impl_->index.cend()){error="Missing music archive entry: "+name;return false;}
    mz_zip_archive_file_stat stat{};
    if(!mz_zip_reader_file_stat(&impl_->zip,at.value(),&stat))return zip_error(impl_->zip,error);
    if(maximum<0 || stat.m_uncomp_size>mz_uint64(maximum)){error="Oversized music archive entry: "+name;return false;}
    QByteArray data(qsizetype(stat.m_uncomp_size),Qt::Uninitialized);
    if(!mz_zip_reader_extract_to_mem(&impl_->zip,at.value(),data.data(),size_t(data.size()),0))return zip_error(impl_->zip,error);
    output=std::move(data);return true;
}
struct StoredZipWriter::Impl {
    QIODevice& device;
    mz_zip_archive zip{};
    bool active=false;
    QSet<QString> names;
    explicit Impl(QIODevice& d):device(d){zip.m_pWrite=write_at;zip.m_pIO_opaque=&device;}
    ~Impl(){if(active)mz_zip_writer_end(&zip);}
};
StoredZipWriter::StoredZipWriter(QIODevice& d):impl_(std::make_unique<Impl>(d)){}
StoredZipWriter::~StoredZipWriter()=default;
bool StoredZipWriter::open(QString& error) {
    if(!mz_zip_writer_init(&impl_->zip,0))return zip_error(impl_->zip,error);
    impl_->active=true;return true;
}
bool StoredZipWriter::add(const QString& name,QByteArrayView bytes,QString& error) {
    if(!safe_name(name) || impl_->names.contains(name) || impl_->names.size()>=50000){error="Unsafe or duplicate music archive entry name";return false;}
    impl_->names.insert(name);
    const auto encoded=name.toUtf8();
    return mz_zip_writer_add_mem(&impl_->zip,encoded.constData(),bytes.data(),size_t(bytes.size()),MZ_NO_COMPRESSION) || zip_error(impl_->zip,error);
}
bool StoredZipWriter::finish(QString& error) {
    if(!mz_zip_writer_finalize_archive(&impl_->zip))return zip_error(impl_->zip,error);
    if(!mz_zip_writer_end(&impl_->zip))return zip_error(impl_->zip,error);
    impl_->active=false;return true;
}
}
