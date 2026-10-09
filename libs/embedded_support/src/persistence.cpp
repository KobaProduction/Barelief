#include <embedded_support/persistence.h>
#include <control/numeric_processing_message_mapper.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace embedded_support {
using signal_contracts::Status;
namespace nc = bdsp_protocol::numeric_control;
namespace {
void put(std::span<std::uint8_t> b, std::size_t at, std::uint64_t v, unsigned n) noexcept {
    for(unsigned i=0;i<n;++i)b[at+i]=std::uint8_t(v>>(8*i));
}
std::uint64_t get(std::span<const std::uint8_t> b, std::size_t at, unsigned n) noexcept {
    std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[at+i])<<(8*i);return v;
}
std::uint32_t checksum(std::span<const std::uint8_t> b) noexcept {
    std::uint32_t crc=~0u;
    for(std::size_t i=0;i<b.size();++i) {
        // CRC field is treated as zero; header, editions and payload are covered.
        crc^=(i>=40 && i<44)?0:b[i];
        for(unsigned k=0;k<8;++k)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}
Status codec_status(nc::Status s) noexcept {
    if(s==nc::Status::Ok)return Status::ok;
    if(s==nc::Status::UnsupportedVersion || s==nc::Status::UnsupportedField)return Status::unsupported_version;
    if(s==nc::Status::Limit || s==nc::Status::Overflow)return Status::exhausted;
    if(s==nc::Status::BufferTooSmall)return Status::buffer_too_small;
    return Status::malformed;
}
}
ProcessingPersistence::ProcessingPersistence(ProcessingStoragePort& storage, PersistenceBuffers buffers,
    const nc::Codec& codec,nc::Limits limits,domain::ProcessingAdmissionLimits admission,
    domain::NumericDeviceCapabilities caps) noexcept
    :storage_(storage),buffers_(buffers),codec_(codec),codec_limits_(limits),admission_(admission),capabilities_(caps) {}
ProcessingPersistence::Record ProcessingPersistence::read(unsigned slot,std::span<std::uint8_t> b) noexcept {
    std::size_t n=0;auto status=storage_.read(slot,b,n);
    if(status!=Status::ok)return {status,0,{}};
    if(n>b.size() || n<record_header_bytes)return {Status::malformed,0,{}};
    auto bytes=b.first(n);
    if(std::memcmp(bytes.data(),"BPS1",4) || get(bytes,8,4)!=1 || get(bytes,36,4)!=n-record_header_bytes ||
       get(bytes,40,4)!=checksum(bytes) || !get(bytes,28,8))return {Status::malformed,0,{}};
    if(get(bytes,4,2)!=1 || get(bytes,6,2)!=(codec_.id()==nc::CodecId::Cbor?1u:2u) ||
       get(bytes,12,4)!=2 || get(bytes,16,4)!=3 || get(bytes,20,4)!=2 || get(bytes,24,4)!=4 || get(bytes,44,4)!=2)
        return {Status::unsupported_version,get(bytes,28,8),{}};
    return {Status::ok,get(bytes,28,8),bytes.subspan(record_header_bytes)};
}
Status ProcessingPersistence::decode(const Record& r,domain::NumericProcessingGraph& out) {
    if(r.status!=Status::ok)return r.status;
    nc::Message message;
    auto result=codec_.decode(r.payload,buffers_.decode_arena,buffers_.decode_staging,message,codec_limits_);
    if(!result)return codec_status(result.status);
    if(!message.extensions.empty())return Status::unsupported_version;
    auto* command=std::get_if<nc::Command>(&message.body);
    // Persisted envelope is normalized; reject ephemeral or unrelated operations.
    if(!command || command->id!=nc::CommandId::Prepare || !command->graph || message.client || message.session ||
       message.epoch || message.request || message.expected_revision || command->token ||
       command->graph_revision || !command->graph_id.empty() || command->parameters || command->node!=-1)
        return Status::malformed;
    const auto& versions=command->graph->versions;
    if(versions.document!=2 || versions.graph!=3 || versions.processing!=2 || versions.control!=2 || versions.data!=4)
        return Status::unsupported_version;
    domain::NumericProcessingGraph graph;
    result=control::numeric::own_graph(*command->graph,codec_limits_,graph);
    if(!result)return codec_status(result.status);
    domain::ProcessingGraphAdmission accepted;
    auto diagnostic=domain::admit_processing_graph(graph,admission_,capabilities_,buffers_.admission_workspace,accepted);
    if(!diagnostic)return diagnostic.error==domain::ProcessingError::UnsupportedVersion ||
        diagnostic.error==domain::ProcessingError::UnknownKind || diagnostic.error==domain::ProcessingError::UnsupportedCombination?
        Status::unsupported_version:
        diagnostic.error==domain::ProcessingError::ResourceLimit || diagnostic.error==domain::ProcessingError::Limit?
        Status::exhausted:Status::incompatible_contract;
    out=std::move(graph);return Status::ok;
}
Status ProcessingPersistence::restore(domain::NumericProcessingGraph& out) {
    auto a=read(0,buffers_.first),b=read(1,buffers_.second);
    if(b.sequence>a.sequence)std::swap(a,b);
    auto s=decode(a,out);if(s==Status::ok)return s;
    auto fallback=decode(b,out);if(fallback==Status::ok)return fallback;
    return s==Status::unavailable?fallback:s;
}
Status ProcessingPersistence::save(const domain::NumericProcessingGraph& graph) {
    const auto& versions=graph.versions;
    if(versions.document!=2 || versions.graph!=3 || versions.processing!=2 || versions.control!=2 || versions.data!=4)
        return Status::unsupported_version;
    auto a=read(0,buffers_.first),b=read(1,buffers_.second);
    // Never overwrite a committed future edition or unreadable oversized record.
    for(auto s:{a.status,b.status})if(s!=Status::ok && s!=Status::malformed && s!=Status::unavailable)return s;
    domain::NumericProcessingGraph candidate;
    auto sa=decode(a,candidate),sb=decode(b,candidate);
    if(sa==Status::unsupported_version || sb==Status::unsupported_version)return Status::unsupported_version;
    const bool va=sa==Status::ok,vb=sb==Status::ok;
    unsigned target=va && (!vb || a.sequence>=b.sequence)?1:0;
    auto sequence=std::max(a.sequence,b.sequence);
    if(sequence==std::numeric_limits<std::uint64_t>::max())return Status::exhausted;
    domain::ProcessingGraphAdmission accepted;
    if(!domain::admit_processing_graph(graph,admission_,capabilities_,buffers_.admission_workspace,accepted))return Status::incompatible_contract;
    control::numeric::GraphProjection projection;
    auto projected=projection.assign(graph,codec_limits_);if(!projected)return codec_status(projected.status);
    nc::Command command;command.id=nc::CommandId::Prepare;command.graph=projection.wire();
    nc::Message message;message.body=command;
    auto bytes=target?buffers_.second:buffers_.first;
    if(bytes.size()<record_header_bytes)return Status::buffer_too_small;
    auto result=codec_.encode(message,bytes.subspan(record_header_bytes),buffers_.encode_scratch,codec_limits_);
    if(!result)return codec_status(result.status);
    if(result.produced>std::numeric_limits<std::uint32_t>::max())return Status::exhausted;
    std::fill_n(bytes.begin(),record_header_bytes,0);std::memcpy(bytes.data(),"BPS1",4);
    put(bytes,4,1,2);put(bytes,6,codec_.id()==nc::CodecId::Cbor?1:2,2);put(bytes,8,1,4);
    put(bytes,12,2,4);put(bytes,16,3,4);put(bytes,20,2,4);put(bytes,24,4,4);put(bytes,28,sequence+1,8);
    put(bytes,36,result.produced,4);put(bytes,44,2,4);
    auto record=bytes.first(record_header_bytes+result.produced);put(bytes,40,checksum(record),4);
    return storage_.commit(target,record);
}
}
