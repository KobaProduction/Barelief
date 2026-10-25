#include "archive_blocks.h"
#include <blosc2.h>
#include <memory>

namespace desktop_analysis::offline {
namespace {
using Context=std::unique_ptr<blosc2_context,decltype(&blosc2_free_ctx)>;
bool lossless_special(unsigned char flags) {
    return flags==0 || flags==(BLOSC2_SPECIAL_ZERO<<4) || flags==(BLOSC2_SPECIAL_VALUE<<4);
}
bool lossless_filters(const unsigned char* data) {
    for(int i=16;i<21;++i)if(data[i]!=BLOSC_NOFILTER)return false;
    if(data[21]!=BLOSC_SHUFFLE)return false;
    for(int i=24;i<30;++i)if(data[i])return false;
    return true;
}
bool block_header(QByteArrayView packed,unsigned width,qsizetype expected) {
    if(packed.size()<BLOSC_EXTENDED_HEADER_LENGTH || expected<0 || expected>64*1024*1024)return false;
    const auto* data=reinterpret_cast<const unsigned char*>(packed.data());
    if(data[0]!=BLOSC2_VERSION_FORMAT_STABLE || data[3]!=width || data[22]!=BLOSC_ZSTD || data[23] || data[30] || !lossless_special(data[31]))return false;
    if(!lossless_filters(data))return false;
    size_t size=0;
    if(blosc1_cbuffer_validate(packed.data(),size_t(packed.size()),&size)!=0 || size!=size_t(expected))return false;
    int32_t nbytes=0,cbytes=0,blocksize=0;
    return blosc2_cbuffer_sizes(packed.data(),&nbytes,&cbytes,&blocksize)==0 && nbytes==expected && cbytes==packed.size();
}
}
QByteArray compress_music_block(QByteArrayView raw,unsigned width) {
    if((width!=4 && width!=8) || raw.size()>64*1024*1024 || raw.size()%width)return {};
    auto params=BLOSC2_CPARAMS_DEFAULTS;params.compcode=BLOSC_ZSTD;params.clevel=5;params.typesize=int32_t(width);params.nthreads=1;
    params.splitmode=BLOSC_AUTO_SPLIT;
    Context context(blosc2_create_cctx(params),blosc2_free_ctx);
    if(!context)return {};
    QByteArray result(raw.size()+BLOSC2_MAX_OVERHEAD,Qt::Uninitialized);
    const auto size=blosc2_compress_ctx(context.get(),raw.data(),int32_t(raw.size()),result.data(),int32_t(result.size()));
    if(size<=0)return {};
    result.resize(size);return result;
}
bool decompress_music_block(QByteArrayView packed,unsigned width,qsizetype expected,QByteArray& raw) {
    if(!block_header(packed,width,expected))return false;
    auto params=BLOSC2_DPARAMS_DEFAULTS;params.nthreads=1;
    Context context(blosc2_create_dctx(params),blosc2_free_ctx);
    if(!context)return false;
    QByteArray decoded(expected,Qt::Uninitialized);
    const auto size=blosc2_decompress_ctx(context.get(),packed.data(),int32_t(packed.size()),decoded.data(),int32_t(decoded.size()));
    if(size!=expected)return false;
    raw=std::move(decoded);return true;
}
}
