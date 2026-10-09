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
Status describe(const NumericGatherRequest& r,Description& d){
    auto status=validate_contract(r.input,r.limits.frame);if(status!=Status::ok)return status;
    d.output=r.input;
    if(r.matrix_row){
        if(r.input.shape.rank!=Rank::Matrix)return Status::incompatible_contract;
        d.output.semantic=r.input.history_element;d.output.history_element=Semantic::Numeric;
        d.output.shape={Rank::Vector,{r.input.shape.axes[1],Axis::None},{r.input.shape.extents[1],ExtentBounds{}}};
        d.output.coordinates={r.input.coordinates[1],AxisCoordinates{}};
    }
    if(!r.selected_channels.empty())d.output.channel_order=r.selected_channels;
    d.planes=d.output.channel_order.size();
    if(d.planes>r.limits.frame.max_planes)return Status::exhausted;
    for(std::size_t i=0;i<d.planes;++i){auto id=d.output.channel_order[i];
        if(std::find(r.input.channel_order.begin(),r.input.channel_order.end(),id)==r.input.channel_order.end())return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(id==d.output.channel_order[j])return Status::invalid_argument;
    }
    status=validate_contract(d.output,r.limits.frame);if(status!=Status::ok)return status;
    status=storage_width(d.output,d.width);if(status!=Status::ok)return status;
    const auto dimensions=rank(d.output);auto stride=d.width;
    if(d.output.representation.layout==ChannelLayout::Interleaved){
        if(d.planes>1 && d.width%d.output.representation.alignment)return Status::incompatible_contract;
        if(!mul(d.planes,d.width,stride))return Status::overflow;
    }
    if(!align(stride,d.output.representation.alignment))return Status::overflow;
    d.plane_block=d.width;
    if(dimensions==1){d.first_stride=stride;if(!mul(std::size_t(d.output.shape.extents[0].maximum),stride,d.plane_block))return Status::overflow;}
    if(dimensions==2){d.second_stride=stride;
        if(!mul(std::size_t(d.output.shape.extents[1].maximum),stride,d.first_stride) ||
            !mul(std::size_t(d.output.shape.extents[0].maximum),d.first_stride,d.plane_block))return Status::overflow;
    }
    auto& q=d.requirements;q.payload_alignment=d.output.representation.alignment;q.scratch_alignment=q.payload_alignment;
    auto last=compact_plane(d,d.planes-1);q.payload_bytes=last.offset;
    std::size_t term=0;
    for(std::size_t i=0;i<dimensions;++i)if(!mul(std::size_t(d.output.shape.extents[i].maximum)-1,last.stride_bytes[i],term) || !add(q.payload_bytes,term))return Status::overflow;
    if(!add(q.payload_bytes,d.width))return Status::overflow;
    q.plan_metadata_alignment=std::max({alignof(PlaneLayout),alignof(std::size_t),alignof(std::uint32_t)});
    auto array=[&](std::size_t count,std::size_t width,std::size_t alignment,std::size_t& offset){
        if(!align(q.plan_metadata_bytes,alignment))return false;
        offset=q.plan_metadata_bytes;
        return mul(count,width,term) && add(q.plan_metadata_bytes,term);
    };
    if(!array(d.planes,sizeof(PlaneLayout),alignof(PlaneLayout),d.layouts) || !array(d.planes,sizeof(std::size_t),alignof(std::size_t),d.mapping) ||
        !array(r.input.channel_order.size(),sizeof(std::uint32_t),alignof(std::uint32_t),d.input_ids) || !array(d.planes,sizeof(std::uint32_t),alignof(std::uint32_t),d.output_ids))return Status::overflow;
    d.clock=q.plan_metadata_bytes;if(!add(q.plan_metadata_bytes,r.input.clock.domain.size()))return Status::overflow;
    for(std::size_t i=0;i<2;++i){d.coordinate_offsets[i]=q.output_metadata_bytes;
        if(d.output.coordinates[i].mode==CoordinateMode::ExplicitFloat64LE && (!mul(std::size_t(d.output.shape.extents[i].maximum),8,term) || !add(q.output_metadata_bytes,term)))return Status::overflow;
    }
    d.time=q.output_metadata_bytes;
    if(r.matrix_row && r.input.coordinates[0].mode!=CoordinateMode::None && !add(q.output_metadata_bytes,8))return Status::overflow;
    d.mask=q.output_metadata_bytes;if(r.matrix_row && !add(q.output_metadata_bytes,1))return Status::overflow;
    d.stream=q.output_metadata_bytes;if(!add(q.output_metadata_bytes,r.limits.frame.max_string_bytes))return Status::overflow;
    q.scratch_bytes=q.payload_bytes;if(!add(q.scratch_bytes,q.output_metadata_bytes))return Status::overflow;
    if(q.payload_bytes>r.limits.max_output_payload_bytes || q.payload_bytes>r.limits.frame.max_payload_bytes ||
        q.output_metadata_bytes>r.limits.max_output_metadata_bytes || q.scratch_bytes>r.limits.max_scratch_bytes ||
        q.plan_metadata_bytes>r.limits.frame.max_metadata_bytes)return Status::exhausted;
    return Status::ok;
}
bool finite_coordinates(std::span<const std::byte> bytes){
    for(std::size_t i=0;i<bytes.size();i+=8){std::uint64_t bits=0;
        for(unsigned j=0;j<8;++j)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[i+j]))<<(8*j);
        if((bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000))return false;
    }return true;
}
}
Status numeric_gather_requirements(const NumericGatherRequest& request,NumericGatherRequirements& out) noexcept {
    if(overlap(object_bytes(out),object_bytes(request)) || contract_alias(object_bytes(out),request.input) ||
        overlap(object_bytes(out),std::as_bytes(request.selected_channels)))return Status::invalid_argument;
    Description d;const auto status=describe(request,d);if(status==Status::ok)out=d.requirements;return status;
}
Status prepare_numeric_gather(const NumericGatherRequest& r,BufferPool& pool,PreparedNumericGather& out) noexcept {
    Description d;auto status=describe(r,d);if(status!=Status::ok)return status;
    PreparedNumericGather candidate;status=pool.acquire(d.requirements.plan_metadata_bytes,candidate.metadata_);if(status!=Status::ok)return status;
    auto bytes=candidate.metadata_.bytes();
    if(reinterpret_cast<std::uintptr_t>(bytes.data())%d.requirements.plan_metadata_alignment || overlap(bytes,object_bytes(r)) ||
        contract_alias(bytes,r.input) || overlap(bytes,std::as_bytes(r.selected_channels)) || overlap(bytes,object_bytes(out)) ||
        overlap(bytes,std::as_const(out.metadata_).bytes()))return Status::invalid_argument;
    auto* layouts=::new(static_cast<void*>(bytes.data()+d.layouts)) PlaneLayout[d.planes];
    auto* mapping=::new(static_cast<void*>(bytes.data()+d.mapping)) std::size_t[d.planes];
    auto* input_ids=::new(static_cast<void*>(bytes.data()+d.input_ids)) std::uint32_t[r.input.channel_order.size()];
    auto* output_ids=::new(static_cast<void*>(bytes.data()+d.output_ids)) std::uint32_t[d.planes];
    std::copy(r.input.channel_order.begin(),r.input.channel_order.end(),input_ids);
    for(std::size_t i=0;i<d.planes;++i){layouts[i]=compact_plane(d,i);output_ids[i]=d.output.channel_order[i];
        mapping[i]=std::size_t(std::find(r.input.channel_order.begin(),r.input.channel_order.end(),output_ids[i])-r.input.channel_order.begin());}
    if(!r.input.clock.domain.empty())std::memcpy(bytes.data()+d.clock,r.input.clock.domain.data(),r.input.clock.domain.size());
    auto clock=r.input.clock.domain.empty()?std::string_view{}:std::string_view(reinterpret_cast<const char*>(bytes.data()+d.clock),r.input.clock.domain.size());
    candidate.input_=r.input;candidate.input_.channel_order={input_ids,r.input.channel_order.size()};candidate.input_.clock.domain=clock;
    candidate.output_=d.output;candidate.output_.channel_order={output_ids,d.planes};candidate.output_.clock.domain=clock;
    candidate.planes_={layouts,d.planes};candidate.selection_={mapping,d.planes};candidate.limits_=r.limits;candidate.requirements_=d.requirements;
    candidate.coordinate_offsets_=d.coordinate_offsets;candidate.time_offset_=d.time;candidate.mask_offset_=d.mask;candidate.stream_offset_=d.stream;
    out=std::move(candidate);return Status::ok;
}
Status gather_numeric_frame(const PreparedNumericGather& plan,const DataFrameView& f,NumericGatherRow row,
    const NumericGatherDestination& target,std::span<std::byte> scratch,NumericGatherOutput& out) noexcept {
    if(!plan)return Status::invalid_argument;
    const auto* source=std::get_if<NumericView>(&f.payload);if(!source || !source->contract)return Status::invalid_argument;
    const auto& q=plan.requirements_;const auto& c=plan.input_;const auto& d=target.value;
    if(!same_physical_contract(*source->contract,c) || !d.contract || !same_physical_contract(*d.contract,plan.output_))return Status::incompatible_contract;
    if(d.storage.size()<q.payload_bytes || target.metadata.size()<q.output_metadata_bytes || scratch.size()<q.scratch_bytes)return Status::buffer_too_small;
    if(d.storage.size()>plan.limits_.frame.max_payload_bytes || target.metadata.size()>plan.limits_.max_output_metadata_bytes || scratch.size()>plan.limits_.max_scratch_bytes)return Status::exhausted;
    auto payload=d.storage.first(q.payload_bytes),metadata=target.metadata.first(q.output_metadata_bytes);scratch=scratch.first(q.scratch_bytes);
    const std::array<std::span<const std::byte>,4> written{payload,metadata,scratch,object_bytes(out)};
    auto conflict=[&](std::span<const std::byte> donor){for(auto region:written)if(overlap(region,donor))return true;return false;};
    if(conflict(object_bytes(plan)) || conflict(plan.metadata_.bytes()) || conflict(object_bytes(f)) || conflict(object_bytes(target)) ||
        conflict(source->storage) || conflict(object_bytes(*source->contract)) || conflict(std::as_bytes(source->planes)) ||
        conflict(std::as_bytes(source->contract->channel_order)) || conflict(text_bytes(source->contract->clock.domain)) ||
        conflict(source->ring.silence_mask) || conflict(text_bytes(f.stamp.stream_id)) || conflict(object_bytes(*d.contract)) ||
        conflict(std::as_bytes(d.planes)) || conflict(std::as_bytes(d.contract->channel_order)) || conflict(text_bytes(d.contract->clock.domain)))return Status::invalid_argument;
    for(auto axis:source->coordinates.axes)if(conflict(axis))return Status::invalid_argument;
    for(auto axis:d.coordinates.axes)if(conflict(axis))return Status::invalid_argument;
    for(std::size_t i=0;i<written.size();++i)for(std::size_t j=0;j<i;++j)if(overlap(written[i],written[j]))return Status::invalid_argument;
    if(!f.stamp.available)return Status::unavailable;
    if(f.stamp.stream_id.size()>plan.limits_.frame.max_string_bytes)return Status::exhausted;
    if(reinterpret_cast<std::uintptr_t>(payload.data())%q.payload_alignment || reinterpret_cast<std::uintptr_t>(scratch.data())%q.scratch_alignment)return Status::invalid_argument;
    if(source->planes.size()!=c.channel_order.size() || d.planes.size()!=plan.planes_.size())return Status::invalid_argument;
    for(std::size_t i=0;i<d.planes.size();++i)if(d.planes[i].offset!=plan.planes_[i].offset || d.planes[i].stride_bytes!=plan.planes_[i].stride_bytes)return Status::invalid_argument;
    std::size_t needed=0;auto status=required_storage(c,source->planes,needed);if(status!=Status::ok)return status;
    if(needed>plan.limits_.frame.max_payload_bytes || source->storage.size()>plan.limits_.frame.max_payload_bytes)return Status::exhausted;
    if(needed>source->storage.size())return Status::buffer_too_small;
    for(const auto& p:source->planes)if(reinterpret_cast<std::uintptr_t>(source->storage.data()+p.offset)%c.representation.alignment)return Status::invalid_argument;
    const auto dimensions=rank(c);
    for(std::size_t i=0;i<2;++i)if(source->actual[i]<c.shape.extents[i].minimum || source->actual[i]>c.shape.extents[i].maximum || (i>=dimensions && source->actual[i]!=1))return Status::invalid_argument;
    const bool history=c.shape.rank==Rank::Matrix && plan.output_.shape.rank==Rank::Vector;NumericGatherRowInfo info;info.history=history;info.silenced=source->semantic_silence;
    if(source->ring.capacity){
        if(c.shape.rank!=Rank::Matrix || source->ring.capacity!=c.shape.extents[0].maximum || source->ring.oldest>=source->ring.capacity ||
           (!source->ring.silence_mask.empty() && source->ring.silence_mask.size()!=source->ring.capacity))return Status::invalid_argument;
        for(auto mask:source->ring.silence_mask)if(mask!=std::byte{0} && mask!=std::byte{1})return Status::malformed;
    }else if(source->ring.oldest || !source->ring.silence_mask.empty())return Status::invalid_argument;
    if(history){
        if(!source->actual[0])return Status::unavailable;
        if(row.latest)row.logical=source->actual[0]-1;
        if(row.logical>=source->actual[0])return Status::invalid_argument;
        info.logical_row=row.logical;info.filled_rows=source->actual[0];info.capacity=source->ring.capacity?source->ring.capacity:std::size_t(c.shape.extents[0].maximum);
        const auto tail=source->ring.capacity-source->ring.oldest;info.physical_row=source->ring.capacity?(row.logical>=tail?row.logical-tail:source->ring.oldest+row.logical):row.logical;
        info.mask_present=!source->ring.silence_mask.empty();info.time_axis=c.coordinates[0];
        if(info.mask_present){auto mask=source->ring.silence_mask[row.logical];if(mask!=std::byte{0} && mask!=std::byte{1})return Status::malformed;info.silenced=info.silenced || mask!=std::byte{0};}
    }else if(source->ring.capacity && c.shape.rank!=Rank::Matrix)return Status::invalid_argument;
    const auto actual=history?std::array<std::size_t,2>{source->actual[1],1}:source->actual;
    if(d.actual!=actual || d.ring.capacity || d.ring.oldest || !d.ring.silence_mask.empty())return Status::invalid_argument;
    std::size_t coordinate_bytes=0;
    for(std::size_t i=0;i<2;++i){const auto axis=source->coordinates.axes[i];
        if(c.coordinates[i].mode==CoordinateMode::ExplicitFloat64LE){std::size_t size=0;
            if(!mul(source->actual[i],8,size))return Status::overflow;
            if(axis.size()!=size)return Status::invalid_argument;
            if(!history || i==1){if(!finite_coordinates(axis))return Status::malformed;}
            else if(!finite_coordinates(axis.subspan(row.logical*8,8)))return Status::malformed;
        }else if(!axis.empty())return Status::invalid_argument;
        if(!add(coordinate_bytes,axis.size()))return Status::overflow;
    }
    if(coordinate_bytes>plan.limits_.frame.max_coordinate_bytes)return Status::exhausted;
    auto stage_payload=scratch.first(q.payload_bytes),stage_meta=scratch.subspan(q.payload_bytes,q.output_metadata_bytes);
    std::fill(stage_payload.begin(),stage_payload.end(),std::byte{0});std::fill(stage_meta.begin(),stage_meta.end(),std::byte{0});
    auto copy=[&](std::size_t offset,std::span<const std::byte> bytes){if(!bytes.empty())std::memcpy(stage_meta.data()+offset,bytes.data(),bytes.size());};
    CoordinateView coordinates;
    for(std::size_t i=0;i<2;++i){auto bytes=source->coordinates.axes[history?1:i];if(history && i==1)bytes={};
        copy(plan.coordinate_offsets_[i],bytes);coordinates.axes[i]=stage_meta.subspan(plan.coordinate_offsets_[i],bytes.size());}
    if(history){
        if(c.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE){copy(plan.time_offset_,source->coordinates.axes[0].subspan(row.logical*8,8));info.time_coordinate=stage_meta.subspan(plan.time_offset_,8);}
        else if(c.coordinates[0].mode==CoordinateMode::Regular){
            // Regular coordinates are relative to the logical oldest row of
            // this snapshot, exactly as on the full-frame projection boundary.
            const auto& axis=c.coordinates[0];
            if(!axis.origin.denominator || !axis.step.denominator)return Status::malformed;
            const double coordinate=double(axis.origin.numerator)/double(axis.origin.denominator)+
                double(row.logical)*double(axis.step.numerator)/double(axis.step.denominator);
            if(!std::isfinite(coordinate))return Status::overflow;
            const auto bits=std::bit_cast<std::uint64_t>(coordinate);
            for(unsigned byte=0;byte<8;++byte)stage_meta[plan.time_offset_+byte]=std::byte((bits>>(byte*8))&255);
            info.time_coordinate=stage_meta.subspan(plan.time_offset_,8);
        }
        stage_meta[plan.mask_offset_]=info.mask_present?source->ring.silence_mask[row.logical]:std::byte{0};info.selected_mask=stage_meta.subspan(plan.mask_offset_,1);
    }
    copy(plan.stream_offset_,text_bytes(f.stamp.stream_id));
    MutableNumericView stage{&plan.output_,stage_payload,plan.planes_,actual,{},coordinates};
    auto stamp=f.stamp;stamp.stream_id=f.stamp.stream_id.empty()?std::string_view{}:std::string_view(reinterpret_cast<const char*>(stage_meta.data()+plan.stream_offset_),f.stamp.stream_id.size());
    status=validate_view({stamp,stage.view()},plan.limits_.frame);if(status!=Status::ok)return status;
    const auto output_rank=rank(plan.output_);const auto rows=output_rank?actual[0]:1,columns=output_rank==2?actual[1]:1;
    for(std::size_t p=0;p<plan.selection_.size();++p)for(std::size_t r=0;r<rows;++r)for(std::size_t col=0;col<columns;++col){
        ElementIndex index{plan.selection_[p],history?row.logical:r,history?r:col};std::uint64_t bits=0;
        status=load_bits(*source,index,c.dtype,bits);if(status!=Status::ok)return status;
        status=store_bits(stage,{p,r,col},c.dtype,bits);if(status!=Status::ok)return status;
    }
    std::memcpy(payload.data(),stage_payload.data(),stage_payload.size());
    if(!metadata.empty())std::memcpy(metadata.data(),stage_meta.data(),stage_meta.size());
    for(std::size_t i=0;i<2;++i)coordinates.axes[i]=metadata.subspan(plan.coordinate_offsets_[i],coordinates.axes[i].size());
    stamp.stream_id=f.stamp.stream_id.empty()?std::string_view{}:std::string_view(reinterpret_cast<const char*>(metadata.data()+plan.stream_offset_),f.stamp.stream_id.size());
    if(history){if(!info.time_coordinate.empty())info.time_coordinate=metadata.subspan(plan.time_offset_,8);info.selected_mask=metadata.subspan(plan.mask_offset_,1);}
    NumericGatherOutput result;result.frame={stamp,NumericView{&plan.output_,payload,plan.planes_,actual,{},coordinates,info.silenced}};
    result.row=info;result.payload_bytes=q.payload_bytes;result.metadata_bytes=q.output_metadata_bytes;out=result;return Status::ok;
}
}
