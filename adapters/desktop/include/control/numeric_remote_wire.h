#pragma once
#include <control/numeric_control_session.h>
#include <pipeline/numeric_delivery.h>
#include <bdsp_protocol/numeric_data_codec.h>
#include <bdsp_protocol/stream_fragments.h>
#include <deque>
#include <set>
#include <bit>
#include <cstring>
#include <limits>
#include <atomic>
#include <memory>
#include <new>
#include <algorithm>

namespace pipeline::remote_numeric {
namespace nd=bdsp_protocol::numeric_data;
namespace nc=bdsp_protocol::numeric_control;
using Status=signal_contracts::Status;
inline constexpr std::size_t MaxBindings=128,QueueSize=8;
inline constexpr std::size_t OrdinaryQueueSize=32;
inline constexpr std::uint16_t RowMetadataTag=1024;
inline std::uint64_t number(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width) {
    if(at>bytes.size()||width>bytes.size()-at)return 0;
    std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(bytes[at+i])<<(8*i);return value;
}
inline void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned width) {
    for(unsigned i=0;i<width;++i)bytes[at+i]=std::byte(value>>(8*i));
}
inline std::uint64_t graph_revision(const nc::Message& message) {
    for(const auto& e:message.extensions)if(e.tag==1024&&e.bytes.size()==16)return number(e.bytes,0,8);
    return 0;
}
inline signal_contracts::DataContract wire_contract(signal_contracts::DataContract value) {
    if(auto* n=std::get_if<signal_contracts::NumericContract>(&value))
        if(n->representation.encoding==signal_contracts::StorageEncoding::Native)
            n->representation.encoding=signal_contracts::StorageEncoding::FixedLittleEndian;
    return value;
}
inline signal_contracts::NumericContract compact_contract(signal_contracts::NumericContract value) {
    using namespace signal_contracts;
    value.semantic=value.history_element;value.history_element=Semantic::Numeric;
    value.shape={Rank::Vector,{value.shape.axes[1],Axis::None},{value.shape.extents[1],ExtentBounds{}}};
    value.coordinates={value.coordinates[1],AxisCoordinates{}};return value;
}
struct ArenaBudget {
    static constexpr std::size_t MaximumBytes=192*1024*1024;
    const std::size_t maximum;
    std::atomic<std::size_t> used{0};
    explicit ArenaBudget(std::size_t bytes=MaximumBytes):maximum(std::min(bytes,MaximumBytes)){}
};
struct ArenaLease {
    std::shared_ptr<ArenaBudget> budget;
    std::size_t bytes=0;
    ~ArenaLease(){if(budget)budget->used.fetch_sub(bytes,std::memory_order_relaxed);}
};
struct Arena {
    static constexpr std::size_t Slots=16;
    // Selected renderer owners + ordinary Host/endpoint queues + History row
    // and cold-snapshot queues at both boundaries + one replacement slot.
    // The endpoint cold state and snapshot queue share the same frame owners.
    static constexpr std::size_t DeliveryReserve=2*OrdinaryQueueSize+4*QueueSize+1;
    std::shared_ptr<ArenaLease> lease;
    signal_contracts::FrameLimits limits;
    std::vector<std::byte> payload,metadata;
    std::vector<signal_contracts::BufferSlot> payload_slots,metadata_slots;
    signal_contracts::BufferPool payload_pool,metadata_pool;
    explicit Arena(signal_contracts::FrameLimits l,std::size_t arena_slot_count=Slots,std::shared_ptr<ArenaLease> charge={}):
        lease(std::move(charge)),limits(l),payload(arena_slot_count*l.max_payload_bytes),metadata(arena_slot_count*l.max_metadata_bytes),
        payload_slots(arena_slot_count),metadata_slots(arena_slot_count),
        payload_pool(payload,payload_slots,l.max_payload_bytes),metadata_pool(metadata,metadata_slots,l.max_metadata_bytes){}
    static std::shared_ptr<Arena> prepare(signal_contracts::FrameLimits l,std::size_t selected,
        const std::shared_ptr<ArenaBudget>& budget) {
        if(!budget||selected>MaxBindings)return {};
        const auto arena_slot_count=selected+DeliveryReserve;
        const auto maximum=budget->maximum;
        if(l.max_payload_bytes>maximum/arena_slot_count||l.max_metadata_bytes>maximum/arena_slot_count)return {};
        const auto bytes=arena_slot_count*l.max_payload_bytes+arena_slot_count*l.max_metadata_bytes+
            2*arena_slot_count*sizeof(signal_contracts::BufferSlot);
        if(bytes>maximum)return {};
        auto used=budget->used.load(std::memory_order_relaxed);
        do {if(used>maximum-bytes)return {};}
        while(!budget->used.compare_exchange_weak(used,used+bytes,std::memory_order_relaxed));
        // Charge survives through all contexts retaining this generation. Failed
        // cold allocations roll it back; no pool grows while DATA is processed.
        std::shared_ptr<ArenaLease> charge;
        try {charge=std::make_shared<ArenaLease>();}
        catch(const std::bad_alloc&){budget->used.fetch_sub(bytes,std::memory_order_relaxed);return {};}
        charge->budget=budget;charge->bytes=bytes;
        try {return std::make_shared<Arena>(l,arena_slot_count,std::move(charge));}
        catch(const std::bad_alloc&){return {};}
    }
    static std::shared_ptr<Arena> prepare_rebind(signal_contracts::FrameLimits l,std::size_t selected,
        const std::shared_ptr<ArenaBudget>& budget,const std::shared_ptr<Arena>& current) {
        if(selected>MaxBindings)return {};
        if(current&&current->lease&&current->lease->budget==budget&&
            current->payload_slots.size()>=selected+DeliveryReserve&&
            current->limits.max_planes==l.max_planes&&current->limits.max_elements==l.max_elements&&
            current->limits.max_payload_bytes==l.max_payload_bytes&&current->limits.max_metadata_bytes==l.max_metadata_bytes&&
            current->limits.max_string_bytes==l.max_string_bytes&&current->limits.max_coordinate_bytes==l.max_coordinate_bytes)
            return current;
        return prepare(l,selected,budget);
    }
};
struct Binding {
    std::shared_ptr<const NumericDeliveryContext> context;
    domain::OwnedProcessingContract wire_owner;
    std::string stream;
    std::uint32_t id=0;
    bool acknowledged=false;
    bool ring=false; // Learned from explicit ring events/snapshots, never from a descriptive tag.
    bool sent=false;
    std::uint64_t sent_at=0;
    nd::Binding view() const {
        nd::Binding b;b.reference={id,1};const auto& e=context->endpoint;
        b.endpoint={e.graph_id,e.node,e.direction==domain::PortDirection::Input?nd::Direction::Input:nd::Direction::Output,e.port};
        b.stream=stream;b.contract=wire_owner.view();
        if(const auto* n=std::get_if<signal_contracts::NumericContract>(&b.contract);n&&n->shape.rank==signal_contracts::Rank::Matrix) {
            b.history_contract=*n;
        }
        return b;
    }
};
inline Status make_binding(const EndpointIdentity& endpoint,const domain::OwnedProcessingContract& contract,
    std::uint32_t id,const std::shared_ptr<Arena>& arena,Binding& out) {
    if(!contract||!id||endpoint.graph_id.empty()||endpoint.node<0)return Status::invalid_argument;
    auto context=std::make_shared<NumericDeliveryContext>();context->endpoint=endpoint;
    context->contract=contract;context->retention_owner=arena;
    Binding next;next.context=std::move(context);next.id=id;
    next.stream="port."+std::to_string(endpoint.node)+"."+(endpoint.direction==domain::PortDirection::Input?"in.":"out.")+std::to_string(endpoint.port);
    auto status=domain::own_processing_contract(wire_contract(contract.view()),arena->limits,arena->limits.max_metadata_bytes,next.wire_owner);
    if(status!=Status::ok)return status;
    out=std::move(next);return Status::ok;
}
inline nd::Message handshake(const Binding& b,std::uint64_t nonce,std::uint64_t session,std::uint64_t revision,std::uint64_t epoch) {
    nd::Message m;m.kind=nd::Kind::Unavailable;m.contract=b.wire_owner.view();m.endpoint=b.view().endpoint;
    m.stamp.available=false;m.stamp.stream_id=b.stream;
    m.envelope={nonce,session,1,epoch,0,revision,b.id};
    return m;
}
struct CodecStorage {
    nd::Limits limits=nd::desktop_limits();
    std::vector<std::byte> wire,scratch,projection;
    std::vector<std::max_align_t> arena_storage;
    std::vector<char> text;
    std::vector<std::uint32_t> channels;
    std::vector<signal_contracts::PlaneLayout> planes;
    std::vector<std::uint8_t> assembly_storage,fragment;
    bdsp_protocol::FrameAssembler assembler;
    std::vector<nd::Binding> references;
    std::size_t wire_bytes=0,wire_offset=0;
    std::uint8_t wire_kind=nd::EventsId;
    explicit CodecStorage(nd::Limits l=nd::desktop_limits()):limits(l),wire(l.message_bytes),scratch(l.message_bytes),
        projection(l.frame.max_payload_bytes),arena_storage((l.message_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t)),
        text(l.text_bytes),channels(2*l.frame.max_planes),planes(l.frame.max_planes),assembly_storage(l.message_bytes),
        fragment(bdsp_protocol::StreamFragmentBytes+8),assembler(assembly_storage) {references.reserve(MaxBindings);}
    nd::ReferenceContext reference_context(const std::deque<Binding>& bindings,std::uint64_t nonce,std::uint64_t session,std::uint64_t revision) {
        references.clear();for(const auto& b:bindings)if(b.acknowledged)references.push_back(b.view());
        return {nonce,session,revision,references};
    }
    Status decode(std::span<const std::uint8_t> bytes,nd::Message& message,nd::ReferenceContext context={}) {
        nd::Arena arena{{reinterpret_cast<std::byte*>(arena_storage.data()),limits.message_bytes},text,channels,planes};
        return nd::decode(std::as_bytes(bytes),message,arena,limits,context).status;
    }
    Status encode(const nd::Message& message,std::uint8_t kind,nd::ReferenceContext context={}) {
        if(wire_bytes)return Status::would_block;
        const auto result=nd::encode(message,wire,scratch,limits,context);if(!result)return result.status;
        wire_bytes=result.produced;wire_offset=0;wire_kind=kind;return Status::ok;
    }
    Status flush(control::numeric::ControlSession& session) {
        if(!wire_bytes)return Status::ok;
        Status result;
        const auto raw=std::span(reinterpret_cast<const std::uint8_t*>(wire.data()),wire_bytes);
        const auto capacity=session.packet_payload_capacity();
        if(capacity<=8)return Status::invalid_argument;
        const auto chunk=std::min(bdsp_protocol::StreamFragmentBytes,capacity-8);
        if(wire_bytes<=capacity)result=session.send_packet(wire_kind,raw);
        else {
            if(wire_kind!=nd::EventsId&&wire_kind!=nd::SnapshotId&&wire_kind!=nd::QueryId)return Status::exhausted;
            auto fragment_result=bdsp_protocol::encode_fragment(raw,wire_offset,fragment,chunk);if(!fragment_result)return fragment_result.status;
            result=session.send_packet(nd::FragmentId,std::span(fragment).first(fragment_result.written_bytes));
            if(result==Status::ok)wire_offset+=std::min(chunk,wire_bytes-wire_offset);
        }
        if(result==Status::ok&&(wire_bytes<=capacity||wire_offset==wire_bytes))wire_bytes=wire_offset=0;
        return result;
    }
    void reset() {wire_bytes=wire_offset=0;assembler.reset();references.clear();}
    // Native is a processing descriptor. DATA's explicit LE serialization does
    // not cast, normalize, change channels/axes or reinterpret semantic meaning.
    Status project(const signal_contracts::NumericView& input,signal_contracts::NumericContract& contract,
        signal_contracts::NumericView& output) {
        using namespace signal_contracts;
        if(!input.contract||input.storage.size()>projection.size())return Status::exhausted;
        contract=std::get<NumericContract>(wire_contract(*input.contract));output=input;output.contract=&contract;
        if(input.contract->representation.encoding!=StorageEncoding::Native)return Status::ok;
        std::memcpy(projection.data(),input.storage.data(),input.storage.size());output.storage=std::span(projection).first(input.storage.size());
        if constexpr(std::endian::native==std::endian::big) {
            MutableNumericView destination{&contract,std::span(projection).first(input.storage.size()),input.planes,input.actual,input.ring,input.coordinates};
            const auto columns=contract.shape.rank==Rank::Matrix?input.actual[1]:1;
            for(std::size_t p=0;p<input.planes.size();++p)for(std::size_t r=0;r<input.actual[0];++r)for(std::size_t c=0;c<columns;++c) {
                std::uint64_t bits;auto s=load_bits(input,{p,r,c},contract.dtype,bits);if(s!=Status::ok)return s;
                s=store_bits(destination,{p,r,c},contract.dtype,bits);if(s!=Status::ok)return s;
            }
        }
        return Status::ok;
    }
    Status restore(nd::Message& message,const Binding& b) {
        using namespace signal_contracts;
        auto original=b.context->contract.view();
        if(message.kind==nd::Kind::HistoryRow) {
            const auto* h=std::get_if<NumericContract>(&original);if(!h)return Status::incompatible_contract;
            const auto history_wire=wire_contract(original);
            if(!same_contract(history_wire,message.history_contract))return Status::incompatible_contract;
            message.history_contract=*h;original=compact_contract(*h);
        }
        const auto expected=wire_contract(original);
        if(!same_contract(expected,message.contract))return Status::incompatible_contract;
        const auto wire_original=message.contract;auto source=message.frame_view();
        if(auto* n=std::get_if<NumericView>(&source.payload))n->contract=std::get_if<NumericContract>(&wire_original);
        message.contract=original;
        const auto* original_numeric=std::get_if<NumericContract>(&original);
        if(!original_numeric||original_numeric->representation.encoding!=StorageEncoding::Native||!message.has_payload())return Status::ok;
        if constexpr(std::endian::native==std::endian::big) {
            const auto* input=std::get_if<NumericView>(&source.payload);if(!input)return Status::incompatible_contract;
            auto storage=std::span(reinterpret_cast<std::byte*>(arena_storage.data()),limits.message_bytes);
            const auto offset=message.numeric.storage.data()-storage.data();
            if(offset<0||std::size_t(offset)>storage.size()||message.numeric.storage.size()>storage.size()-std::size_t(offset))return Status::malformed;
            MutableNumericView destination{original_numeric,storage.subspan(std::size_t(offset),message.numeric.storage.size()),input->planes,input->actual,input->ring,input->coordinates};
            const auto columns=original_numeric->shape.rank==Rank::Matrix?input->actual[1]:1;
            for(std::size_t p=0;p<input->planes.size();++p)for(std::size_t r=0;r<input->actual[0];++r)for(std::size_t c=0;c<columns;++c) {
                std::uint64_t bits;auto s=load_bits(*input,{p,r,c},original_numeric->dtype,bits);if(s!=Status::ok)return s;
                s=store_bits(destination,{p,r,c},original_numeric->dtype,bits);if(s!=Status::ok)return s;
            }
        }
        return Status::ok;
    }
};
using Queued=std::variant<DeliveredNumericFrame,DeliveredNumericHistoryRow>;
enum class EnqueueResult { Appended, Replaced, Overflow };
// Transport queues preserve order. Overflow rejects the new frame and records
// the exact endpoint whose next admitted frame must carry a discontinuity.
using DeliveryLosses=std::set<EndpointIdentity>;
inline EnqueueResult enqueue_delivery(std::deque<Queued>& queue,const Queued& value,DeliveryLosses& losses) {
    const bool ordinary=std::holds_alternative<DeliveredNumericFrame>(value);
    const auto& delivered=ordinary?std::get<DeliveredNumericFrame>(value):std::get<DeliveredNumericHistoryRow>(value).compact;
    if(!delivered.context)return EnqueueResult::Overflow;
    const auto& endpoint=delivered.context->endpoint;
    const auto same_kind=[&](const auto& entry){return std::holds_alternative<DeliveredNumericFrame>(entry)==ordinary;};
    if(std::count_if(queue.begin(),queue.end(),same_kind)>= (ordinary?OrdinaryQueueSize:QueueSize)) {
        losses.insert(endpoint);return EnqueueResult::Overflow;
    }
    auto owned=value;
    if(losses.erase(endpoint)) {
        auto& target=std::holds_alternative<DeliveredNumericFrame>(owned)
            ?std::get<DeliveredNumericFrame>(owned):std::get<DeliveredNumericHistoryRow>(owned).compact;
        if(target.frame)target.frame=target.frame.with_discontinuity();
    }
    queue.push_back(std::move(owned));return EnqueueResult::Appended;
}
inline Status encode_snapshot(CodecStorage& codec,const DeliveredNumericFrame& frame,const Binding& binding,
    std::uint64_t nonce,std::uint64_t session,std::uint64_t revision,std::uint64_t sequence,std::uint64_t request,
    std::uint64_t epoch,std::uint64_t ordinal,std::uint64_t filled_count,std::uint8_t packet_kind=nd::SnapshotId,bool explicit_history=true) {
    using namespace signal_contracts;
    nd::Message m=handshake(binding,nonce,session,revision,epoch);m.envelope.delivery_sequence=sequence;m.envelope.request=request;
    if(explicit_history)if(const auto* c=std::get_if<NumericContract>(&m.contract))m.history={filled_count,c->shape.extents[0].maximum,ordinal};
    if(frame.frame){const auto candidate=frame.frame.view();
        if(const auto* value=std::get_if<NumericView>(&candidate.payload);value && value->ring.capacity)
            m.history={filled_count,value->ring.capacity,ordinal};}
    if(!frame.frame||frame.status!=Status::ok)return codec.encode(m,packet_kind);
    const auto view=frame.frame.view();const auto* input=std::get_if<NumericView>(&view.payload);
    if(!input||!input->contract)return Status::incompatible_contract;
    NumericContract contract;NumericView projected;auto status=codec.project(*input,contract,projected);if(status!=Status::ok)return status;
    m.kind=input->ring.capacity?nd::Kind::Snapshot:nd::Kind::Frame;m.stamp=view.stamp;if(m.stamp.stream_id.empty())m.stamp.stream_id=binding.stream;
    m.envelope.delivery_generation=m.stamp.generation;m.contract=contract;
    m.numeric={projected.storage,projected.planes,projected.actual,projected.ring,projected.coordinates,projected.semantic_silence};
    if(projected.ring.capacity)m.history={filled_count,projected.ring.capacity,ordinal};else m.history={};return codec.encode(m,packet_kind);
}
inline Status retain_snapshot(nd::Message& message,const Binding& binding,CodecStorage& codec,
    const std::shared_ptr<Arena>& arena,DeliveredNumericFrame& out) {
    if(message.kind!=nd::Kind::Snapshot&&message.kind!=nd::Kind::Frame&&message.kind!=nd::Kind::Unavailable)return Status::incompatible_contract;
    auto status=codec.restore(message,binding);if(status!=Status::ok)return status;
    DeliveredNumericFrame value{binding.context,{message.envelope.revision,message.envelope.delivery_epoch},{},
        message.stamp.available?Status::ok:Status::unavailable};
    status=signal_contracts::retain_frame(message.frame_view(),arena->limits,arena->payload_pool,arena->metadata_pool,value.frame);
    if(status==Status::ok)out=std::move(value);
    return status;
}
inline Status encode_delivery(CodecStorage& codec,const Queued& delivery,const Binding& b,
    std::uint64_t nonce,std::uint64_t session,std::uint64_t revision,std::uint64_t sequence,bool gap,std::uint64_t epoch=0) {
    using namespace signal_contracts;
    const auto& frame=std::visit([](const auto& f)->const DeliveredNumericFrame& {
        using T=std::decay_t<decltype(f)>;if constexpr(std::is_same_v<T,DeliveredNumericFrame>)return f;else return f.compact;
    },delivery);
    nd::Message message;message.endpoint=b.view().endpoint;message.contract=b.wire_owner.view();
    message.stamp.available=false;message.stamp.stream_id=b.stream;
    if(frame.frame)message.stamp=frame.frame.view().stamp;
    if(message.stamp.stream_id.empty())message.stamp.stream_id=b.stream;
    message.stamp.discontinuity=message.stamp.discontinuity||gap;
    message.envelope={nonce,session,sequence,epoch?epoch:frame.delivery.epoch,message.stamp.generation,revision,0};
    std::array<std::byte,33> row_metadata{};
    if(const auto* row=std::get_if<DeliveredNumericHistoryRow>(&delivery)) {
        message.history={row->filled_count,row->capacity,row->accepted_row_ordinal};
        switch(row->event) {
        case NumericHistoryEvent::Row:message.kind=nd::Kind::HistoryRow;break;
        case NumericHistoryEvent::Empty:message.kind=nd::Kind::Empty;break;
        case NumericHistoryEvent::Reset:message.kind=nd::Kind::Reset;break;
        case NumericHistoryEvent::Unavailable:message.kind=nd::Kind::Unavailable;break;
        }
        if(row->event==NumericHistoryEvent::Row) {
            auto history=std::get<NumericContract>(message.contract);message.history_contract=history;
            message.contract=compact_contract(history);
            put(row_metadata,0,RowMetadataTag,2);put(row_metadata,2,27,4);put(row_metadata,6,1,1);
            put(row_metadata,7,(row->has_time_coordinate?1:0)|(row->mask_present?2:0)|(row->silenced?4:0),1);
            row_metadata[8]=row->silence_mask;put(row_metadata,9,row->logical_row,8);put(row_metadata,17,row->physical_row,8);
            std::copy(row->time_coordinate.begin(),row->time_coordinate.end(),row_metadata.begin()+25);message.extensions=row_metadata;
        }
    } else message.kind=frame.status==Status::ok&&message.stamp.available?nd::Kind::Frame:nd::Kind::Unavailable;
    NumericContract projected;NumericView payload;
    if(message.kind==nd::Kind::Frame||message.kind==nd::Kind::HistoryRow) {
        if(!frame.frame)return Status::unavailable;
        const auto view=frame.frame.view();
        if(const auto* numeric=std::get_if<NumericView>(&view.payload)) {
            auto s=codec.project(*numeric,projected,payload);if(s!=Status::ok)return s;message.contract=projected;
            message.numeric={payload.storage,payload.planes,payload.actual,payload.ring,payload.coordinates,payload.semantic_silence};
        } else message.structured=std::get<StructuredView>(view.payload).bytes;
    } else message.stamp.available=message.kind==nd::Kind::Empty||message.kind==nd::Kind::Reset;
    return codec.encode(message,nd::EventsId);
}
inline Status retain_delivery(nd::Message& message,const Binding& binding,CodecStorage& codec,
    const std::shared_ptr<Arena>& arena,Queued& output) {
    using namespace signal_contracts;
    auto status=codec.restore(message,binding);if(status!=Status::ok)return status;
    DeliveryStamp stamp{message.envelope.revision,message.envelope.delivery_epoch};
    const auto expected=binding.context->contract.view();const auto* h=std::get_if<NumericContract>(&expected);
    if(h && (message.kind==nd::Kind::HistoryRow || message.kind==nd::Kind::Empty || message.kind==nd::Kind::Reset || (message.kind==nd::Kind::Unavailable && message.history.capacity))) {
        NumericHistoryEvent event;
        switch(message.kind) {
        case nd::Kind::HistoryRow:event=NumericHistoryEvent::Row;break;
        case nd::Kind::Empty:event=NumericHistoryEvent::Empty;break;
        case nd::Kind::Reset:event=NumericHistoryEvent::Reset;break;
        case nd::Kind::Unavailable:event=NumericHistoryEvent::Unavailable;break;
        default:return Status::incompatible_contract;
        }
        NumericGatherOutput compact;compact.frame=message.frame_view();compact.row.history=true;
        // Canonical History rows refer to the full-capacity matrix; occupancy
        // travels separately and cannot substitute for its logical geometry.
        compact.row.filled_rows=message.history.capacity;compact.row.capacity=message.history.capacity;
        compact.row.time_axis=h->coordinates[0];
        if(event==NumericHistoryEvent::Row) {
            const auto e=message.extensions;
            if(e.size()!=33||number({reinterpret_cast<const std::uint8_t*>(e.data()),e.size()},0,2)!=RowMetadataTag||
               number({reinterpret_cast<const std::uint8_t*>(e.data()),e.size()},2,4)!=27||e[6]!=std::byte{1})return Status::malformed;
            const auto flags=std::to_integer<unsigned>(e[7]);if(flags&~7U)return Status::malformed;
            const auto logical=number({reinterpret_cast<const std::uint8_t*>(e.data()),e.size()},9,8);
            const auto physical=number({reinterpret_cast<const std::uint8_t*>(e.data()),e.size()},17,8);
            if(logical>std::numeric_limits<std::size_t>::max()||physical>std::numeric_limits<std::size_t>::max())return Status::exhausted;
            compact.row.logical_row=std::size_t(logical);compact.row.physical_row=std::size_t(physical);
            compact.row.mask_present=bool(flags&2);compact.row.silenced=bool(flags&4);
            if(flags&1)compact.row.time_coordinate=e.subspan(25,8);
            if(flags&2)compact.row.selected_mask=e.subspan(8,1);
        }
        DeliveredNumericHistoryRow retained;
        status=retain_numeric_history_row(binding.context,stamp,event,compact,message.history.filled,message.history.capacity,
            message.history.ordinal,arena->limits,arena->payload_pool,arena->metadata_pool,retained);
        if(status==Status::ok)output=std::move(retained);
        return status;
    }
    if(message.kind!=nd::Kind::Frame&&message.kind!=nd::Kind::Unavailable)return Status::incompatible_contract;
    DeliveredNumericFrame retained;
    status=retain_numeric_delivery(binding.context,stamp,message.frame_view(),message.stamp.available?Status::ok:Status::unavailable,
        arena->limits,arena->payload_pool,arena->metadata_pool,retained);
    if(status==Status::ok)output=std::move(retained);
    return status;
}
}
