#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bdsp_protocol::text_detail {
struct Utf8Sequence {
    unsigned remaining=0;
    std::uint32_t value=0,minimum=0;
};
inline Utf8Sequence utf8_prefix(unsigned char lead) noexcept {
    if(lead>=0xc2 && lead<=0xdf)return {1,std::uint32_t(lead&31),0x80};
    if(lead>=0xe0 && lead<=0xef)return {2,std::uint32_t(lead&15),0x800};
    if(lead>=0xf0 && lead<=0xf4)return {3,std::uint32_t(lead&7),0x10000};
    return {};
}
inline bool utf8_continuations(std::string_view text,std::size_t& cursor,Utf8Sequence sequence) noexcept {
    if(sequence.remaining>text.size()-cursor)return false;
    while(sequence.remaining--) {
        const auto byte=static_cast<unsigned char>(text[cursor++]);
        if((byte&0xc0)!=0x80)return false;
        sequence.value=(sequence.value<<6)|(byte&63);
    }
    return sequence.value>=sequence.minimum && sequence.value<=0x10ffff &&
        !(sequence.value>=0xd800 && sequence.value<=0xdfff);
}
// Strict protocol text: reject NUL, overlong encodings, surrogates and invalid scalars.
inline bool utf8_without_nul(std::string_view text) noexcept {
    for(std::size_t cursor=0;cursor<text.size();) {
        const auto lead=static_cast<unsigned char>(text[cursor++]);
        if(!lead)return false;
        if(lead<0x80)continue;
        const auto sequence=utf8_prefix(lead);
        if(!sequence.remaining || !utf8_continuations(text,cursor,sequence))return false;
    }
    return true;
}
}
