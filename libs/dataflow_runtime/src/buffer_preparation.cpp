#include "preparation_support.h"
#include <wled_protocol/wled_packet.h>

namespace dataflow_runtime::detail {
bool numeric_add(std::size_t& a,std::size_t b) noexcept {
    if(b>SIZE_MAX-a)return false;
    a+=b;return true;
}
bool numeric_mul(std::size_t a,std::size_t b,std::size_t& out) noexcept {
    if(a && b>SIZE_MAX/a)return false;
    out=a*b;return true;
}
const ProcessingPort* numeric_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& p:ports)if(p.id==id)return &p;
    return nullptr;
}
bool numeric_geometry(const NumericContract& c,std::size_t& bytes,std::size_t& stride,
    std::size_t& plane,std::array<std::size_t,2>& shape) noexcept {
    std::size_t width=0;
    if(storage_width(c,width)!=Status::ok || c.representation.alignment>alignof(std::max_align_t))return false;
    const auto alignment=c.representation.alignment;
    if(!alignment || width>SIZE_MAX-(alignment-1))return false;
    const auto lane=(width+alignment-1)/alignment*alignment;
    shape={1,1};if(c.shape.rank!=Rank::Scalar)shape[0]=std::size_t(c.shape.extents[0].maximum);
    if(c.shape.rank==Rank::Matrix)shape[1]=std::size_t(c.shape.extents[1].maximum);
    std::size_t elements=0;if(!numeric_mul(shape[0],shape[1],elements))return false;
    if(c.representation.layout==ChannelLayout::Interleaved){
        if(width%alignment || !numeric_mul(width,c.channel_order.size(),stride))return false;
        plane=width;return numeric_mul(elements,stride,bytes);
    }
    stride=lane;if(!numeric_mul(elements,lane,plane))return false;
    return numeric_mul(plane,c.channel_order.size(),bytes);
}
bool zero_bytes(const NumericContract& c) noexcept {
    const auto* dtype=dtype_descriptor(c.dtype);
    return dtype && c.representation.normalization!=Normalization::UnsignedUnit;
}
bool direct_window(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.shape.rank==Rank::Vector &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        c.representation.alignment<=alignof(float) && !c.representation.valid_bits &&
        (c.representation.normalization==Normalization::None || c.representation.normalization==Normalization::FloatUnit);
}
Status numeric_buffer(const NumericContract& c,NumericBuffer& b,const std::shared_ptr<const std::vector<std::byte>>& shared) {
    std::size_t size=0,stride=0,plane=0;
    if(!numeric_geometry(c,size,stride,plane,b.actual))return Status::invalid_argument;
    if(shared && zero_bytes(c)){if(size>shared->size())return Status::exhausted;b.silence=shared;b.silence_bytes=size;}
    else if(c.dtype==DType::Float32 && c.representation.encoding==StorageEncoding::Native)b.floats.resize(size/sizeof(float));
    else b.bytes.resize(size);
    b.planes.resize(c.channel_order.size());
    for(std::size_t i=0;i<b.planes.size();++i)b.planes[i]={i*plane,c.shape.rank==Rank::Matrix?std::array{stride*b.actual[1],stride}:std::array{stride,std::size_t{0}}};
    MutableNumericView v{&c,b.storage(),b.planes,b.actual,{},{}};
    for(std::size_t axis=0;axis<2;++axis)if(c.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        b.coordinates[axis].resize(b.actual[axis]*8*(c.shape.rank==Rank::Matrix && axis==0?2:1));
        v.coordinates.axes[axis]={b.coordinates[axis].data(),b.actual[axis]*8};}
    if(b.silence){v.storage={};auto value=v.view();value.storage=std::as_const(b).storage();value.semantic_silence=true;
        return validate_view({{},value},{b.planes.size(),SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX});}
    return fill_silence(v,{b.planes.size(),SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX});
}
bool packet_contract(const StructuredContract& c) noexcept {
    return c.schema=="wled-audio-sync" && c.version==1 && c.encoding=="audio-sync-44-le" &&
        c.payload_bytes.minimum==wled_protocol::packet_size && c.payload_bytes.maximum==wled_protocol::packet_size;
}
Status numeric_buffer(const DataContract& contract,NumericBuffer& buffer,const std::shared_ptr<const std::vector<std::byte>>& shared) {
    if(const auto* numeric=std::get_if<NumericContract>(&contract))return numeric_buffer(*numeric,buffer,shared);
    const auto& structured=std::get<StructuredContract>(contract);buffer.bytes.resize(std::size_t(structured.payload_bytes.maximum));
    if(packet_contract(structured)){
        const auto result=wled_protocol::silence(0,{reinterpret_cast<std::uint8_t*>(buffer.bytes.data()),buffer.bytes.size()},{0,wled_protocol::packet_size});
        if(result.status!=wled_protocol::Status::Ok)return Status::invalid_argument;
    }
    return Status::ok;
}
NumericGatherRequest numeric_gather_request(const ResolvedProcessingNode& n,const NumericRuntimeProfile& p) noexcept {
    const auto* in=numeric_port(n.operation==ProcessingOperation::History || n.operation==ProcessingOperation::Source || n.operation==ProcessingOperation::Converter?n.outputs:n.inputs,0);
    auto contract=std::get<NumericContract>(in->contract.view());std::span<const std::uint32_t> selected;
    if(n.operation==ProcessingOperation::Reduce){const auto& r=std::get<ReduceProcessingParameters>(*n.parameters);
        if(r.channels==ReductionChannels::SelectedChannels)selected=r.selected_channels;}
    const bool row=contract.shape.rank==Rank::Matrix && (n.operation!=ProcessingOperation::Reduce || std::get<ReduceProcessingParameters>(*n.parameters).rows==ReductionRows::LatestHistoryRow);
    return {contract,selected,{p.admission.frame,p.admission.frame.max_payload_bytes,p.admission.frame.max_metadata_bytes,p.kernels.max_scratch_bytes},row};
}
dsp_kernels::ScalarReduction numeric_reduction(NumericReduction op) noexcept {
    switch(op){case NumericReduction::MaxAbs:return dsp_kernels::ScalarReduction::MaximumAbsolute;
    case NumericReduction::SignedMean:return dsp_kernels::ScalarReduction::SignedMean;
    case NumericReduction::RMS:return dsp_kernels::ScalarReduction::Rms;}
    return dsp_kernels::ScalarReduction::MaximumAbsolute;
}
NumericContract numeric_projection(NumericContract c,std::span<const std::uint32_t> selected,bool matrix_row) noexcept {
    if(matrix_row){c.semantic=c.history_element;c.history_element=Semantic::Numeric;c.shape.rank=Rank::Vector;c.shape.axes={c.shape.axes[1],Axis::None};
        c.shape.extents={c.shape.extents[1],ExtentBounds{1,1}};c.coordinates={c.coordinates[1],AxisCoordinates{}};}
    if(!selected.empty())c.channel_order=selected;
    return c;
}
}
