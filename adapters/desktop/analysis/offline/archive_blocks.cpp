#include "archive_blocks.h"
#include <QCryptographicHash>
#include <cmath>

namespace desktop_analysis::offline {
namespace {
unsigned type_width(const QJsonObject& o){const auto t=o.value("dtype").toString();return t=="float32_le"?4:t=="float64_le"?8:0;}
bool integer(const QJsonValue& v,qsizetype maximum,qsizetype& output) {
    if(!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble()<0 || v.toDouble()>double(maximum) || std::floor(v.toDouble())!=v.toDouble())return false;
    output=qsizetype(v.toDouble());return true;
}
bool block_size(const QJsonObject& object,qsizetype& bytes) {
    const auto width=type_width(object);const auto shape=object.value("shape").toArray();
    if(!width || shape.isEmpty() || shape.size()>2)return false;
    qsizetype elements=1;
    for(const auto& v:shape){qsizetype dimension=0;if(!integer(v,64*1024*1024/width,dimension) || !dimension || dimension>(64*1024*1024/width)/elements)return false;elements*=dimension;}
    bytes=elements*width;return object.value("bytes").toDouble(-1)==double(bytes);
}
QString digest(QByteArrayView raw){return QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex());}
QJsonObject descriptor(const QString& entry,QByteArrayView raw,unsigned width,std::size_t columns,QJsonObject o) {
    const auto count=raw.size()/width;
    o["entry"]=entry;o["dtype"]=width==4?"float32_le":"float64_le";
    o["shape"]=columns?QJsonArray{double(count/qsizetype(columns)),double(columns)}:QJsonArray{double(count)};
    o["bytes"]=double(raw.size());o["sha256"]=digest(raw);return o;
}
}
MusicBlockSink::MusicBlockSink(StoredZipWriter& z,QString& e,const QJsonArray& previous):zip_(z),error_(e){
    for(const auto& value:previous){const auto o=value.toObject();previous_.insert(o.value("entry").toString(),o);}
}
QJsonObject MusicBlockSink::store(const QString& name,QByteArrayView raw,unsigned width,std::size_t columns) {
    if(!valid_)return {};
    if((width!=4 && width!=8) || raw.isEmpty() || raw.size()>music_archive_limit-total_ || (columns && raw.size()%(columns*width))){error_="Invalid numeric block geometry or memory budget";valid_=false;return {};}
    const auto packed=compress_music_block(raw,width);const auto entry="arrays/"+name+".b2";
    if(packed.isEmpty() || !zip_.add(entry,packed,error_)){if(error_.isEmpty())error_="Could not compress numeric block";valid_=false;return {};}
    blocks_.append(descriptor(entry,raw,width,columns,previous_.value(entry)));total_+=raw.size();
    return {{"encoding","blosc2-le"},{"entry",entry},{"offset",0},{"count",double(raw.size()/width)}};
}
MusicBlockSource::MusicBlockSource(StoredZipReader& z,QString& e):zip_(z),error_(e){}
bool MusicBlockSource::add_descriptor(const QJsonObject& object,qsizetype& total) {
    const auto entry=object.value("entry").toString(),hash=object.value("sha256").toString();qsizetype bytes=0;
    if(!entry.startsWith("arrays/") || !entry.endsWith(".b2") || blocks_.contains(entry) || !block_size(object,bytes) || bytes>music_archive_limit-total || hash.size()!=64 || QString::fromLatin1(QByteArray::fromHex(hash.toLatin1()).toHex())!=hash){error_="Invalid numeric block descriptor";return false;}
    blocks_.insert(entry,object);total+=bytes;return true;
}
bool MusicBlockSource::open(const QJsonArray& blocks) {
    qsizetype total=0;
    if(blocks.size()>49999){error_="Too many numeric blocks";return false;}
    for(const auto& value:blocks)if(!value.isObject() || !add_descriptor(value.toObject(),total))return false;
    auto expected=blocks_.keys();expected.append("manifest.json");expected.sort();
    if(expected!=zip_.entries()){error_="Missing or unexpected music archive entries";return false;}
    return true;
}
bool MusicBlockSource::load(const QString& entry,const QJsonObject& description,QByteArray& raw) {
    if(const auto* cached=cache_.object(entry)){raw=*cached;return true;}
    const auto size=qsizetype(description.value("bytes").toDouble());QByteArray packed;
    if(!zip_.read(entry,size+32,packed,error_))return false;
    if(!decompress_music_block(packed,type_width(description),size,raw) || digest(raw)!=description.value("sha256").toString()){error_="Damaged numeric block: "+entry;return false;}
    return true;
}
bool MusicBlockSource::read(const QJsonObject& reference,std::size_t count,unsigned width,QByteArray& output) {
    const auto entry=reference.value("entry").toString();const auto at=blocks_.constFind(entry);
    if(at==blocks_.cend() || type_width(at.value())!=width || reference.value("count").toDouble(-1)!=double(count)){error_="Invalid numeric block reference";return false;}
    const auto bytes=qsizetype(at.value().value("bytes").toDouble());qsizetype offset=0;
    if(!integer(reference.value("offset"),bytes/width,offset) || count>std::size_t(bytes/width-offset)){error_="Numeric block slice exceeds its array";return false;}
    QByteArray raw;if(!load(entry,at.value(),raw))return false;
    output=raw.mid(offset*width,qsizetype(count*width));remember(entry,raw,count<std::size_t(bytes/width));
    return true;
}
void MusicBlockSource::remember(const QString& entry,const QByteArray& raw,bool sliced) {
    used_.insert(entry);
    if(sliced && !cache_.contains(entry))cache_.insert(entry,new QByteArray(raw),int((raw.size()+1023)/1024));
}
bool MusicBlockSource::complete() {
    if(used_.size()!=blocks_.size()){error_="Unreferenced numeric block in music archive";return false;}
    return true;
}
}
