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
        Status offset(const NumericView& v,ElementIndex i,std::size_t& out,std::size_t& width,bool& silent){
            if(!v.contract || i.plane>=v.planes.size() || v.planes.size()!=v.contract->channel_order.size())return Status::invalid_argument;
            auto s=storage_width(*v.contract,width);
            if(s!=Status::ok)return s;
            if(v.contract->shape.rank!=Rank::Scalar && v.contract->shape.rank!=Rank::Vector && v.contract->shape.rank!=Rank::Matrix)return Status::invalid_argument;
            const auto r=rank(*v.contract);
            if(!r && v.planes.size()!=1)return Status::invalid_argument;
            for(std::size_t axis=0;axis<2;++axis)if(v.actual[axis]<v.contract->shape.extents[axis].minimum || v.actual[axis]>v.contract->shape.extents[axis].maximum || (axis>=r && v.actual[axis]!=1))return Status::invalid_argument;
            if((r==0 && (i.row || i.column)) || (r==1 && (i.row>=v.actual[0] || i.column)) || (r==2 && (i.row>=v.actual[0] || i.column>=v.actual[1])))return Status::invalid_argument;
            auto row=i.row;
            silent=v.semantic_silence;
            if(v.ring.capacity){if(r!=2 || !v.ring.capacity || v.actual[0]>v.ring.capacity || v.ring.oldest>=v.ring.capacity || (!v.ring.silence_mask.empty() && v.ring.silence_mask.size()<v.actual[0]))return Status::invalid_argument;
                if(!v.ring.silence_mask.empty()){
                    if(v.ring.silence_mask[i.row]!=std::byte{0} && v.ring.silence_mask[i.row]!=std::byte{1})return Status::malformed;
                    silent=silent || v.ring.silence_mask[i.row]!=std::byte{0};
                }
                row=v.ring.oldest;
                const auto tail=v.ring.capacity-row;
                row=i.row>=tail?i.row-tail:row+i.row;
            }
            out=v.planes[i.plane].offset;
            std::size_t term=0;
            if(r && (!product(row,v.planes[i.plane].stride_bytes[0],term) || !add(out,term)))return Status::overflow;
            if(r==2 && (!product(i.column,v.planes[i.plane].stride_bytes[1],term) || !add(out,term)))return Status::overflow;
            if(out>v.storage.size() || width>v.storage.size()-out)return Status::buffer_too_small;
            if(!v.contract->representation.alignment || reinterpret_cast<std::uintptr_t>(v.storage.data()+out)%v.contract->representation.alignment)return Status::invalid_argument;
            return Status::ok;
        }
        Status layout(const NumericContract& c,std::span<const PlaneLayout> planes,std::size_t& out){
            std::size_t w=0;
            auto s=storage_width(c,w);
            if(s!=Status::ok)return s;
            if(planes.empty() || planes.size()!=c.channel_order.size() || !c.representation.alignment || (c.representation.alignment&(c.representation.alignment-1)))return Status::invalid_argument;
            if(c.shape.rank!=Rank::Scalar && c.shape.rank!=Rank::Vector && c.shape.rank!=Rank::Matrix)return Status::invalid_argument;
            const auto r=rank(c);
            std::array<std::size_t,2> e{1,1};
            for(std::size_t j=0;j<r;++j){if(!c.shape.extents[j].maximum || c.shape.extents[j].maximum>maximum)return Status::invalid_argument;
                e[j]=static_cast<std::size_t>(c.shape.extents[j].maximum);
            }
            std::size_t end_max=0;
            const auto inter=c.representation.layout==ChannelLayout::Interleaved;
            std::size_t lane=0;
            if(!product(planes.size(),w,lane))return Status::overflow;
            for(std::size_t i=0;i<planes.size();++i){const auto& p=planes[i];
                std::size_t end=p.offset,term=0;
                if(r){auto minimum_stride=w;
                    if(inter)minimum_stride=lane;
                    if(r==2){if(p.stride_bytes[1]<minimum_stride)return Status::invalid_argument;
                        if(!product(e[1],p.stride_bytes[1],term))return Status::overflow;
                        if(p.stride_bytes[0]<term)return Status::invalid_argument;
                    }
                    else if(p.stride_bytes[0]<minimum_stride)return Status::invalid_argument;
                    for(std::size_t j=0;j<r;++j){if(p.stride_bytes[j]%c.representation.alignment)return Status::invalid_argument;
                        if(!product(e[j]-1,p.stride_bytes[j],term) || !add(end,term))return Status::overflow;
                    }}
                if(!add(end,w))return Status::overflow;
                if(inter){std::size_t expected=planes[0].offset;
                    if(!product(i,w,term) || !add(expected,term))return Status::overflow;
                    if(p.offset!=expected || p.stride_bytes!=planes[0].stride_bytes)return Status::invalid_argument;
                }
                else for(std::size_t j=0;j<i;++j){std::size_t other=planes[j].offset;
                    for(std::size_t k=0;k<r;++k){if(!product(e[k]-1,planes[j].stride_bytes[k],term) || !add(other,term))return Status::overflow;
                    }if(!add(other,w))return Status::overflow;
                    if(p.offset<other && planes[j].offset<end)return Status::invalid_argument;
                }
                end_max=std::max(end_max,end);
            }
            out=end_max;
            return Status::ok;
        }
        Status numeric_validation(const NumericView& v,const FrameLimits& l){
            if(!v.contract)return Status::invalid_argument;
            auto s=validate_contract(DataContract{*v.contract},l);
            if(s!=Status::ok)return s;
            const auto r=rank(*v.contract);
            for(std::size_t i=0;i<2;++i){const auto& bound=v.contract->shape.extents[i];
                if(v.actual[i]<bound.minimum || v.actual[i]>bound.maximum || (i>=r && v.actual[i]!=1))return Status::invalid_argument;
            }
            if(v.ring.capacity){if(r!=2 || v.ring.capacity!=v.contract->shape.extents[0].maximum || !v.ring.capacity || v.ring.oldest>=v.ring.capacity || (!v.ring.silence_mask.empty() && v.ring.silence_mask.size()!=v.ring.capacity))return Status::invalid_argument;
                for(auto m:v.ring.silence_mask)if(m!=std::byte{0} && m!=std::byte{1})return Status::invalid_argument;
            }
            else if(v.ring.capacity || v.ring.oldest || !v.ring.silence_mask.empty())return Status::invalid_argument;
            std::size_t needed=0;
            s=layout(*v.contract,v.planes,needed);
            if(s!=Status::ok)return s;
            if(needed>l.max_payload_bytes || v.storage.size()>l.max_payload_bytes)return Status::exhausted;
            if(needed>v.storage.size())return Status::buffer_too_small;
            for(const auto& p:v.planes)if(reinterpret_cast<std::uintptr_t>(v.storage.data()+p.offset)%v.contract->representation.alignment)return Status::invalid_argument;
            std::size_t coordinates=0;
            for(std::size_t i=0;i<2;++i){const auto bytes=v.coordinates.axes[i];
                const auto mode=v.contract->coordinates[i].mode;
                if(mode==CoordinateMode::ExplicitFloat64LE){std::size_t size=0;
                    if(!product(v.actual[i],8,size))return Status::overflow;
                    if(bytes.size()!=size)return Status::invalid_argument;
                    for(std::size_t n=0;n<bytes.size();n+=8){auto bits=read(bytes.subspan(n,8),false);
                        double value;
                        std::memcpy(&value,&bits,8);
                        if(!std::isfinite(value))return Status::invalid_argument;
                    }}
                else if(!bytes.empty())return Status::invalid_argument;
                if(!add(coordinates,bytes.size()))return Status::overflow;
            }
            if(coordinates>l.max_coordinate_bytes)return Status::exhausted;
            return Status::ok;
        }
        struct MetaPlan {std::size_t size=0,planes=0,channels=0,schema=0,encoding=0,clock=0,stream=0,mask=0;
            std::array<std::size_t,2> coordinates{};
        };
        Status metadata_plan(const DataFrameView& f,const FrameLimits& l,MetaPlan& p){
            auto append=[&](std::size_t n,std::size_t& start){start=p.size;
                return add(p.size,n);
            };
            if(const auto* n=std::get_if<NumericView>(&f.payload)){
                std::size_t count=0;
                if(!align(p.size,alignof(PlaneLayout)))return Status::overflow;
                p.planes=p.size;
                if(!product(n->planes.size(),sizeof(PlaneLayout),count) || !add(p.size,count) || !align(p.size,alignof(std::uint32_t)))return Status::overflow;
                p.channels=p.size;
                if(!product(n->contract->channel_order.size(),sizeof(std::uint32_t),count) || !add(p.size,count) || !append(n->contract->clock.domain.size(),p.clock) || !append(n->ring.silence_mask.size(),p.mask))return Status::overflow;
                for(std::size_t i=0;i<2;++i)if(!append(n->coordinates.axes[i].size(),p.coordinates[i]))return Status::overflow;
            } else {const auto& c=*std::get<StructuredView>(f.payload).contract;
                if(!append(c.schema.size(),p.schema) || !append(c.encoding.size(),p.encoding))return Status::overflow;
            }
            if(!append(f.stamp.stream_id.size(),p.stream))return Status::overflow;
            if(p.size>l.max_metadata_bytes)return Status::exhausted;
            return Status::ok;
        }
        bool overlaps(std::span<const std::byte> a,std::span<const std::byte> b){if(a.empty() || b.empty())return false;
            auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
            return x<=y?y-x<a.size():x-y<b.size();
        }
    }
    Status required_storage(const NumericContract& c,std::span<const PlaneLayout> p,std::size_t& out) noexcept {return layout(c,p,out);
    }
    Status validate_view(const DataFrameView& f,const FrameLimits& l) noexcept {
    if(f.stamp.stream_id.size()>l.max_string_bytes)return Status::exhausted;
    if(f.stamp.latency_seconds.numerator<0 || !f.stamp.latency_seconds.denominator)return Status::invalid_argument;
    if(f.stamp.has_sample_interval && (f.stamp.sample_count>std::uint64_t(INT64_MAX) || (f.stamp.sample_count && f.stamp.sample_begin>INT64_MAX-std::int64_t(f.stamp.sample_count-1))))return Status::overflow;
    Status status=Status::ok;
    if(const auto* n=std::get_if<NumericView>(&f.payload)) {
        if(!f.stamp.available) {
            if(!n->contract || !n->storage.empty() || n->planes.size()>l.max_planes)return Status::invalid_argument;
            status=validate_contract(DataContract{*n->contract},l);
        } else status=numeric_validation(*n,l);
    } else {
        const auto& p=std::get<StructuredView>(f.payload);
        if(!p.contract)return Status::invalid_argument;
        status=validate_contract(DataContract{*p.contract},l);
        if(status==Status::ok && (f.stamp.available ?
           (p.bytes.size()<p.contract->payload_bytes.minimum || p.bytes.size()>p.contract->payload_bytes.maximum) : !p.bytes.empty()))status=Status::invalid_argument;
    }
    if(status!=Status::ok)return status;
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
        const auto* d=dtype_descriptor(type);
        const unsigned bits=v.contract->representation.valid_bits?v.contract->representation.valid_bits:d->bytes*8;
        auto raw=read(v.storage.subspan(pos,width),big(v.contract->representation.encoding));
        if(!d->ieee && bits<width*8){const bool msb=v.contract->representation.placement==BitPlacement::MostSignificant;
            const auto shift=unsigned(width*8)-bits;
            if(msb){if(raw&mask(shift))return Status::malformed;
                raw>>=shift;
            }
            else {const auto high=raw&~mask(bits);
                const auto expected=d->signed_value && (raw&(std::uint64_t{1}<<(bits-1)))?mask(unsigned(width*8))&~mask(bits):0;
                if(high!=expected)return Status::malformed;
                raw&=mask(bits);
            }}
        if(!d->ieee && d->signed_value && (raw&(std::uint64_t{1}<<(bits-1))))raw|=mask(d->bytes*8)&~mask(bits);
        out=raw;
        return Status::ok;
    }
    Status store_bits(MutableNumericView& v,ElementIndex i,DType type,std::uint64_t bits_value) noexcept {
        if(!v.contract || v.contract->dtype!=type)return Status::incompatible_contract;
        std::size_t pos=0,width=0;
        bool silent=false;
        auto s=offset(v.view(),i,pos,width,silent);
        if(s!=Status::ok)return s;
        const auto* d=dtype_descriptor(type);
        const unsigned bits=v.contract->representation.valid_bits?v.contract->representation.valid_bits:d->bytes*8;
        if(!d->ieee && bits<d->bytes*8){const auto truncated=bits_value&mask(bits);
            const auto extended=d->signed_value && (truncated&(std::uint64_t{1}<<(bits-1)))?truncated|(mask(d->bytes*8)&~mask(bits)):truncated;
            if((bits_value&mask(d->bytes*8))!=extended)return Status::overflow;
        }
        auto raw=bits_value&mask(bits);
        if(!d->ieee && bits<width*8){if(v.contract->representation.placement==BitPlacement::MostSignificant)raw<<=(unsigned(width*8)-bits);
            else if(d->signed_value && (raw&(std::uint64_t{1}<<(bits-1))))raw|=mask(unsigned(width*8))&~mask(bits);
        }
        write(v.storage.subspan(pos,width),raw,big(v.contract->representation.encoding));
        return Status::ok;
    }
    Status validate_values(const NumericView& v,ValueDomain domain,const FrameLimits& l) noexcept {
        auto s=numeric_validation(v,l);
        if(s!=Status::ok)return s;
        if(domain!=ValueDomain::FiniteOnly && domain!=ValueDomain::AnyIeee)return Status::invalid_argument;
        const auto r=rank(*v.contract);
        for(std::size_t p=0;p<v.planes.size();++p)for(std::size_t row=0;row<(r?v.actual[0]:1);++row)for(std::size_t col=0;col<(r==2?v.actual[1]:1);++col){std::uint64_t bits=0;
            s=load_bits(v,{p,row,col},v.contract->dtype,bits);
            if(s!=Status::ok)return s;
            if(domain==ValueDomain::FiniteOnly){if(v.contract->dtype==DType::Float64){double x;
                    std::memcpy(&x,&bits,8);
                    if(!std::isfinite(x))return Status::invalid_argument;
                }if(v.contract->dtype==DType::Float32){auto b=std::uint32_t(bits);
                    float x;
                    std::memcpy(&x,&b,4);
                    if(!std::isfinite(x))return Status::invalid_argument;
                }}}
        return Status::ok;
    }
    Status fill_silence(MutableNumericView& v,const FrameLimits& l) noexcept {
        auto s=validate_mutable(v,l);
        if(s!=Status::ok)return s;
        auto conflict=[&](std::span<const std::byte> metadata){return overlaps(v.storage,metadata);};
        if(conflict(std::as_bytes(std::span(&v,1))) || conflict(std::as_bytes(std::span(v.contract,1))) ||
           conflict(std::as_bytes(v.planes)) || conflict(std::as_bytes(v.contract->channel_order)) ||
           conflict(std::as_bytes(std::span(v.contract->clock.domain.data(),v.contract->clock.domain.size()))) ||
           conflict(v.ring.silence_mask))return Status::invalid_argument;
        for(auto coordinates:v.coordinates.axes)if(conflict(coordinates))return Status::invalid_argument;
        const auto r=rank(*v.contract);
        const auto value=silence(*v.contract);
        for(std::size_t p=0;p<v.planes.size();++p)for(std::size_t row=0;row<(r?v.actual[0]:1);++row)for(std::size_t col=0;col<(r==2?v.actual[1]:1);++col){s=store_bits(v,{p,row,col},v.contract->dtype,value);
            if(s!=Status::ok)return s;
        }return Status::ok;
    }
    Status history_row(const NumericView& v,std::size_t row,NumericView& out,NumericContract& c,std::span<PlaneLayout> planes) noexcept {
        if(!v.contract || v.contract->shape.rank!=Rank::Matrix || row>=v.actual[0] || planes.size()<v.planes.size())return Status::invalid_argument;
        const auto output_view=std::as_bytes(std::span(&out,1));
        const auto output_contract=std::as_bytes(std::span(&c,1));
        const auto output_planes=std::as_bytes(planes.first(v.planes.size()));
        // All writable outputs must be disjoint from every borrowed donor, not
        // merely the source plane array. Check before touching any output.
        auto aliases_source=[&](std::span<const std::byte> output) {
            if(overlaps(output,std::as_bytes(std::span(&v,1))) ||
               overlaps(output,std::as_bytes(std::span(v.contract,1))) ||
               overlaps(output,v.storage) || overlaps(output,std::as_bytes(v.planes)) ||
               overlaps(output,std::as_bytes(v.contract->channel_order)) ||
               overlaps(output,v.ring.silence_mask) ||
               overlaps(output,std::as_bytes(std::span(v.contract->clock.domain.data(),v.contract->clock.domain.size()))))return true;
            for(const auto coordinate:v.coordinates.axes)if(overlaps(output,coordinate))return true;
            return false;
        };
        if(aliases_source(output_view) || aliases_source(output_contract) || aliases_source(output_planes) ||
           overlaps(output_view,output_contract) || overlaps(output_view,output_planes) || overlaps(output_contract,output_planes))return Status::invalid_argument;
        // This accessor has no admission limits argument; enforce contract
        // invariants without inventing a profile ceiling or reading all rows.
        constexpr FrameLimits structural_limits{maximum,maximum,maximum,maximum,maximum,maximum};
        auto s=validate_view(DataFrameView{{},v},structural_limits);
        if(s!=Status::ok)return s;
        std::size_t w=0,pos=0;
        bool silent=false;
        s=offset(v,{0,row,0},pos,w,silent);
        if(s!=Status::ok)return s;
        for(std::size_t i=0;i<v.planes.size();++i){std::size_t p=0;
            bool flag=false;
            s=offset(v,{i,row,0},p,w,flag);
            if(s!=Status::ok)return s;
        }
        auto candidate=*v.contract;
        candidate.semantic=candidate.history_element;
        candidate.history_element=Semantic::Numeric;
        candidate.shape.rank=Rank::Vector;
        candidate.shape.axes={v.contract->shape.axes[1],Axis::None};
        candidate.shape.extents={v.contract->shape.extents[1],ExtentBounds{}};
        candidate.coordinates={v.contract->coordinates[1],AxisCoordinates{}};
        s=validate_contract(DataContract{candidate},structural_limits);
        if(s!=Status::ok)return s;
        // Source bytes/descriptors remain immutable and all offsets were checked;
        // the following publication cannot fail or overwrite any donor.
        c=candidate;
        for(std::size_t i=0;i<v.planes.size();++i){std::size_t plane_pos=0;
            bool flag=false;
            offset(v,{i,row,0},plane_pos,w,flag);
            planes[i]={plane_pos,{v.planes[i].stride_bytes[1],0}};
        }
        out={&c,v.storage,planes.first(v.planes.size()),{v.actual[1],1},{},{},silent};
        out.coordinates.axes[0]=v.coordinates.axes[1];
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
    Status retain_frame(const DataFrameView& f,const FrameLimits& l,BufferPool& payload,BufferPool& metadata,OwnedDataFrame& out) noexcept {
        RetentionRequirements req;
        auto s=retention_requirements(f,l,req);
        if(s!=Status::ok)return s;
        MetaPlan m;
        s=metadata_plan(f,l,m);
        if(s!=Status::ok)return s;
        OwnedDataFrame x;
        s=payload.acquire(req.payload_bytes,x.payload_);
        if(s!=Status::ok)return s;
        s=metadata.acquire(req.metadata_bytes,x.metadata_);
        if(s!=Status::ok)return s;
        auto pb=x.payload_.bytes(),mb=x.metadata_.bytes();
        if(!mb.empty() && reinterpret_cast<std::uintptr_t>(mb.data())%req.metadata_alignment)return Status::invalid_argument;
        const auto input=std::holds_alternative<NumericView>(f.payload)?std::get<NumericView>(f.payload).storage:std::get<StructuredView>(f.payload).bytes;
        if(overlaps(pb,mb) || overlaps(pb,input) || overlaps(mb,input))return Status::invalid_argument;
        auto conflict=[&](std::span<const std::byte> source){return overlaps(pb,source) || overlaps(mb,source);
        };
        if(conflict(std::as_bytes(std::span(&f,1))) || conflict(std::as_bytes(std::span(f.stamp.stream_id.data(),f.stamp.stream_id.size()))))return Status::invalid_argument;
        if(const auto* n=std::get_if<NumericView>(&f.payload)){
            if(conflict(std::as_bytes(std::span(n->contract,1))) || conflict(std::as_bytes(n->planes)) || conflict(std::as_bytes(n->contract->channel_order)) || conflict(n->ring.silence_mask) || conflict(std::as_bytes(std::span(n->contract->clock.domain.data(),n->contract->clock.domain.size()))))return Status::invalid_argument;
            for(const auto& coordinate:n->coordinates.axes)if(conflict(coordinate))return Status::invalid_argument;
            if(f.stamp.available)for(const auto& plane:n->planes)if(reinterpret_cast<std::uintptr_t>(pb.data()+plane.offset)%n->contract->representation.alignment)return Status::invalid_argument;
        } else {const auto& c=*std::get<StructuredView>(f.payload).contract;
            if(conflict(std::as_bytes(std::span(&c,1))) || conflict(std::as_bytes(std::span(c.schema.data(),c.schema.size()))) || conflict(std::as_bytes(std::span(c.encoding.data(),c.encoding.size()))))return Status::invalid_argument;
        }
        auto copy=[&](std::size_t start,std::span<const std::byte> data){if(!data.empty())std::memcpy(mb.data()+start,data.data(),data.size());
        };
        auto text=[&](std::size_t start,std::string_view value){copy(start,std::as_bytes(std::span(value.data(),value.size())));
        };
        if(!input.empty())std::memcpy(pb.data(),input.data(),input.size());
        x.stamp_=f.stamp;
        x.stream_offset_=m.stream;
        text(m.stream,f.stamp.stream_id);
        if(const auto* n=std::get_if<NumericView>(&f.payload)){
            x.contract_=*n->contract;
            x.actual_=n->actual;
            x.oldest_=n->ring.oldest;
            x.capacity_=n->ring.capacity;
            x.semantic_silence_=n->semantic_silence;
            x.planes_offset_=m.planes;
            x.planes_count_=n->planes.size();
            x.channels_offset_=m.channels;
            x.clock_offset_=m.clock;
            x.mask_offset_=m.mask;
            x.mask_size_=n->ring.silence_mask.size();
            // Non-allocating placement array new establishes metadata object lifetimes.
            // This overload has no array-cookie storage overhead (C++20 [expr.new]).
            auto* ps=::new(static_cast<void*>(mb.data()+m.planes)) PlaneLayout[n->planes.size()];
            std::copy(n->planes.begin(),n->planes.end(),ps);
            auto* cs=::new(static_cast<void*>(mb.data()+m.channels)) std::uint32_t[n->contract->channel_order.size()];
            std::copy(n->contract->channel_order.begin(),n->contract->channel_order.end(),cs);
            text(m.clock,n->contract->clock.domain);
            copy(m.mask,n->ring.silence_mask);
            auto& stored=std::get<NumericContract>(x.contract_);
            stored.channel_order={cs,n->contract->channel_order.size()};
            stored.clock.domain=n->contract->clock.domain.empty()?std::string_view{}:std::string_view(reinterpret_cast<const char*>(mb.data()+m.clock),n->contract->clock.domain.size());
            for(std::size_t i=0;i<2;++i){x.coordinate_offsets_[i]=m.coordinates[i];
                x.coordinate_sizes_[i]=n->coordinates.axes[i].size();
                copy(m.coordinates[i],n->coordinates.axes[i]);
            }
        } else {const auto& c=*std::get<StructuredView>(f.payload).contract;
            x.contract_=c;
            x.schema_offset_=m.schema;
            x.encoding_offset_=m.encoding;
            text(m.schema,c.schema);
            text(m.encoding,c.encoding);
            auto& stored=std::get<StructuredContract>(x.contract_);
            stored.schema={reinterpret_cast<const char*>(mb.data()+m.schema),c.schema.size()};
            stored.encoding={reinterpret_cast<const char*>(mb.data()+m.encoding),c.encoding.size()};
        }
        out=std::move(x);
        return Status::ok;
    }
    DataFrameView OwnedDataFrame::view() const noexcept {
        if(!*this)return {};
        auto mb=metadata_.bytes();
        DataFrameView f;
        f.stamp=stamp_;
        auto text=[&](std::size_t pos,std::size_t count){return count?std::string_view(reinterpret_cast<const char*>(mb.data()+pos),count):std::string_view{};
        };
        f.stamp.stream_id=text(stream_offset_,stamp_.stream_id.size());
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
