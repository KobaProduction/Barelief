#include <signal_contracts/numeric_frame_gather.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
namespace signal_contracts {
namespace {
constexpr auto maximum=std::numeric_limits<std::size_t>::max();
bool add(std::size_t& a,std::size_t b){if(b>maximum-a)return false;a+=b;return true;}
bool mul(std::size_t a,std::size_t b,std::size_t& out){if(a && b>maximum/a)return false;out=a*b;return true;}
bool align(std::size_t& a,std::size_t alignment){if(!alignment || (alignment&(alignment-1)))return false;return add(a,(alignment-a%alignment)%alignment);}
bool overlap(std::span<const std::byte> a,std::span<const std::byte> b){
    if(a.empty() || b.empty())return false;
    auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
    return x<=y?y-x<a.size():x-y<b.size();
}
template<class T> auto object_bytes(const T& object){return std::as_bytes(std::span(&object,1));}
auto text_bytes(std::string_view text){return std::as_bytes(std::span(text.data(),text.size()));}
std::size_t rank(const NumericContract& c){return c.shape.rank==Rank::Scalar?0:c.shape.rank==Rank::Vector?1:2;}
bool contract_alias(std::span<const std::byte> bytes,const NumericContract& c){
    return overlap(bytes,object_bytes(c)) || overlap(bytes,std::as_bytes(c.channel_order)) || overlap(bytes,text_bytes(c.clock.domain));
}
struct Description {
    NumericGatherRequirements requirements;
    NumericContract output;
    std::size_t planes=0,width=0,plane_block=0,first_stride=0,second_stride=0;
    std::size_t layouts=0,mapping=0,input_ids=0,output_ids=0,clock=0;
    std::array<std::size_t,2> coordinate_offsets{};
    std::size_t time=0,mask=0,stream=0;
};
PlaneLayout compact_plane(const Description& d,std::size_t index){
    return {d.output.representation.layout==ChannelLayout::Interleaved?index*d.width:index*d.plane_block,
        {d.first_stride,d.second_stride}};
}
Status validate_gather_channels(const NumericGatherRequest& request,const Description& description) {
    if(description.planes>request.limits.frame.max_planes)return Status::exhausted;
    for(std::size_t index=0;index<description.planes;++index) {
        const auto id=description.output.channel_order[index];
        if(std::find(request.input.channel_order.begin(),request.input.channel_order.end(),id)==request.input.channel_order.end())
            return Status::incompatible_contract;
        for(std::size_t prior=0;prior<index;++prior)
            if(id==description.output.channel_order[prior])return Status::invalid_argument;
    }
    return Status::ok;
}
Status project_matrix_row(const NumericGatherRequest& request,Description& description) noexcept {
    if(request.input.shape.rank!=Rank::Matrix)return Status::incompatible_contract;
    auto& output=description.output;
    output.semantic=request.input.history_element;
    output.history_element=Semantic::Numeric;
    output.shape={Rank::Vector,{request.input.shape.axes[1],Axis::None},
                  {request.input.shape.extents[1],ExtentBounds{}}};
    output.coordinates={request.input.coordinates[1],AxisCoordinates{}};
    return Status::ok;
}

Status validate_projected_contract(const NumericGatherRequest& r,Description& d) {
    d.planes=d.output.channel_order.size();
    auto status=validate_gather_channels(r,d);if(status!=Status::ok)return status;
    status=validate_contract(d.output,r.limits.frame);if(status!=Status::ok)return status;
    return storage_width(d.output,d.width);
}
Status project_gather_contract(const NumericGatherRequest& r,Description& d) {
    auto status=validate_contract(r.input,r.limits.frame);if(status!=Status::ok)return status;
    d.output=r.input;
    if(r.matrix_row){status=project_matrix_row(r,d);if(status!=Status::ok)return status;}
    if(!r.selected_channels.empty())d.output.channel_order=r.selected_channels;
    return validate_projected_contract(r,d);
}
Status gather_element_stride(const Description& d,std::size_t& stride) noexcept {
    stride=d.width;
    if(d.output.representation.layout==ChannelLayout::Interleaved){
        if(d.planes>1 && d.width%d.output.representation.alignment)return Status::incompatible_contract;
        if(!mul(d.planes,d.width,stride))return Status::overflow;
    }
    return align(stride,d.output.representation.alignment)?Status::ok:Status::overflow;
}

Status gather_plane_block(Description& d,std::size_t dimensions,std::size_t stride) noexcept {
    d.plane_block=d.width;
    if(dimensions==1){
        d.first_stride=stride;
        if(!mul(std::size_t(d.output.shape.extents[0].maximum),stride,d.plane_block))return Status::overflow;
    }
    if(dimensions==2){
        d.second_stride=stride;
        if(!mul(std::size_t(d.output.shape.extents[1].maximum),stride,d.first_stride) ||
           !mul(std::size_t(d.output.shape.extents[0].maximum),d.first_stride,d.plane_block))return Status::overflow;
    }
    return Status::ok;
}

Status prepare_gather_strides(Description& d) {
    std::size_t stride=0;
    const auto status=gather_element_stride(d,stride);
    if(status!=Status::ok)return status;
    return gather_plane_block(d,rank(d.output),stride);
}
Status prepare_gather_payload(Description& d) {
    auto& q=d.requirements;
    q.payload_alignment=d.output.representation.alignment;
    q.scratch_alignment=q.payload_alignment;
    const auto dimensions=rank(d.output);
    auto last=compact_plane(d,d.planes-1);q.payload_bytes=last.offset;
    std::size_t term=0;
    for(std::size_t i=0;i<dimensions;++i)if(!mul(std::size_t(d.output.shape.extents[i].maximum)-1,last.stride_bytes[i],term) || !add(q.payload_bytes,term))return Status::overflow;
    if(!add(q.payload_bytes,d.width))return Status::overflow;
    return Status::ok;
}
Status prepare_gather_geometry(Description& d) {
    const auto strides=prepare_gather_strides(d);
    return strides==Status::ok?prepare_gather_payload(d):strides;
}
bool reserve_plan_array(Description& d,std::size_t count,std::size_t width,std::size_t alignment,
                        std::size_t& offset) {
    auto& bytes=d.requirements.plan_metadata_bytes;
    if(!align(bytes,alignment))return false;
    offset=bytes;
    std::size_t term=0;
    return mul(count,width,term) && add(bytes,term);
}
Status plan_gather_metadata(const NumericGatherRequest& r,Description& d) {
    auto& q=d.requirements;
    q.plan_metadata_alignment=std::max({alignof(PlaneLayout),alignof(std::size_t),alignof(std::uint32_t)});
    if(!reserve_plan_array(d,d.planes,sizeof(PlaneLayout),alignof(PlaneLayout),d.layouts) ||
       !reserve_plan_array(d,d.planes,sizeof(std::size_t),alignof(std::size_t),d.mapping) ||
       !reserve_plan_array(d,r.input.channel_order.size(),sizeof(std::uint32_t),alignof(std::uint32_t),d.input_ids) ||
       !reserve_plan_array(d,d.planes,sizeof(std::uint32_t),alignof(std::uint32_t),d.output_ids))return Status::overflow;
    d.clock=q.plan_metadata_bytes;if(!add(q.plan_metadata_bytes,r.input.clock.domain.size()))return Status::overflow;
    return Status::ok;
}
Status plan_coordinate_metadata(Description& d) noexcept {
    auto& metadata_bytes=d.requirements.output_metadata_bytes;
    for(std::size_t axis=0;axis<2;++axis){
        d.coordinate_offsets[axis]=metadata_bytes;
        if(d.output.coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)continue;
        std::size_t size=0;
        if(!mul(std::size_t(d.output.shape.extents[axis].maximum),8,size) ||
           !add(metadata_bytes,size))return Status::overflow;
    }
    return Status::ok;
}

Status gather_metadata_limits(const NumericGatherRequest& request,const Description& d) noexcept {
    const auto& q=d.requirements;
    if(q.payload_bytes>request.limits.max_output_payload_bytes ||
       q.payload_bytes>request.limits.frame.max_payload_bytes ||
       q.output_metadata_bytes>request.limits.max_output_metadata_bytes ||
       q.scratch_bytes>request.limits.max_scratch_bytes ||
       q.plan_metadata_bytes>request.limits.frame.max_metadata_bytes)return Status::exhausted;
    return Status::ok;
}

Status plan_history_metadata(const NumericGatherRequest& r,Description& d) {
    auto& q=d.requirements;
    d.time=q.output_metadata_bytes;
    if(r.matrix_row && r.input.coordinates[0].mode!=CoordinateMode::None &&
       !add(q.output_metadata_bytes,8))return Status::overflow;
    d.mask=q.output_metadata_bytes;
    if(r.matrix_row && !add(q.output_metadata_bytes,1))return Status::overflow;
    return Status::ok;
}
Status plan_output_metadata(const NumericGatherRequest& r,Description& d) {
    auto& q=d.requirements;
    const auto coordinates=plan_coordinate_metadata(d);
    if(coordinates!=Status::ok)return coordinates;
    const auto history=plan_history_metadata(r,d);
    if(history!=Status::ok)return history;
    d.stream=q.output_metadata_bytes;
    if(!add(q.output_metadata_bytes,r.limits.frame.max_string_bytes))return Status::overflow;
    q.scratch_bytes=q.payload_bytes;
    if(!add(q.scratch_bytes,q.output_metadata_bytes))return Status::overflow;
    return gather_metadata_limits(r,d);
}
Status describe(const NumericGatherRequest& request,Description& description) {
    auto status=project_gather_contract(request,description);if(status!=Status::ok)return status;
    status=prepare_gather_geometry(description);if(status!=Status::ok)return status;
    status=plan_gather_metadata(request,description);if(status!=Status::ok)return status;
    return plan_output_metadata(request,description);
}
bool finite_coordinates(std::span<const std::byte> bytes){
    for(std::size_t i=0;i<bytes.size();i+=8){std::uint64_t bits=0;
        for(unsigned j=0;j<8;++j)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[i+j]))<<(8*j);
        if((bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000))return false;
    }return true;
}
}
struct NumericGatherPlanBuilder {
    static bool storage_disjoint(const NumericGatherRequest& request,const Description& description,
                                 const PreparedNumericGather& output,
                                 std::span<const std::byte> bytes) noexcept {
        return reinterpret_cast<std::uintptr_t>(bytes.data())%description.requirements.plan_metadata_alignment==0 &&
            !overlap(bytes,object_bytes(request)) && !contract_alias(bytes,request.input) &&
            !overlap(bytes,std::as_bytes(request.selected_channels)) && !overlap(bytes,object_bytes(output)) &&
            !overlap(bytes,std::as_const(output.metadata_).bytes());
    }
    static void map_channels(const NumericGatherRequest& request,const Description& description,
                             PlaneLayout* layouts,std::size_t* mapping,std::uint32_t* input_ids,
                             std::uint32_t* output_ids) noexcept {
        std::copy(request.input.channel_order.begin(),request.input.channel_order.end(),input_ids);
        for(std::size_t index=0;index<description.planes;++index) {
            layouts[index]=compact_plane(description,index);
            output_ids[index]=description.output.channel_order[index];
            mapping[index]=std::size_t(std::find(request.input.channel_order.begin(),request.input.channel_order.end(),
                                                 output_ids[index])-request.input.channel_order.begin());
        }
    }
    static void publish_contracts(const NumericGatherRequest& request,const Description& description,
                                  PreparedNumericGather& candidate,
                                  PlaneLayout* layouts,std::size_t* mapping,
                                  std::uint32_t* input_ids,std::uint32_t* output_ids,
                                  std::string_view clock) noexcept {
        candidate.input_=request.input;
        candidate.input_.channel_order={input_ids,request.input.channel_order.size()};
        candidate.input_.clock.domain=clock;
        candidate.output_=description.output;
        candidate.output_.channel_order={output_ids,description.planes};
        candidate.output_.clock.domain=clock;
        candidate.planes_={layouts,description.planes};candidate.selection_={mapping,description.planes};
        candidate.limits_=request.limits;candidate.requirements_=description.requirements;
        candidate.coordinate_offsets_=description.coordinate_offsets;
        candidate.time_offset_=description.time;candidate.mask_offset_=description.mask;
        candidate.stream_offset_=description.stream;
    }
    static void build(const NumericGatherRequest& request,const Description& description,
                      PreparedNumericGather& candidate) noexcept {
        auto bytes=candidate.metadata_.bytes();
        auto* layouts=::new(static_cast<void*>(bytes.data()+description.layouts)) PlaneLayout[description.planes];
        auto* mapping=::new(static_cast<void*>(bytes.data()+description.mapping)) std::size_t[description.planes];
        auto* input_ids=::new(static_cast<void*>(bytes.data()+description.input_ids)) std::uint32_t[request.input.channel_order.size()];
        auto* output_ids=::new(static_cast<void*>(bytes.data()+description.output_ids)) std::uint32_t[description.planes];
        map_channels(request,description,layouts,mapping,input_ids,output_ids);
        if(!request.input.clock.domain.empty())
            std::memcpy(bytes.data()+description.clock,request.input.clock.domain.data(),request.input.clock.domain.size());
        const auto clock=request.input.clock.domain.empty()?std::string_view{}:
            std::string_view(reinterpret_cast<const char*>(bytes.data()+description.clock),request.input.clock.domain.size());
        publish_contracts(request,description,candidate,layouts,mapping,input_ids,output_ids,clock);
    }
};
Status numeric_gather_requirements(const NumericGatherRequest& request,NumericGatherRequirements& out) noexcept {
    if(overlap(object_bytes(out),object_bytes(request)) || contract_alias(object_bytes(out),request.input) ||
        overlap(object_bytes(out),std::as_bytes(request.selected_channels)))return Status::invalid_argument;
    Description d;const auto status=describe(request,d);if(status==Status::ok)out=d.requirements;return status;
}
Status prepare_numeric_gather(const NumericGatherRequest& r,BufferPool& pool,PreparedNumericGather& out) noexcept {
    Description d;auto status=describe(r,d);if(status!=Status::ok)return status;
    PreparedNumericGather candidate;status=pool.acquire(d.requirements.plan_metadata_bytes,candidate.metadata_);if(status!=Status::ok)return status;
    auto bytes=candidate.metadata_.bytes();
    if(!NumericGatherPlanBuilder::storage_disjoint(r,d,out,bytes))return Status::invalid_argument;
    NumericGatherPlanBuilder::build(r,d,candidate);
    out=std::move(candidate);return Status::ok;
}
struct NumericGatherRunner {
    const PreparedNumericGather& plan;
    const DataFrameView& frame;
    NumericGatherRow row;
    const NumericGatherDestination& target;
    std::span<std::byte> scratch;
    NumericGatherOutput& output;
    const NumericView* source=nullptr;
    const NumericGatherRequirements& requirements;
    const NumericContract& input_contract;
    const MutableNumericView& destination;
    std::span<std::byte> payload,metadata,stage_payload,stage_meta;
    std::array<std::size_t,2> actual{1,1};
    NumericGatherRowInfo info;
    CoordinateView coordinates;
    FrameStamp stamp;
    bool history=false;

    NumericGatherRunner(const PreparedNumericGather& prepared,const DataFrameView& donor,
                        NumericGatherRow selection,const NumericGatherDestination& writable,
                        std::span<std::byte> workspace,NumericGatherOutput& result) noexcept
        :plan(prepared),frame(donor),row(selection),target(writable),scratch(workspace),output(result),
         requirements(plan.requirements_),input_contract(plan.input_),destination(target.value){}
    Status validate_buffers() noexcept {
        if(!plan)return Status::invalid_argument;
        source=std::get_if<NumericView>(&frame.payload);
        if(!source || !source->contract)return Status::invalid_argument;
        if(!same_physical_contract(*source->contract,input_contract) || !destination.contract ||
           !same_physical_contract(*destination.contract,plan.output_))return Status::incompatible_contract;
        if(destination.storage.size()<requirements.payload_bytes || target.metadata.size()<requirements.output_metadata_bytes ||
           scratch.size()<requirements.scratch_bytes)return Status::buffer_too_small;
        if(destination.storage.size()>plan.limits_.frame.max_payload_bytes ||
           target.metadata.size()>plan.limits_.max_output_metadata_bytes ||
           scratch.size()>plan.limits_.max_scratch_bytes)return Status::exhausted;
        payload=destination.storage.first(requirements.payload_bytes);
        metadata=target.metadata.first(requirements.output_metadata_bytes);
        scratch=scratch.first(requirements.scratch_bytes);
        return Status::ok;
    }
    bool conflicts(std::span<const std::byte> donor) const noexcept {
        for(const auto written:{std::span<const std::byte>(payload),std::span<const std::byte>(metadata),
                                std::span<const std::byte>(scratch),object_bytes(output)})
            if(overlap(written,donor))return true;
        return false;
    }
    bool source_aliases() const noexcept {
        if(conflicts(source->storage) || conflicts(object_bytes(*source->contract)) ||
           conflicts(std::as_bytes(source->planes)) || conflicts(std::as_bytes(source->contract->channel_order)) ||
           conflicts(text_bytes(source->contract->clock.domain)) || conflicts(source->ring.silence_mask) ||
           conflicts(text_bytes(frame.stamp.stream_id)))return true;
        for(const auto axis:source->coordinates.axes)if(conflicts(axis))return true;
        return false;
    }
    bool destination_aliases() const noexcept {
        if(conflicts(object_bytes(*destination.contract)) || conflicts(std::as_bytes(destination.planes)) ||
           conflicts(std::as_bytes(destination.contract->channel_order)) ||
           conflicts(text_bytes(destination.contract->clock.domain)))return true;
        for(const auto axis:destination.coordinates.axes)if(conflicts(axis))return true;
        return false;
    }
    Status validate_aliases() const noexcept {
        if(conflicts(object_bytes(plan)) || conflicts(plan.metadata_.bytes()) ||
           conflicts(object_bytes(frame)) || conflicts(object_bytes(target)) ||
           source_aliases() || destination_aliases())return Status::invalid_argument;
        const std::array<std::span<const std::byte>,4> written{payload,metadata,scratch,object_bytes(output)};
        for(std::size_t index=0;index<written.size();++index)
            for(std::size_t prior=0;prior<index;++prior)
                if(overlap(written[index],written[prior]))return Status::invalid_argument;
        return Status::ok;
    }
    Status validate_plane_descriptors() const noexcept {
        if(source->planes.size()!=input_contract.channel_order.size() ||
           destination.planes.size()!=plan.planes_.size())return Status::invalid_argument;
        for(std::size_t index=0;index<destination.planes.size();++index)
            if(destination.planes[index].offset!=plan.planes_[index].offset ||
               destination.planes[index].stride_bytes!=plan.planes_[index].stride_bytes)
                return Status::invalid_argument;
        return Status::ok;
    }
    Status validate_plane_alignment() const noexcept {
        for(const auto& plane:source->planes)
            if(reinterpret_cast<std::uintptr_t>(source->storage.data()+plane.offset)%
               input_contract.representation.alignment)return Status::invalid_argument;
        return Status::ok;
    }

Status validate_source_buffer_bounds() const noexcept {
        std::size_t needed=0;
        const auto storage=required_storage(input_contract,source->planes,needed);
        if(storage!=Status::ok)return storage;
        if(needed>plan.limits_.frame.max_payload_bytes ||
           source->storage.size()>plan.limits_.frame.max_payload_bytes)return Status::exhausted;
        if(needed>source->storage.size())return Status::buffer_too_small;
        return Status::ok;
    }

    Status validate_source_storage() const noexcept {
        if(frame.stamp.stream_id.size()>plan.limits_.frame.max_string_bytes)return Status::exhausted;
        if(reinterpret_cast<std::uintptr_t>(payload.data())%requirements.payload_alignment ||
           reinterpret_cast<std::uintptr_t>(scratch.data())%requirements.scratch_alignment)return Status::invalid_argument;
        auto status=validate_plane_descriptors();if(status!=Status::ok)return status;
        status=validate_source_buffer_bounds();if(status!=Status::ok)return status;
        return validate_plane_alignment();
    }
Status validate_ring_shape() const noexcept {
        if(!source->ring.capacity)
            return source->ring.oldest || !source->ring.silence_mask.empty()?
                Status::invalid_argument:Status::ok;
        if((input_contract.shape.rank!=Rank::Matrix && (input_contract.shape.rank!=Rank::Vector || input_contract.semantic!=Semantic::History)) ||
           source->ring.capacity!=input_contract.shape.extents[0].maximum ||
           source->ring.oldest>=source->ring.capacity ||
           (!source->ring.silence_mask.empty() &&
            source->ring.silence_mask.size()!=source->ring.capacity))return Status::invalid_argument;
        for(const auto mask:source->ring.silence_mask)
            if(mask!=std::byte{0} && mask!=std::byte{1})return Status::malformed;
        return Status::ok;
    }

    Status validate_source_shape() const noexcept {
        const auto dimensions=rank(input_contract);
        for(std::size_t axis=0;axis<2;++axis)
            if(source->actual[axis]<input_contract.shape.extents[axis].minimum ||
               source->actual[axis]>input_contract.shape.extents[axis].maximum ||
               (axis>=dimensions && source->actual[axis]!=1))return Status::invalid_argument;
        return validate_ring_shape();
    }
Status select_history_mask() noexcept {
        info.mask_present=!source->ring.silence_mask.empty();
        if(!info.mask_present)return Status::ok;
        const auto mask=source->ring.silence_mask[row.logical];
        if(mask!=std::byte{0} && mask!=std::byte{1})return Status::malformed;
        info.silenced=info.silenced || mask!=std::byte{0};
        return Status::ok;
    }

void locate_history_row() noexcept {
        info.logical_row=row.logical;info.filled_rows=source->actual[0];
        info.capacity=source->ring.capacity?source->ring.capacity:
            std::size_t(input_contract.shape.extents[0].maximum);
        const auto tail=source->ring.capacity-source->ring.oldest;
        info.physical_row=source->ring.capacity?
            (row.logical>=tail?row.logical-tail:source->ring.oldest+row.logical):row.logical;
        info.time_axis=input_contract.coordinates[0];
    }

    Status select_history_row() noexcept {
        if(!history)return Status::ok;
        if(!source->actual[0])return Status::unavailable;
        if(row.latest)row.logical=source->actual[0]-1;
        if(row.logical>=source->actual[0])return Status::invalid_argument;
        locate_history_row();
        return select_history_mask();
    }
    Status validate_explicit_coordinate_axis(std::size_t axis,std::span<const std::byte> bytes) const noexcept {
        std::size_t size=0;
        if(!mul(source->actual[axis],8,size))return Status::overflow;
        if(bytes.size()!=size)return Status::invalid_argument;
        const auto selected=(!history || axis==1)?bytes:bytes.subspan(row.logical*8,8);
        return finite_coordinates(selected)?Status::ok:Status::malformed;
    }
Status validate_coordinate_axis(std::size_t axis,std::size_t& total) const noexcept {
        const auto bytes=source->coordinates.axes[axis];
        if(input_contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE) {
            const auto status=validate_explicit_coordinate_axis(axis,bytes);
            if(status!=Status::ok)return status;
        }else if(!bytes.empty())return Status::invalid_argument;
        return add(total,bytes.size())?Status::ok:Status::overflow;
    }

    Status validate_coordinates() const noexcept {
        std::size_t coordinate_bytes=0;
        for(std::size_t axis=0;axis<2;++axis) {
            const auto status=validate_coordinate_axis(axis,coordinate_bytes);
            if(status!=Status::ok)return status;
        }
        return coordinate_bytes>plan.limits_.frame.max_coordinate_bytes?Status::exhausted:Status::ok;
    }
    void copy_metadata(std::size_t offset,std::span<const std::byte> bytes) noexcept {
        if(!bytes.empty())std::memcpy(stage_meta.data()+offset,bytes.data(),bytes.size());
    }
Status stage_regular_history_time() noexcept {
        const auto& axis=input_contract.coordinates[0];
        if(!axis.origin.denominator || !axis.step.denominator)return Status::malformed;
        const double coordinate=double(axis.origin.numerator)/double(axis.origin.denominator)+
            double(row.logical)*double(axis.step.numerator)/double(axis.step.denominator);
        if(!std::isfinite(coordinate))return Status::overflow;
        const auto bits=std::bit_cast<std::uint64_t>(coordinate);
        for(unsigned byte=0;byte<8;++byte)
            stage_meta[plan.time_offset_+byte]=std::byte((bits>>(byte*8))&255);
        info.time_coordinate=stage_meta.subspan(plan.time_offset_,8);
        return Status::ok;
    }

    Status stage_history_time() noexcept {
        if(input_contract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE) {
            copy_metadata(plan.time_offset_,source->coordinates.axes[0].subspan(row.logical*8,8));
            info.time_coordinate=stage_meta.subspan(plan.time_offset_,8);
        }else if(input_contract.coordinates[0].mode==CoordinateMode::Regular) {
            const auto status=stage_regular_history_time();
            if(status!=Status::ok)return status;
        }
        stage_meta[plan.mask_offset_]=info.mask_present?
            source->ring.silence_mask[row.logical]:std::byte{0};
        info.selected_mask=stage_meta.subspan(plan.mask_offset_,1);
        return Status::ok;
    }
void stage_coordinate_metadata() noexcept {
        for(std::size_t axis=0;axis<2;++axis) {
            auto bytes=source->coordinates.axes[history?1:axis];
            if(history && axis==1)bytes={};
            copy_metadata(plan.coordinate_offsets_[axis],bytes);
            coordinates.axes[axis]=stage_meta.subspan(plan.coordinate_offsets_[axis],bytes.size());
        }
    }

void stage_stream_metadata() noexcept {
        copy_metadata(plan.stream_offset_,text_bytes(frame.stamp.stream_id));
        stamp=frame.stamp;
        stamp.stream_id=frame.stamp.stream_id.empty()?std::string_view{}:
            std::string_view(reinterpret_cast<const char*>(stage_meta.data()+plan.stream_offset_),
                             frame.stamp.stream_id.size());
    }

    Status stage_metadata() noexcept {
        stage_payload=scratch.first(requirements.payload_bytes);
        stage_meta=scratch.subspan(requirements.payload_bytes,requirements.output_metadata_bytes);
        std::fill(stage_payload.begin(),stage_payload.end(),std::byte{0});
        std::fill(stage_meta.begin(),stage_meta.end(),std::byte{0});
        stage_coordinate_metadata();
        if(history) {
            const auto status=stage_history_time();
            if(status!=Status::ok)return status;
        }
        stage_stream_metadata();
        return Status::ok;
    }
Status copy_element(MutableNumericView& stage,std::size_t plane,std::size_t row_index,
                        std::size_t column) const noexcept {
        const ElementIndex source_index{plan.selection_[plane],history?row.logical:row_index,
                                        history?row_index:column};
        std::uint64_t bits=0;
        const auto loaded=load_bits(*source,source_index,input_contract.dtype,bits);
        if(loaded!=Status::ok)return loaded;
        return store_bits(stage,{plane,row_index,column},input_contract.dtype,bits);
    }

    Status copy_values(MutableNumericView& stage) const noexcept {
        const auto output_rank=rank(plan.output_);
        const auto rows=output_rank?actual[0]:1,columns=output_rank==2?actual[1]:1;
        for(std::size_t plane=0;plane<plan.selection_.size();++plane)
            for(std::size_t row_index=0;row_index<rows;++row_index)
                for(std::size_t column=0;column<columns;++column) {
                    const auto status=copy_element(stage,plane,row_index,column);
                    if(status!=Status::ok)return status;
                }
        return Status::ok;
    }
    Status stage_values() noexcept {
        MutableNumericView stage{&plan.output_,stage_payload,plan.planes_,actual,{},coordinates};
        const auto status=validate_view({stamp,stage.view()},plan.limits_.frame);
        return status==Status::ok?copy_values(stage):status;
    }
void rebind_published_metadata() noexcept {
        for(std::size_t axis=0;axis<2;++axis)
            coordinates.axes[axis]=metadata.subspan(plan.coordinate_offsets_[axis],coordinates.axes[axis].size());
        stamp.stream_id=frame.stamp.stream_id.empty()?std::string_view{}:
            std::string_view(reinterpret_cast<const char*>(metadata.data()+plan.stream_offset_),
                             frame.stamp.stream_id.size());
        if(history) {
            if(!info.time_coordinate.empty())info.time_coordinate=metadata.subspan(plan.time_offset_,8);
            info.selected_mask=metadata.subspan(plan.mask_offset_,1);
        }
    }

    void publish() noexcept {
        std::memcpy(payload.data(),stage_payload.data(),stage_payload.size());
        if(!metadata.empty())std::memcpy(metadata.data(),stage_meta.data(),stage_meta.size());
        rebind_published_metadata();
        NumericGatherOutput result;
        result.frame={stamp,NumericView{&plan.output_,payload,plan.planes_,actual,{},coordinates,info.silenced}};
        result.row=info;result.payload_bytes=requirements.payload_bytes;
        result.metadata_bytes=requirements.output_metadata_bytes;
        output=result;
    }
Status validate_input() noexcept {
        auto status=validate_buffers();if(status!=Status::ok)return status;
        status=validate_aliases();if(status!=Status::ok)return status;
        if(!frame.stamp.available)return Status::unavailable;
        status=validate_source_storage();if(status!=Status::ok)return status;
        return validate_source_shape();
    }

Status select_input() noexcept {
        history=input_contract.shape.rank==Rank::Matrix && plan.output_.shape.rank==Rank::Vector;
        info.history=history;info.silenced=source->semantic_silence;
        const auto status=select_history_row();if(status!=Status::ok)return status;
        actual=history?std::array<std::size_t,2>{source->actual[1],1}:source->actual;
        if(destination.actual!=actual || destination.ring.capacity || destination.ring.oldest ||
           !destination.ring.silence_mask.empty())return Status::invalid_argument;
        return validate_coordinates();
    }

    Status run() noexcept {
        auto status=validate_input();if(status!=Status::ok)return status;
        status=select_input();if(status!=Status::ok)return status;
        status=stage_metadata();if(status!=Status::ok)return status;
        status=stage_values();if(status!=Status::ok)return status;
        publish();return Status::ok;
    }
};
Status gather_numeric_frame(const PreparedNumericGather& plan,const DataFrameView& frame,NumericGatherRow row,
    const NumericGatherDestination& target,std::span<std::byte> scratch,NumericGatherOutput& output) noexcept {
    return NumericGatherRunner(plan,frame,row,target,scratch,output).run();
}
}
