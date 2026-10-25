#include "profile_arrays.h"
#ifdef _WIN32
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif
#include <limits>

namespace desktop_analysis::offline::array_detail {
namespace {
QString encoding(unsigned width){return width==4?QStringLiteral("f32le-qcompress-base64"):QStringLiteral("f64le-qcompress-base64");}
bool packed_bytes(const QJsonObject& object,std::size_t expected,unsigned width,QByteArray& packed) {
    if(object.value("encoding").toString()!=encoding(width) || object.value("count").toDouble(-1)!=double(expected) || !object.value("data").isString())return false;
    const auto text=object.value("data").toString().toLatin1();
    const auto conversion=QByteArray::fromBase64Encoding(text,QByteArray::AbortOnBase64DecodingErrors);
    if(!conversion || conversion.decoded.toBase64()!=text || conversion.decoded.size()<4)return false;
    packed=conversion.decoded;return true;
}
bool legacy_bytes(const QJsonObject& object,std::size_t expected,unsigned width,QByteArray& raw) {
    QByteArray packed;if(!packed_bytes(object,expected,width,packed))return false;
    std::uint32_t length=0;
    for(int j=0;j<4;++j)length=(length<<8)|static_cast<unsigned char>(packed[j]);
    if(expected>std::numeric_limits<std::uint32_t>::max()/width || length!=expected*width)return false;
    raw.resize(qsizetype(length));uLongf output_size=length;uLong input_size=uLong(packed.size()-4);
    return uncompress2(reinterpret_cast<Bytef*>(raw.data()),&output_size,reinterpret_cast<const Bytef*>(packed.constData()+4),&input_size)==Z_OK && output_size==length && input_size==uLong(packed.size()-4);
}
}
QJsonObject pack(QByteArrayView raw,unsigned width,QJsonObject previous,ProfileArraySink* sink,std::size_t columns,const QString& name) {
    for(const auto* key:{"data","entry","offset"})previous.remove(key);
    if(sink){
        const auto reference=sink->store(name,raw,width,columns);
        for(auto i=reference.begin();i!=reference.end();++i)previous[i.key()]=i.value();
        return previous;
    }
    previous["encoding"]=encoding(width);previous["count"]=double(raw.size()/width);
    previous["data"]=QString::fromLatin1(qCompress(reinterpret_cast<const uchar*>(raw.data()),raw.size(),3).toBase64());return previous;
}
bool unpack(const QJsonObject& object,std::size_t expected,unsigned width,QByteArray& raw,ProfileArraySource* source) {
    if(source && object.value("encoding").toString()=="blosc2-le")return source->read(object,expected,width,raw);
    return legacy_bytes(object,expected,width,raw);
}
}
