#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace desktop_analysis::offline {
class ProfileArraySink {
public:
    virtual ~ProfileArraySink()=default;
    virtual QJsonObject store(const QString& name,QByteArrayView raw,unsigned width,std::size_t columns)=0;
};
class ProfileArraySource {
public:
    virtual ~ProfileArraySource()=default;
    virtual bool read(const QJsonObject&,std::size_t count,unsigned width,QByteArray& raw)=0;
};
namespace array_detail {
QJsonObject pack(QByteArrayView,unsigned width,QJsonObject previous,ProfileArraySink*,std::size_t columns,const QString& name);
bool unpack(const QJsonObject&,std::size_t expected,unsigned width,QByteArray&,ProfileArraySource*);
template<class T> QByteArray bytes(const std::vector<T>& values) {
    using Bits=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;
    QByteArray raw(qsizetype(values.size()*sizeof(T)),Qt::Uninitialized);
    for(std::size_t i=0;i<values.size();++i){
        const auto bits=std::bit_cast<Bits>(values[i]);
        for(std::size_t j=0;j<sizeof(T);++j)raw[qsizetype(i*sizeof(T)+j)]=char(bits>>(8*j));
    }
    return raw;
}
template<class T> QJsonObject array(const std::vector<T>& values,const QJsonValue& previous={},ProfileArraySink* sink=nullptr,std::size_t columns=0,const QString& name={}) {
    return pack(bytes(values),sizeof(T),previous.toObject(),sink,columns,name);
}
template<class T> bool numeric(const QJsonArray& source,std::vector<T>& decoded,std::size_t count) {
    if(std::size_t(source.size())!=count)return false;
    for(const auto& item:source){
        if(!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble()<0)return false;
        const auto value=T(item.toDouble());if(!std::isfinite(value))return false;
        decoded.push_back(value);
    }
    return true;
}
template<class T> bool binary(QByteArrayView raw,std::vector<T>& decoded,std::size_t count) {
    using Bits=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;
    if(raw.size()!=qsizetype(count*sizeof(T)))return false;
    for(std::size_t i=0;i<count;++i){
        Bits bits=0;
        for(std::size_t j=0;j<sizeof(T);++j)bits|=Bits(static_cast<unsigned char>(raw[qsizetype(i*sizeof(T)+j)]))<<(8*j);
        const auto value=std::bit_cast<T>(bits);if(!std::isfinite(value) || value<0)return false;
        decoded.push_back(value);
    }
    return true;
}
template<class T> bool vector(const QJsonValue& value,std::vector<T>& result,std::size_t count,std::size_t& budget,ProfileArraySource* source=nullptr) {
    if(count>budget/sizeof(T))return false;
    budget-=count*sizeof(T);std::vector<T> decoded;decoded.reserve(count);
    if(value.isArray()){
        if(!numeric(value.toArray(),decoded,count))return false;
    }else if(value.isObject()){
        QByteArray raw;
        if(!unpack(value.toObject(),count,sizeof(T),raw,source) || !binary(raw,decoded,count))return false;
    }else return false;
    result=std::move(decoded);return true;
}
}
}
