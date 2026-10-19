#include <signal_contracts/data_frame.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
namespace signal_contracts {
    namespace {
        constexpr auto maximum=std::numeric_limits<std::size_t>::max();
        bool add(std::size_t& a,std::size_t b){if(b>maximum-a)return false;
            a+=b;
            return true;
        }
        bool product(std::size_t a,std::size_t b,std::size_t& out){if(b && a>maximum/b)return false;
            out=a*b;
            return true;
        }
        bool align(std::size_t& n,std::size_t a){if(!a || (a&(a-1)))return false;
            const auto padding=(a-n%a)%a;
            return add(n,padding);
        }
        std::size_t rank(const NumericContract& c){return c.shape.rank==Rank::Scalar?0:c.shape.rank==Rank::Vector?1:2;
        }
        std::uint64_t mask(unsigned bits){return bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
        }
        bool big(StorageEncoding e){return e==StorageEncoding::FixedBigEndian || e==StorageEncoding::PackedSigned24BE || (e==StorageEncoding::Native && std::endian::native==std::endian::big);
        }
        std::uint64_t read(std::span<const std::byte> bytes,bool be){std::uint64_t x=0;
            for(std::size_t i=0;i<bytes.size();++i)x|=std::uint64_t(std::to_integer<unsigned char>(bytes[i]))<<(8*(be?bytes.size()-1-i:i));
            return x;
        }
        void write(std::span<std::byte> bytes,std::uint64_t x,bool be){for(std::size_t i=0;i<bytes.size();++i)bytes[i]=std::byte((x>>(8*(be?bytes.size()-1-i:i)))&255);
        }
        std::uint64_t silence(const NumericContract& c){const auto* d=dtype_descriptor(c.dtype);
            if(c.representation.normalization==Normalization::UnsignedUnit)if(d && !d->signed_value && !d->ieee)return std::uint64_t{1}<<((c.representation.valid_bits?c.representation.valid_bits:d->bytes*8)-1);
            return 0;
        }
        Status validate_actual(const NumericView& view,std::size_t dimensions) {
            for(std::size_t axis=0;axis<2;++axis)
                if(view.actual[axis]<view.contract->shape.extents[axis].minimum || view.actual[axis]>view.contract->shape.extents[axis].maximum || (axis>=dimensions && view.actual[axis]!=1))return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_element_geometry(const NumericView& v,ElementIndex i,std::size_t r) {
            if(v.contract->shape.rank!=Rank::Scalar && v.contract->shape.rank!=Rank::Vector && v.contract->shape.rank!=Rank::Matrix)return Status::invalid_argument;
            if(!r && v.planes.size()!=1)return Status::invalid_argument;
            const auto actual=validate_actual(v,r);if(actual!=Status::ok)return actual;
            if((r==0 && (i.row || i.column)) || (r==1 && (i.row>=v.actual[0] || i.column)) || (r==2 && (i.row>=v.actual[0] || i.column>=v.actual[1])))return Status::invalid_argument;
            return Status::ok;
        }
        Status physical_row(const NumericView& v,std::size_t logical,std::size_t r,std::size_t& row,bool& silent) {
            row=logical;
            silent=v.semantic_silence;
            if(v.ring.capacity){if((r!=1 && r!=2) || (r==1 && v.contract->semantic!=Semantic::History) || !v.ring.capacity || v.actual[0]>v.ring.capacity || v.ring.oldest>=v.ring.capacity || (!v.ring.silence_mask.empty() && v.ring.silence_mask.size()<v.actual[0]))return Status::invalid_argument;
                if(!v.ring.silence_mask.empty()){
                    if(v.ring.silence_mask[logical]!=std::byte{0} && v.ring.silence_mask[logical]!=std::byte{1})return Status::malformed;
                    silent=silent || v.ring.silence_mask[logical]!=std::byte{0};
                }
                row=v.ring.oldest;
                const auto tail=v.ring.capacity-row;
                row=logical>=tail?logical-tail:row+logical;
            }
            return Status::ok;
        }
        Status element_byte_offset(const NumericView& v,ElementIndex i,std::size_t r,std::size_t row,std::size_t width,std::size_t& out) {
            out=v.planes[i.plane].offset;
            std::size_t term=0;
            if(r && (!product(row,v.planes[i.plane].stride_bytes[0],term) || !add(out,term)))return Status::overflow;
            if(r==2 && (!product(i.column,v.planes[i.plane].stride_bytes[1],term) || !add(out,term)))return Status::overflow;
            if(out>v.storage.size() || width>v.storage.size()-out)return Status::buffer_too_small;
            if(!v.contract->representation.alignment || reinterpret_cast<std::uintptr_t>(v.storage.data()+out)%v.contract->representation.alignment)return Status::invalid_argument;
            return Status::ok;
        }
        Status offset(const NumericView& v,ElementIndex i,std::size_t& out,std::size_t& width,bool& silent) {
            if(!v.contract || i.plane>=v.planes.size() || v.planes.size()!=v.contract->channel_order.size())return Status::invalid_argument;
            auto status=storage_width(*v.contract,width);if(status!=Status::ok)return status;
            const auto dimensions=rank(*v.contract);
            status=validate_element_geometry(v,i,dimensions);if(status!=Status::ok)return status;
            std::size_t row=0;status=physical_row(v,i.row,dimensions,row,silent);if(status!=Status::ok)return status;
            return element_byte_offset(v,i,dimensions,row,width,out);
        }
        struct LayoutGeometry {
            const NumericContract& contract;std::span<const PlaneLayout> planes;
            std::size_t width=0,dimensions=0,lane=0;std::array<std::size_t,2> extents{1,1};bool interleaved=false;
            Status describe() {
                if(planes.empty() || planes.size()!=contract.channel_order.size() || !contract.representation.alignment || (contract.representation.alignment&(contract.representation.alignment-1)))return Status::invalid_argument;
                if(contract.shape.rank!=Rank::Scalar && contract.shape.rank!=Rank::Vector && contract.shape.rank!=Rank::Matrix)return Status::invalid_argument;
                dimensions=rank(contract);interleaved=contract.representation.layout==ChannelLayout::Interleaved;
                for(std::size_t axis=0;axis<dimensions;++axis) {
                    if(!contract.shape.extents[axis].maximum || contract.shape.extents[axis].maximum>maximum)return Status::invalid_argument;
                    extents[axis]=static_cast<std::size_t>(contract.shape.extents[axis].maximum);
                }
                return product(planes.size(),width,lane)?Status::ok:Status::overflow;
            }
            Status strides(const PlaneLayout& plane) const {
                if(!dimensions)return Status::ok;
                const auto minimum_stride=interleaved?lane:width;
                if(dimensions==2) {
                    if(plane.stride_bytes[1]<minimum_stride)return Status::invalid_argument;
                    std::size_t row=0;if(!product(extents[1],plane.stride_bytes[1],row))return Status::overflow;
                    if(plane.stride_bytes[0]<row)return Status::invalid_argument;
                }else if(plane.stride_bytes[0]<minimum_stride)return Status::invalid_argument;
                return Status::ok;
            }
            Status accumulate_extents(const PlaneLayout& plane,std::size_t& end) const {
                std::size_t term=0;
                for(std::size_t axis=0;axis<dimensions;++axis) {
                    if(plane.stride_bytes[axis]%contract.representation.alignment)return Status::invalid_argument;
                    if(!product(extents[axis]-1,plane.stride_bytes[axis],term) || !add(end,term))return Status::overflow;
                }
                return Status::ok;
            }
            Status plane_end(const PlaneLayout& plane,std::size_t& end) const {
                end=plane.offset;auto status=strides(plane);if(status!=Status::ok)return status;
                status=accumulate_extents(plane,end);if(status!=Status::ok)return status;
                return add(end,width)?Status::ok:Status::overflow;
            }
            Status interleaved_plane(std::size_t index) const {
                std::size_t expected=planes[0].offset,term=0;
                if(!product(index,width,term) || !add(expected,term))return Status::overflow;
                return planes[index].offset==expected && planes[index].stride_bytes==planes[0].stride_bytes?Status::ok:Status::invalid_argument;
            }
            Status planar_plane(std::size_t index,std::size_t end) const {
                for(std::size_t previous=0;previous<index;++previous) {
                    std::size_t other=0;const auto status=plane_end(planes[previous],other);if(status!=Status::ok)return status;
                    if(planes[index].offset<other && planes[previous].offset<end)return Status::invalid_argument;
                }
                return Status::ok;
            }
            Status measure(std::size_t& out) const {
                std::size_t maximum_end=0;
                for(std::size_t index=0;index<planes.size();++index) {
                    std::size_t end=0;auto status=plane_end(planes[index],end);if(status!=Status::ok)return status;
                    status=interleaved?interleaved_plane(index):planar_plane(index,end);if(status!=Status::ok)return status;
                    maximum_end=std::max(maximum_end,end);
                }
                out=maximum_end;return Status::ok;
            }
        };
        Status layout(const NumericContract& contract,std::span<const PlaneLayout> planes,std::size_t& out) {
            std::size_t width=0;auto status=storage_width(contract,width);if(status!=Status::ok)return status;
            LayoutGeometry geometry{contract,planes,width};
            status=geometry.describe();if(status!=Status::ok)return status;
            return geometry.measure(out);
        }
        Status validate_ring(const NumericView& v,std::size_t r) {
            if(v.ring.capacity){if((r!=1 && r!=2) || (r==1 && v.contract->semantic!=Semantic::History) || v.ring.capacity!=v.contract->shape.extents[0].maximum || !v.ring.capacity || v.ring.oldest>=v.ring.capacity || (!v.ring.silence_mask.empty() && v.ring.silence_mask.size()!=v.ring.capacity))return Status::invalid_argument;
                for(auto m:v.ring.silence_mask)if(m!=std::byte{0} && m!=std::byte{1})return Status::invalid_argument;
            }
            else if(v.ring.capacity || v.ring.oldest || !v.ring.silence_mask.empty())return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_numeric_payload(const NumericView& v,const FrameLimits& l) {
            std::size_t needed=0;
            const auto s=layout(*v.contract,v.planes,needed);
            if(s!=Status::ok)return s;
            if(needed>l.max_payload_bytes || v.storage.size()>l.max_payload_bytes)return Status::exhausted;
            if(needed>v.storage.size())return Status::buffer_too_small;
            for(const auto& p:v.planes)if(reinterpret_cast<std::uintptr_t>(v.storage.data()+p.offset)%v.contract->representation.alignment)return Status::invalid_argument;
            return Status::ok;
        }
        bool finite_coordinate_values(std::span<const std::byte> bytes) {
            for(std::size_t offset=0;offset<bytes.size();offset+=8) {
                const auto bits=read(bytes.subspan(offset,8),false);double value;
                std::memcpy(&value,&bits,8);if(!std::isfinite(value))return false;
            }
            return true;
        }
        Status validate_coordinate_axis(const NumericView& view,std::size_t axis) {
            const auto bytes=view.coordinates.axes[axis];
            if(view.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return bytes.empty()?Status::ok:Status::invalid_argument;
            std::size_t size=0;if(!product(view.actual[axis],8,size))return Status::overflow;
            if(bytes.size()!=size)return Status::invalid_argument;
            return finite_coordinate_values(bytes)?Status::ok:Status::invalid_argument;
        }
        Status validate_numeric_coordinates(const NumericView& v,const FrameLimits& l) {
            std::size_t coordinates=0;
            for(std::size_t axis=0;axis<2;++axis) {
                const auto status=validate_coordinate_axis(v,axis);if(status!=Status::ok)return status;
                if(!add(coordinates,v.coordinates.axes[axis].size()))return Status::overflow;
            }
            if(coordinates>l.max_coordinate_bytes)return Status::exhausted;
            return Status::ok;
        }
        Status validate_numeric_shape(const NumericView& view) {
            const auto dimensions=rank(*view.contract);
            const auto actual=validate_actual(view,dimensions);
            return actual==Status::ok?validate_ring(view,dimensions):actual;
        }
        Status numeric_validation(const NumericView& view,const FrameLimits& limits) {
            if(!view.contract)return Status::invalid_argument;
            auto status=validate_contract(DataContract{*view.contract},limits);if(status!=Status::ok)return status;
            status=validate_numeric_shape(view);if(status!=Status::ok)return status;
            status=validate_numeric_payload(view,limits);if(status!=Status::ok)return status;
            return validate_numeric_coordinates(view,limits);
        }
        struct MetaPlan {std::size_t size=0,planes=0,channels=0,schema=0,encoding=0,clock=0,stream=0,mask=0;
            std::array<std::size_t,2> coordinates{};
            bool append(std::size_t bytes,std::size_t& offset) {offset=size;return add(size,bytes);}
            bool array(std::size_t count,std::size_t width,std::size_t alignment,std::size_t& offset) {
                if(!align(size,alignment))return false;
                offset=size;std::size_t bytes=0;return product(count,width,bytes) && add(size,bytes);
            }
        };
        Status numeric_metadata_plan(const NumericView& view,MetaPlan& plan) {
            if(!plan.array(view.planes.size(),sizeof(PlaneLayout),alignof(PlaneLayout),plan.planes) || !plan.array(view.contract->channel_order.size(),sizeof(std::uint32_t),alignof(std::uint32_t),plan.channels))return Status::overflow;
            if(!plan.append(view.contract->clock.domain.size(),plan.clock) || !plan.append(view.ring.silence_mask.size(),plan.mask))return Status::overflow;
            for(std::size_t axis=0;axis<2;++axis)if(!plan.append(view.coordinates.axes[axis].size(),plan.coordinates[axis]))return Status::overflow;
            return Status::ok;
        }
        Status metadata_plan(const DataFrameView& f,const FrameLimits& l,MetaPlan& p){
            if(const auto* n=std::get_if<NumericView>(&f.payload)){
                const auto status=numeric_metadata_plan(*n,p);if(status!=Status::ok)return status;
            } else {const auto& c=*std::get<StructuredView>(f.payload).contract;
                if(!p.append(c.schema.size(),p.schema) || !p.append(c.encoding.size(),p.encoding))return Status::overflow;
            }
            if(!p.append(f.stamp.stream_id.size(),p.stream))return Status::overflow;
            if(p.size>l.max_metadata_bytes)return Status::exhausted;
            return Status::ok;
        }
        bool overlaps(std::span<const std::byte> a,std::span<const std::byte> b){if(a.empty() || b.empty())return false;
            auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
            return x<=y?y-x<a.size():x-y<b.size();
        }
        Status decode_padding(const NumericContract& contract,const DTypeDescriptor& dtype,unsigned bits,std::size_t width,std::uint64_t& raw) {
            const auto shift=unsigned(width*8)-bits;
            if(contract.representation.placement==BitPlacement::MostSignificant) {
                if(raw&mask(shift))return Status::malformed;
                raw>>=shift;return Status::ok;
            }
            const auto high=raw&~mask(bits);
            const auto expected=dtype.signed_value && (raw&(std::uint64_t{1}<<(bits-1)))?mask(unsigned(width*8))&~mask(bits):0;
            if(high!=expected)return Status::malformed;
            raw&=mask(bits);return Status::ok;
        }
        Status decode_integer_bits(const NumericContract& contract,std::size_t width,std::uint64_t& raw) {
            const auto* dtype=dtype_descriptor(contract.dtype);
            const unsigned bits=contract.representation.valid_bits?contract.representation.valid_bits:dtype->bytes*8;
            if(!dtype->ieee && bits<width*8) {
                const auto status=decode_padding(contract,*dtype,bits,width,raw);if(status!=Status::ok)return status;
            }
            if(!dtype->ieee && dtype->signed_value && (raw&(std::uint64_t{1}<<(bits-1))))raw|=mask(dtype->bytes*8)&~mask(bits);
            return Status::ok;
        }
        Status validate_integer_bits(const DTypeDescriptor& dtype,unsigned bits,std::uint64_t value) {
            if(dtype.ieee || bits>=dtype.bytes*8)return Status::ok;
            const auto truncated=value&mask(bits);
            const auto extended=dtype.signed_value && (truncated&(std::uint64_t{1}<<(bits-1)))?truncated|(mask(dtype.bytes*8)&~mask(bits)):truncated;
            return (value&mask(dtype.bytes*8))==extended?Status::ok:Status::overflow;
        }
        Status encode_integer_bits(const NumericContract& contract,std::size_t width,std::uint64_t value,std::uint64_t& raw) {
            const auto* dtype=dtype_descriptor(contract.dtype);
            const unsigned bits=contract.representation.valid_bits?contract.representation.valid_bits:dtype->bytes*8;
            const auto status=validate_integer_bits(*dtype,bits,value);if(status!=Status::ok)return status;
            raw=value&mask(bits);
            if(!dtype->ieee && bits<width*8) {
                if(contract.representation.placement==BitPlacement::MostSignificant)raw<<=(unsigned(width*8)-bits);
                else if(dtype->signed_value && (raw&(std::uint64_t{1}<<(bits-1))))raw|=mask(unsigned(width*8))&~mask(bits);
            }
            return Status::ok;
        }
        bool finite_bits(DType dtype,std::uint64_t bits) {
            if(dtype==DType::Float64)return std::isfinite(std::bit_cast<double>(bits));
            if(dtype==DType::Float32)return std::isfinite(std::bit_cast<float>(std::uint32_t(bits)));
            return true;
        }
        Status validate_value_row(const NumericView& view,std::size_t plane,std::size_t row,std::size_t columns,ValueDomain domain) {
            for(std::size_t column=0;column<columns;++column) {
                std::uint64_t bits=0;const auto status=load_bits(view,{plane,row,column},view.contract->dtype,bits);
                if(status!=Status::ok)return status;
                if(domain==ValueDomain::FiniteOnly && !finite_bits(view.contract->dtype,bits))return Status::invalid_argument;
            }
            return Status::ok;
        }
        Status validate_value_plane(const NumericView& view,std::size_t plane,ValueDomain domain) {
            const auto dimensions=rank(*view.contract);
            const auto rows=dimensions?view.actual[0]:1,columns=dimensions==2?view.actual[1]:1;
            for(std::size_t row=0;row<rows;++row) {
                const auto status=validate_value_row(view,plane,row,columns,domain);if(status!=Status::ok)return status;
            }
            return Status::ok;
        }
        bool metadata_alias(std::span<const std::byte> bytes,const NumericView& view) {
            if(overlaps(bytes,std::as_bytes(std::span(view.contract,1))) || overlaps(bytes,std::as_bytes(view.planes)) || overlaps(bytes,std::as_bytes(view.contract->channel_order)) || overlaps(bytes,std::as_bytes(std::span(view.contract->clock.domain.data(),view.contract->clock.domain.size()))) || overlaps(bytes,view.ring.silence_mask))return true;
            for(const auto coordinate:view.coordinates.axes)if(overlaps(bytes,coordinate))return true;
            return false;
        }
        Status silence_plane(MutableNumericView& view,std::size_t plane,std::uint64_t value) {
            const auto dimensions=rank(*view.contract);
            const auto rows=dimensions?view.actual[0]:1,columns=dimensions==2?view.actual[1]:1;
            for(std::size_t row=0;row<rows;++row)for(std::size_t column=0;column<columns;++column) {
                const auto status=store_bits(view,{plane,row,column},view.contract->dtype,value);if(status!=Status::ok)return status;
            }
            return Status::ok;
        }
        Status validate_stamp(const FrameStamp& stamp,const FrameLimits& limits) {
            if(stamp.stream_id.size()>limits.max_string_bytes)return Status::exhausted;
            if(stamp.latency_seconds.numerator<0 || !stamp.latency_seconds.denominator)return Status::invalid_argument;
            if(stamp.has_sample_interval && (stamp.sample_count>std::uint64_t(INT64_MAX) || (stamp.sample_count && stamp.sample_begin>INT64_MAX-std::int64_t(stamp.sample_count-1))))return Status::overflow;
            return Status::ok;
        }
        Status validate_numeric_availability(const NumericView& view,bool available,const FrameLimits& limits) {
            if(available)return numeric_validation(view,limits);
            if(!view.contract || !view.storage.empty() || view.planes.size()>limits.max_planes)return Status::invalid_argument;
            return validate_contract(DataContract{*view.contract},limits);
        }
        Status validate_structured_view(const StructuredView& view,bool available,const FrameLimits& limits) {
            if(!view.contract)return Status::invalid_argument;
            const auto status=validate_contract(DataContract{*view.contract},limits);if(status!=Status::ok)return status;
            if(available?(view.bytes.size()<view.contract->payload_bytes.minimum || view.bytes.size()>view.contract->payload_bytes.maximum):!view.bytes.empty())return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_payload_view(const DataFrameView& frame,const FrameLimits& limits) {
            if(const auto* numeric=std::get_if<NumericView>(&frame.payload))return validate_numeric_availability(*numeric,frame.stamp.available,limits);
            return validate_structured_view(std::get<StructuredView>(frame.payload),frame.stamp.available,limits);
        }
        std::string_view metadata_text(std::span<const std::byte> bytes,std::size_t offset,std::size_t count) {
            return count?std::string_view(reinterpret_cast<const char*>(bytes.data()+offset),count):std::string_view{};
        }
        bool history_output_aliases(const NumericView& source,std::span<const std::byte> output) {
            return overlaps(output,std::as_bytes(std::span(&source,1))) ||
                overlaps(output,source.storage) || metadata_alias(output,source);
        }
        bool history_outputs_disjoint(const NumericView& source,const NumericView& output,
                                      const NumericContract& contract,std::span<PlaneLayout> planes) {
            const auto view_bytes=std::as_bytes(std::span(&output,1));
            const auto contract_bytes=std::as_bytes(std::span(&contract,1));
            const auto plane_bytes=std::as_bytes(planes.first(source.planes.size()));
            return !history_output_aliases(source,view_bytes) && !history_output_aliases(source,contract_bytes) &&
                !history_output_aliases(source,plane_bytes) && !overlaps(view_bytes,contract_bytes) &&
                !overlaps(view_bytes,plane_bytes) && !overlaps(contract_bytes,plane_bytes);
        }
        Status validate_history_planes(const NumericView& source,std::size_t row) {
            std::size_t width=0,position=0;
            bool silent=false;
            for(std::size_t plane=0;plane<source.planes.size();++plane) {
                const auto status=offset(source,{plane,row,0},position,width,silent);
                if(status!=Status::ok)return status;
            }
            return Status::ok;
        }
        NumericContract history_element_contract(const NumericContract& source) {
            auto result=source;
            result.semantic=source.history_element;
            result.history_element=Semantic::Numeric;
            result.shape.rank=Rank::Vector;
            result.shape.axes={source.shape.axes[1],Axis::None};
            result.shape.extents={source.shape.extents[1],ExtentBounds{}};
            result.coordinates={source.coordinates[1],AxisCoordinates{}};
            return result;
        }
        void publish_history_row(const NumericView& source,std::size_t row,NumericView& output,
                                 NumericContract& contract,std::span<PlaneLayout> planes) {
            std::size_t width=0,position=0;
            bool silent=false;
            offset(source,{0,row,0},position,width,silent);
            contract=history_element_contract(*source.contract);
            for(std::size_t plane=0;plane<source.planes.size();++plane) {
                bool plane_silent=false;
                offset(source,{plane,row,0},position,width,plane_silent);
                planes[plane]={position,{source.planes[plane].stride_bytes[1],0}};
            }
            output={&contract,source.storage,planes.first(source.planes.size()),{source.actual[1],1},{},{},silent};
            output.coordinates.axes[0]=source.coordinates.axes[1];
        }
    }
    Status required_storage(const NumericContract& c,std::span<const PlaneLayout> p,std::size_t& out) noexcept {return layout(c,p,out);
    }
    Status validate_view(const DataFrameView& f,const FrameLimits& l) noexcept {
    auto status=validate_stamp(f.stamp,l);if(status!=Status::ok)return status;
    status=validate_payload_view(f,l);if(status!=Status::ok)return status;
    MetaPlan metadata;
    status=metadata_plan(f,l,metadata);
    if(status!=Status::ok)return status;
    return f.stamp.available?Status::ok:Status::unavailable;
}
Status validate_mutable(const MutableNumericView& v,const FrameLimits& l) noexcept {
    return validate_view(DataFrameView{{},v.view()},l);
}
Status load_bits(const NumericView& v,ElementIndex i,DType type,std::uint64_t& out) noexcept {
        if(!v.contract || v.contract->dtype!=type)return Status::incompatible_contract;
        std::size_t pos=0,width=0;
        bool silent=false;
        auto s=offset(v,i,pos,width,silent);
        if(s!=Status::ok)return s;
        if(silent){out=silence(*v.contract);
            return Status::ok;
        }
        auto raw=read(v.storage.subspan(pos,width),big(v.contract->representation.encoding));
        s=decode_integer_bits(*v.contract,width,raw);if(s!=Status::ok)return s;
        out=raw;
        return Status::ok;
    }
    Status store_bits(MutableNumericView& v,ElementIndex i,DType type,std::uint64_t bits_value) noexcept {
        if(!v.contract || v.contract->dtype!=type)return Status::incompatible_contract;
        std::size_t pos=0,width=0;
        bool silent=false;
        auto s=offset(v.view(),i,pos,width,silent);
        if(s!=Status::ok)return s;
        std::uint64_t raw=0;s=encode_integer_bits(*v.contract,width,bits_value,raw);if(s!=Status::ok)return s;
        write(v.storage.subspan(pos,width),raw,big(v.contract->representation.encoding));
        return Status::ok;
    }
    Status validate_values(const NumericView& v,ValueDomain domain,const FrameLimits& l) noexcept {
        auto s=numeric_validation(v,l);
        if(s!=Status::ok)return s;
        if(domain!=ValueDomain::FiniteOnly && domain!=ValueDomain::AnyIeee)return Status::invalid_argument;
        for(std::size_t plane=0;plane<v.planes.size();++plane) {
            s=validate_value_plane(v,plane,domain);if(s!=Status::ok)return s;
        }
        return Status::ok;
    }
    Status fill_silence(MutableNumericView& v,const FrameLimits& l) noexcept {
        auto s=validate_mutable(v,l);
        if(s!=Status::ok)return s;
        if(overlaps(v.storage,std::as_bytes(std::span(&v,1))) || metadata_alias(v.storage,v.view()))return Status::invalid_argument;
        const auto value=silence(*v.contract);
        for(std::size_t plane=0;plane<v.planes.size();++plane){s=silence_plane(v,plane,value);
            if(s!=Status::ok)return s;
        }return Status::ok;
    }
static Status validate_history_row_source(const NumericView& view,std::size_t row) noexcept {
    constexpr FrameLimits structural_limits{maximum,maximum,maximum,maximum,maximum,maximum};
    auto status=validate_view(DataFrameView{{},view},structural_limits);
    if(status!=Status::ok)return status;
    status=validate_history_planes(view,row);
    if(status!=Status::ok)return status;
    const auto candidate=history_element_contract(*view.contract);
    return validate_contract(DataContract{candidate},structural_limits);
}

    Status history_row(const NumericView& v,std::size_t row,NumericView& out,NumericContract& c,std::span<PlaneLayout> planes) noexcept {
    if(!v.contract || v.contract->shape.rank!=Rank::Matrix || row>=v.actual[0] ||
       planes.size()<v.planes.size())return Status::invalid_argument;
    if(!history_outputs_disjoint(v,out,c,planes))return Status::invalid_argument;
    const auto status=validate_history_row_source(v,row);
    if(status!=Status::ok)return status;
    publish_history_row(v,row,out,c,planes);
    return Status::ok;
}
    Status retention_requirements(const DataFrameView& f,const FrameLimits& l,RetentionRequirements& out) noexcept {
        auto s=validate_view(f,l);
        if(s!=Status::ok && s!=Status::unavailable)return s;
        MetaPlan m;
        s=metadata_plan(f,l,m);
        if(s!=Status::ok)return s;
        const auto bytes=std::holds_alternative<NumericView>(f.payload)?std::get<NumericView>(f.payload).storage.size():std::get<StructuredView>(f.payload).bytes.size();
        out={bytes,m.size,std::holds_alternative<NumericView>(f.payload)?alignof(PlaneLayout):alignof(char)};
        return Status::ok;
    }
    struct FrameRetention {
        const DataFrameView& source;const FrameLimits& limits;BufferPool& payload;BufferPool& metadata;
        RetentionRequirements requirements;MetaPlan plan;OwnedDataFrame candidate;
        std::span<const std::byte> input() const {
            if(const auto* numeric=std::get_if<NumericView>(&source.payload))return numeric->storage;
            return std::get<StructuredView>(source.payload).bytes;
        }
        bool conflict(std::span<const std::byte> bytes) const {
            return overlaps(candidate.payload_.bytes(),bytes) || overlaps(candidate.metadata_.bytes(),bytes);
        }
        bool structured_alias(const StructuredContract& contract) const {
            return conflict(std::as_bytes(std::span(&contract,1))) || conflict(std::as_bytes(std::span(contract.schema.data(),contract.schema.size()))) || conflict(std::as_bytes(std::span(contract.encoding.data(),contract.encoding.size())));
        }
        Status numeric_source_aliases(const NumericView& numeric) const {
            if(metadata_alias(candidate.payload_.bytes(),numeric) || metadata_alias(candidate.metadata_.bytes(),numeric))return Status::invalid_argument;
            if(!source.stamp.available)return Status::ok;
            for(const auto& plane:numeric.planes) {
                if(reinterpret_cast<std::uintptr_t>(candidate.payload_.bytes().data()+plane.offset)%numeric.contract->representation.alignment)return Status::invalid_argument;
            }
            return Status::ok;
        }
        Status source_aliases() const {
            if(conflict(std::as_bytes(std::span(&source,1))) || conflict(std::as_bytes(std::span(source.stamp.stream_id.data(),source.stamp.stream_id.size()))))return Status::invalid_argument;
            if(const auto* numeric=std::get_if<NumericView>(&source.payload))return numeric_source_aliases(*numeric);
            if(structured_alias(*std::get<StructuredView>(source.payload).contract))return Status::invalid_argument;
            return Status::ok;
        }
        Status allocate() {
            auto status=payload.acquire(requirements.payload_bytes,candidate.payload_);if(status!=Status::ok)return status;
            status=metadata.acquire(requirements.metadata_bytes,candidate.metadata_);if(status!=Status::ok)return status;
            const auto pb=candidate.payload_.bytes(),mb=candidate.metadata_.bytes();
            if(!mb.empty() && reinterpret_cast<std::uintptr_t>(mb.data())%requirements.metadata_alignment)return Status::invalid_argument;
            if(overlaps(pb,mb) || overlaps(pb,input()) || overlaps(mb,input()))return Status::invalid_argument;
            return source_aliases();
        }
        void copy(std::size_t start,std::span<const std::byte> bytes) {
            if(!bytes.empty())std::memcpy(candidate.metadata_.bytes().data()+start,bytes.data(),bytes.size());
        }
        void text(std::size_t start,std::string_view value) {copy(start,std::as_bytes(std::span(value.data(),value.size())));}
        void numeric_fields(const NumericView& source) {
            candidate.contract_=*source.contract;candidate.actual_=source.actual;
            candidate.oldest_=source.ring.oldest;candidate.capacity_=source.ring.capacity;candidate.semantic_silence_=source.semantic_silence;
            candidate.planes_offset_=plan.planes;candidate.planes_count_=source.planes.size();candidate.channels_offset_=plan.channels;
            candidate.clock_offset_=plan.clock;candidate.mask_offset_=plan.mask;candidate.mask_size_=source.ring.silence_mask.size();
        }
        void coordinates(const NumericView& source) {
            for(std::size_t axis=0;axis<2;++axis) {
                candidate.coordinate_offsets_[axis]=plan.coordinates[axis];candidate.coordinate_sizes_[axis]=source.coordinates.axes[axis].size();
                copy(plan.coordinates[axis],source.coordinates.axes[axis]);
            }
        }
        void numeric_arrays(const NumericView& source) {
            auto bytes=candidate.metadata_.bytes();
            // Placement arrays establish object lifetimes without allocating or array cookies.
            auto* planes=::new(static_cast<void*>(bytes.data()+plan.planes)) PlaneLayout[source.planes.size()];
            std::copy(source.planes.begin(),source.planes.end(),planes);
            auto* channels=::new(static_cast<void*>(bytes.data()+plan.channels)) std::uint32_t[source.contract->channel_order.size()];
            std::copy(source.contract->channel_order.begin(),source.contract->channel_order.end(),channels);
            text(plan.clock,source.contract->clock.domain);copy(plan.mask,source.ring.silence_mask);
            auto& stored=std::get<NumericContract>(candidate.contract_);stored.channel_order={channels,source.contract->channel_order.size()};
            stored.clock.domain=source.contract->clock.domain.empty()?std::string_view{}:std::string_view(reinterpret_cast<const char*>(bytes.data()+plan.clock),source.contract->clock.domain.size());
            coordinates(source);
        }
        void structured(const StructuredContract& contract) {
            candidate.contract_=contract;candidate.schema_offset_=plan.schema;candidate.encoding_offset_=plan.encoding;
            text(plan.schema,contract.schema);text(plan.encoding,contract.encoding);
            auto& stored=std::get<StructuredContract>(candidate.contract_);const auto bytes=candidate.metadata_.bytes();
            stored.schema={reinterpret_cast<const char*>(bytes.data()+plan.schema),contract.schema.size()};
            stored.encoding={reinterpret_cast<const char*>(bytes.data()+plan.encoding),contract.encoding.size()};
        }
        void capture() {
            const auto bytes=input();if(!bytes.empty())std::memcpy(candidate.payload_.bytes().data(),bytes.data(),bytes.size());
            candidate.stamp_=source.stamp;candidate.stream_offset_=plan.stream;text(plan.stream,source.stamp.stream_id);
            if(const auto* numeric=std::get_if<NumericView>(&source.payload)){numeric_fields(*numeric);numeric_arrays(*numeric);}
            else structured(*std::get<StructuredView>(source.payload).contract);
        }
        Status prepare() {
            auto status=retention_requirements(source,limits,requirements);if(status!=Status::ok)return status;
            status=metadata_plan(source,limits,plan);if(status!=Status::ok)return status;
            status=allocate();if(status!=Status::ok)return status;
            capture();return Status::ok;
        }
    };
    Status retain_frame(const DataFrameView& frame,const FrameLimits& limits,BufferPool& payload,BufferPool& metadata,OwnedDataFrame& out) noexcept {
        FrameRetention retention{frame,limits,payload,metadata};
        const auto status=retention.prepare();if(status!=Status::ok)return status;
        out=std::move(retention.candidate);return Status::ok;
    }
    DataFrameView OwnedDataFrame::view() const noexcept {
        if(!*this)return {};
        auto mb=metadata_.bytes();
        DataFrameView f;
        f.stamp=stamp_;
        f.stamp.stream_id=metadata_text(mb,stream_offset_,stamp_.stream_id.size());
        // Descriptors already point into shared immutable metadata;
        // Default moves/copies retain that handle.
        if(const auto* n=std::get_if<NumericContract>(&contract_)){
            const auto* ps=std::launder(reinterpret_cast<const PlaneLayout*>(mb.data()+planes_offset_));
            NumericView v{n,payload_.bytes(),{ps,planes_count_},actual_,{capacity_,oldest_,mb.subspan(mask_offset_,mask_size_)},{},semantic_silence_};
            for(std::size_t i=0;i<2;++i)v.coordinates.axes[i]=mb.subspan(coordinate_offsets_[i],coordinate_sizes_[i]);
            f.payload=v;
        } else f.payload=StructuredView{&std::get<StructuredContract>(contract_),payload_.bytes()};
        return f;
    }
}
