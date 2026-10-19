#include <bdsp_protocol/version_admission.h>
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
bool persisted_command_valid(const nc::Message& message,const nc::Command* command) noexcept {
    return command && command->id==nc::CommandId::Prepare && command->graph && !message.client &&
        !message.session && !message.epoch && !message.request && !message.expected_revision &&
        !command->token && !command->graph_revision && command->graph_id.empty() &&
        !command->parameters && command->node==-1;
}
Status persisted_message_status(const nc::Message& message,const nc::Command*& command) noexcept {
    if(!message.extensions.empty())return Status::unsupported_version;
    command=std::get_if<nc::Command>(&message.body);
    if(!persisted_command_valid(message,command))return Status::malformed;
    if(nc::graph_versions_supported(command->graph->versions)!=Status::ok)
        return Status::unsupported_version;
    return Status::ok;
}
Status admission_status(domain::ProcessingError error) noexcept {
    if(error==domain::ProcessingError::UnsupportedVersion || error==domain::ProcessingError::UnknownKind ||
       error==domain::ProcessingError::UnsupportedCombination)return Status::unsupported_version;
    if(error==domain::ProcessingError::ResourceLimit || error==domain::ProcessingError::Limit)return Status::exhausted;
    return Status::incompatible_contract;
}
void write_record_header(std::span<std::uint8_t> bytes,const nc::GraphVersions& versions,
                         nc::CodecId codec,std::uint64_t sequence,std::size_t produced) noexcept {
    std::fill_n(bytes.begin(),ProcessingPersistence::record_header_bytes,0);
    std::memcpy(bytes.data(),"BPS1",4);
    put(bytes,4,1,2);put(bytes,6,codec==nc::CodecId::Cbor?1:2,2);put(bytes,8,1,4);
    put(bytes,12,versions.document,4);put(bytes,16,versions.graph,4);
    put(bytes,20,versions.processing,4);put(bytes,24,versions.data,4);
    put(bytes,28,sequence,8);put(bytes,36,produced,4);put(bytes,44,versions.control,4);
    put(bytes,40,checksum(bytes.first(ProcessingPersistence::record_header_bytes+produced)),4);
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
       get(bytes,12,4)!=nc::GraphVersions{}.document || get(bytes,16,4)!=nc::GraphVersions{}.graph || get(bytes,20,4)!=nc::GraphVersions{}.processing || get(bytes,24,4)!=nc::GraphVersions{}.data || get(bytes,44,4)!=nc::GraphVersions{}.control)
        return {Status::unsupported_version,get(bytes,28,8),{}};
    return {Status::ok,get(bytes,28,8),bytes.subspan(record_header_bytes)};
}
Status ProcessingPersistence::decode_payload(const Record& r,domain::NumericProcessingGraph& graph) {
    if(r.status!=Status::ok)return r.status;
    nc::Message message;
    auto result=codec_.decode(r.payload,buffers_.decode_arena,buffers_.decode_staging,message,codec_limits_);
    if(!result)return codec_status(result.status);
    const nc::Command* command=nullptr;
    const auto message_status=persisted_message_status(message,command);
    if(message_status!=Status::ok)return message_status;
    result=control::numeric::own_graph(*command->graph,codec_limits_,graph);
    if(!result)return codec_status(result.status);
    return Status::ok;
}
Status ProcessingPersistence::decode(const Record& record,domain::NumericProcessingGraph& out) {
    domain::NumericProcessingGraph graph;
    const auto status=decode_payload(record,graph);
    if(status!=Status::ok)return status;
    domain::ProcessingGraphAdmission accepted;
    auto diagnostic=domain::admit_processing_graph(graph,admission_,capabilities_,buffers_.admission_workspace,accepted);
    if(!diagnostic)return admission_status(diagnostic.error);
    out=std::move(graph);return Status::ok;
}
Status ProcessingPersistence::restore(domain::NumericProcessingGraph& out) {
    auto a=read(0,buffers_.first),b=read(1,buffers_.second);
    if(b.sequence>a.sequence)std::swap(a,b);
    auto s=decode(a,out);if(s==Status::ok)return s;
    auto fallback=decode(b,out);if(fallback==Status::ok)return fallback;
    return s==Status::unavailable?fallback:s;
}
Status ProcessingPersistence::select_save_slot(const Record& a,const Record& b,
                                                unsigned& target,std::uint64_t& sequence) {
    // Never overwrite a committed future edition or unreadable oversized record.
    for(auto s:{a.status,b.status})if(s!=Status::ok && s!=Status::malformed && s!=Status::unavailable)return s;
    domain::NumericProcessingGraph candidate;
    auto sa=decode(a,candidate),sb=decode(b,candidate);
    if(sa==Status::unsupported_version || sb==Status::unsupported_version)return Status::unsupported_version;
    const bool va=sa==Status::ok,vb=sb==Status::ok;
    target=va && (!vb || a.sequence>=b.sequence)?1:0;
    sequence=std::max(a.sequence,b.sequence);
    if(sequence==std::numeric_limits<std::uint64_t>::max())return Status::exhausted;
    return Status::ok;
}
Status ProcessingPersistence::encode_record(const domain::NumericProcessingGraph& graph,unsigned target,
                                            std::uint64_t sequence,const nc::GraphVersions& versions) {
    domain::ProcessingGraphAdmission accepted;
    if(!domain::admit_processing_graph(graph,admission_,capabilities_,buffers_.admission_workspace,accepted))return Status::incompatible_contract;
    control::numeric::GraphProjection projection;
    auto projected=projection.assign(graph,codec_limits_);if(!projected)return codec_status(projected.status);
    nc::Command command;command.id=nc::CommandId::Prepare;command.graph=projection.wire();
    nc::Message message;message.body=command;
    return commit_record(message,target,sequence,versions);
}
Status ProcessingPersistence::commit_record(const nc::Message& message,unsigned target,
                                            std::uint64_t sequence,const nc::GraphVersions& versions) {
    auto bytes=target?buffers_.second:buffers_.first;
    if(bytes.size()<record_header_bytes)return Status::buffer_too_small;
    auto result=codec_.encode(message,bytes.subspan(record_header_bytes),buffers_.encode_scratch,codec_limits_);
    if(!result)return codec_status(result.status);
    if(result.produced>std::numeric_limits<std::uint32_t>::max())return Status::exhausted;
    write_record_header(bytes,versions,codec_.id(),sequence+1,result.produced);
    return storage_.commit(target,bytes.first(record_header_bytes+result.produced));
}
Status ProcessingPersistence::save(const domain::NumericProcessingGraph& graph) {
    const nc::GraphVersions versions{bdsp_protocol::document_version,graph.versions.graph,graph.versions.processing,
        bdsp_protocol::control_version,bdsp_protocol::processing_data_version};
    if(nc::graph_versions_supported(versions)!=Status::ok)return Status::unsupported_version;
    auto a=read(0,buffers_.first),b=read(1,buffers_.second);
    unsigned target=0;
    std::uint64_t sequence=0;
    const auto status=select_save_slot(a,b,target,sequence);
    return status==Status::ok?encode_record(graph,target,sequence,versions):status;
}
}
