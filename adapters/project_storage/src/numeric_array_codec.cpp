#include <project_storage/numeric_array_codec.h>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
namespace project_storage {
using signal_contracts::Status;
namespace {
constexpr std::string_view encoding="f32le-base64:";
constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
struct Group { std::uint32_t bits=0;unsigned bytes=0; };
bool decode_group(std::string_view text,bool last,Group& out) {
    const auto a=alphabet.find(text[0]),b=alphabet.find(text[1]);
    const bool pad2=text[2]=='=',pad3=text[3]=='=';
    const auto c=pad2?0:alphabet.find(text[2]),d=pad3?0:alphabet.find(text[3]);
    if(a==alphabet.npos || b==alphabet.npos || c==alphabet.npos || d==alphabet.npos)return false;
    if((pad2 && !pad3) || ((pad2 || pad3) && !last))return false;
    if((pad2 && (b&15)) || (pad3 && !pad2 && (c&3)))return false;
    out={std::uint32_t((a<<18)|(b<<12)|(c<<6)|d),pad2?1u:pad3?2u:3u};return true;
}
struct FloatDecoder {
    std::vector<float> values;
    std::uint32_t bits=0;
    unsigned bytes=0;
    bool byte(std::uint32_t value) {
        bits|=value<<(8*bytes++);
        if(bytes!=4)return true;
        const auto scalar=std::bit_cast<float>(bits);
        if(!std::isfinite(scalar) || values.size()>=domain::max_numeric_array_values)return false;
        values.push_back(scalar);bits=0;bytes=0;return true;
    }
    bool group(const Group& value) {
        for(unsigned j=0;j<value.bytes;++j)if(!byte((value.bits>>(16-8*j))&255))return false;
        return true;
    }
};
Status decode_base64(std::string_view text,std::vector<float>& out) {
    if(text.size()%4 || text.size()>4*((domain::max_numeric_array_values*4+2)/3))return Status::invalid_argument;
    FloatDecoder decoder;decoder.values.reserve((text.size()/4*3)/sizeof(float));
    for(std::size_t i=0;i<text.size();i+=4) {
        Group group;
        if(!decode_group(text.substr(i,4),i+4==text.size(),group) || !decoder.group(group))return Status::invalid_argument;
    }
    if(decoder.bytes)return Status::invalid_argument;
    out=std::move(decoder.values);return Status::ok;
}
bool append_csv_value(std::string_view item,std::vector<float>& values) {
    const auto first=item.find_first_not_of(" \t\r\n"),last=item.find_last_not_of(" \t\r\n");
    if(first==std::string_view::npos)return false;
    item=item.substr(first,last-first+1);float value=0;
    const auto parsed=std::from_chars(item.data(),item.data()+item.size(),value);
    if(parsed.ec!=std::errc{} || parsed.ptr!=item.data()+item.size() || !std::isfinite(value) || values.size()>=domain::max_numeric_array_values)return false;
    values.push_back(value);return true;
}
Status parse_csv(std::string_view text,std::vector<float>& values) {
    while(!text.empty()) {
        const auto comma=text.find(',');
        if(!append_csv_value(text.substr(0,comma),values))return Status::invalid_argument;
        if(comma==text.npos)return Status::ok;
        text.remove_prefix(comma+1);
        if(text.empty())return Status::invalid_argument;
    }
    return Status::ok;
}
std::uint32_t encoded_group(std::span<const float> values,std::size_t index) {
    const auto byte=[&](std::size_t i){return (std::bit_cast<std::uint32_t>(values[i/4])>>(8*(i%4)))&255;};
    const auto remaining=values.size()*4-index;
    return (byte(index)<<16)|(remaining>1?byte(index+1)<<8:0)|(remaining>2?byte(index+2):0);
}
void append_encoded(std::string& out,std::uint32_t value,std::size_t remaining) {
    out+=alphabet[(value>>18)&63];out+=alphabet[(value>>12)&63];
    out+=remaining>1?alphabet[(value>>6)&63]:'=';
    out+=remaining>2?alphabet[value&63]:'=';
}
}
std::string encode_array_generator_values(std::span<const float> values) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    if(values.size()>domain::max_numeric_array_values || std::any_of(values.begin(),values.end(),[](float v){return !std::isfinite(v);}))return {};
    std::string result(encoding);result.reserve(encoding.size()+4*((values.size()*4+2)/3));
    for(std::size_t i=0;i<values.size()*4;i+=3)append_encoded(result,encoded_group(values,i),values.size()*4-i);
    return result;
}
Status parse_numeric_array(std::string_view text,domain::NumericArray& out) {
    domain::NumericArray candidate;
    const auto status=text.starts_with(encoding)?decode_base64(text.substr(encoding.size()),candidate.values):parse_csv(text,candidate.values);
    if(status!=Status::ok)return status;
    out=std::move(candidate);return Status::ok;
}
}
