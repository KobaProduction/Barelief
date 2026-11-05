// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wave_input.h"
#include <QFile>
#include <QtEndian>
#include <optional>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
QByteArray read(QFile& file,std::uint64_t offset,qsizetype size) {
    require(file.seek(qint64(offset)),"Cannot seek WAV container");const auto result=file.read(size);
    require(result.size()==size,"Truncated WAV container metadata");return result;
}
std::uint16_t u16(const QByteArray& bytes,qsizetype offset) {
    return qFromLittleEndian<std::uint16_t>(bytes.constData()+offset);
}
std::uint32_t u32(const QByteArray& bytes,qsizetype offset) {
    return qFromLittleEndian<std::uint32_t>(bytes.constData()+offset);
}
bool supported_format(std::uint16_t tag,std::uint16_t bits) {
    if(tag==1)return bits==8 || bits==16 || bits==24 || bits==32;
    return tag==3 && (bits==32 || bits==64);
}
struct Format {std::uint32_t rate=0;std::uint16_t alignment=0;};
Format format(QFile& file,std::uint64_t offset,std::uint64_t size) {
    require(size==16 || size==18,"Unsupported WAV fmt structure");const auto bytes=read(file,offset,qsizetype(size));
    const auto tag=u16(bytes,0),channels=u16(bytes,2),alignment=u16(bytes,12),bits=u16(bytes,14);
    const auto rate=u32(bytes,4),byte_rate=u32(bytes,8);
    require(supported_format(tag,bits),"Only PCM and IEEE float WAV formats are supported");
    require(channels>0 && channels<=256 && rate>0,"Invalid WAV channel count or sample rate");
    const auto expected=std::uint64_t(channels)*(bits/8);
    require(alignment==expected && expected<=65535,"Invalid WAV block alignment");
    require(std::uint64_t(byte_rate)==std::uint64_t(rate)*alignment,"Invalid WAV byte rate");
    if(size==18)require(u16(bytes,16)==0,"Unsupported WAV format extension");
    return {rate,alignment};
}
struct Chunk {QByteArray id;std::uint64_t payload=0,size=0,next=0;};
Chunk chunk(QFile& file,std::uint64_t offset,std::uint64_t end) {
    require(offset<=end && end-offset>=8,"Incomplete WAV chunk header");
    const auto header=read(file,offset,8);const auto payload=offset+8;
    const auto size=std::uint64_t(u32(header,4));
    require(size<=end-payload,"WAV chunk extends beyond declared RIFF bounds");
    const auto after=payload+size;
    require((size&1)<=end-after,"Missing WAV odd-chunk padding");
    return {header.first(4),payload,size,after+(size&1)};
}
WaveInputInfo chunks(QFile& file,std::uint64_t end) {
    std::optional<Format> fmt;std::optional<std::uint64_t> data;std::uint64_t offset=12;
    while(offset<end){
        const auto current=chunk(file,offset,end);
        if(current.id=="fmt "){
            require(!fmt && !data,"Duplicate or out-of-order WAV fmt chunk");fmt=format(file,current.payload,current.size);
        }else if(current.id=="data"){
            require(fmt.has_value() && !data,"Missing fmt or duplicate WAV data chunk");data=current.size;
        }
        offset=current.next;
    }
    require(fmt.has_value() && data.has_value(),"WAV requires fmt and data chunks");
    require(*data%fmt->alignment==0,"WAV data ends inside a sample frame");return {*data/fmt->alignment,fmt->rate};
}
}
WaveInputInfo inspect_wave_input(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot open input WAV");
    const auto header=read(file,0,12);
    require(header.first(4)=="RIFF" && header.sliced(8,4)=="WAVE","Only RIFF WAVE containers are supported");
    const auto end=std::uint64_t(u32(header,4))+8;
    require(end>=12 && file.size()>=0 && end==std::uint64_t(file.size()),"Declared RIFF size differs from physical WAV length");
    return chunks(file,end);
}
