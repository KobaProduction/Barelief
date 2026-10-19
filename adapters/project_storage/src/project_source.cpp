#include <project_storage/project_source.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace project_storage {
namespace {
constexpr auto none = std::numeric_limits<std::size_t>::max();
struct TextRange { std::size_t begin = 0, size = 0; };
struct Node {
    NodeKind kind = NodeKind::Null;
    std::size_t begin = 0, end = 0, member_begin = 0;
    TextRange decoded, key;
    std::size_t parent = none, first = none, next = none;
};
bool add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a) return false;
    out = a + b; return true;
}
bool multiply(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (a && b > std::numeric_limits<std::size_t>::max() / a) return false;
    out = a * b; return true;
}
bool charge(std::size_t& used, std::size_t amount, std::size_t limit) noexcept {
    std::size_t total;
    if (!add(used, amount, total) || total > limit) return false;
    used = total; return true;
}
bool valid_limits(const DocumentLimits& l) noexcept {
    const DocumentLimits upper;
    return l.source_bytes <= upper.source_bytes && l.candidate_bytes <= upper.candidate_bytes
        && l.depth <= upper.depth && l.records <= upper.records && l.tokens <= upper.tokens
        && l.number_bytes <= upper.number_bytes && l.key_bytes <= upper.key_bytes
        && l.raw_key_bytes <= upper.raw_key_bytes && l.string_bytes <= upper.string_bytes
        && l.raw_string_bytes <= upper.raw_string_bytes && l.decoded_bytes <= upper.decoded_bytes
        && l.edits <= upper.edits && l.replacement_bytes <= upper.replacement_bytes
        && l.peak_bytes <= upper.peak_bytes;
}
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
int hex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
struct Counts { std::size_t nodes = 0, tokens = 0, decoded = 0; };

// Two passes over identical immutable bytes: grammar/count, then exact-sized
// materialization. No container grows and the counting pass allocates nothing.
class Parser {
public:
    Parser(std::string_view bytes, const DocumentLimits& limits,
           Node* nodes = nullptr, char* decoded = nullptr) noexcept
        : bytes_(bytes), limits_(limits), nodes_(nodes), decoded_(decoded) {}
    Diagnostic run() noexcept {
        whitespace(); value(none, 0); whitespace();
        if (error_ && at_ != bytes_.size()) fail(Error::InvalidJson);
        return error_;
    }
    Diagnostic member_fragment(char* key, std::size_t& size) noexcept {
        whitespace(); TextRange text;
        capture_ = key; string(true, text); capture_ = nullptr; size = text.size;
        whitespace(); punctuation(':'); whitespace(); value(none, 0); whitespace();
        if (error_ && at_ != bytes_.size()) fail(Error::InvalidJson);
        return error_;
    }
    Counts counts() const noexcept { return count_; }
private:
    std::string_view bytes_;
    const DocumentLimits& limits_;
    Node* nodes_;
    char* decoded_;
    char* capture_ = nullptr;
    std::size_t capture_at_ = 0, at_ = 0;
    Counts count_;
    Diagnostic error_;
    char current() const noexcept { return at_ < bytes_.size() ? bytes_[at_] : '\0'; }
    void fail(Error e) noexcept { if (error_) error_ = {e, at_}; }
    void whitespace() noexcept {
        while (at_ < bytes_.size() && (current() == ' ' || current() == '\t'
               || current() == '\r' || current() == '\n')) ++at_;
    }
    void token() noexcept {
        if (!error_) return;
        if (count_.tokens >= limits_.tokens) fail(Error::TokenLimit);
        else ++count_.tokens;
    }
    void punctuation(char wanted) noexcept {
        if (!error_) return;
        if (at_ == bytes_.size() || current() != wanted) { fail(Error::InvalidJson); return; }
        token(); if (error_) ++at_;
    }
    void emit(unsigned char byte) noexcept {
        if (!error_) return;
        if (count_.decoded >= limits_.decoded_bytes) { fail(Error::TextLimit); return; }
        if (capture_ && capture_at_ >= limits_.key_bytes) { fail(Error::TextLimit); return; }
        if (decoded_) decoded_[count_.decoded] = static_cast<char>(byte);
        if (capture_) capture_[capture_at_++] = static_cast<char>(byte);
        ++count_.decoded;
    }
    void codepoint(std::uint32_t cp) noexcept {
        if (cp <= 0x7f) emit(static_cast<unsigned char>(cp));
        else if (cp <= 0x7ff) { emit(static_cast<unsigned char>(0xc0u | (cp >> 6))); emit(0x80u | (cp & 63)); }
        else if (cp <= 0xffff) {
            emit(static_cast<unsigned char>(0xe0u | (cp >> 12))); emit(0x80u | ((cp >> 6) & 63)); emit(0x80u | (cp & 63));
        } else {
            emit(static_cast<unsigned char>(0xf0u | (cp >> 18))); emit(0x80u | ((cp >> 12) & 63));
            emit(0x80u | ((cp >> 6) & 63)); emit(0x80u | (cp & 63));
        }
    }
    bool unicode_unit(std::uint32_t& cp) noexcept {
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_ == bytes_.size() || hex(current()) < 0) { fail(Error::InvalidEscape); return false; }
            cp = cp * 16u + static_cast<unsigned>(hex(current())); ++at_;
        }
        return true;
    }
    static bool utf8_prefix(unsigned lead,unsigned& length,std::uint32_t& cp,
                            std::uint32_t& minimum) noexcept {
        if(lead>=0xc2 && lead<=0xdf){length=2;cp=lead&31;minimum=0x80;return true;}
        if(lead>=0xe0 && lead<=0xef){length=3;cp=lead&15;minimum=0x800;return true;}
        if(lead>=0xf0 && lead<=0xf4){length=4;cp=lead&7;minimum=0x10000;return true;}
        return false;
    }
    void utf8() noexcept {
        const auto begin = at_;
        const auto lead = static_cast<unsigned char>(current());
        unsigned length = 0; std::uint32_t cp = 0, minimum = 0;
        if(!utf8_prefix(lead,length,cp,minimum)){fail(Error::InvalidUtf8);return;}
        if (bytes_.size() - at_ < length) { fail(Error::InvalidUtf8); return; }
        for (unsigned i = 1; i < length; ++i) {
            const auto b = static_cast<unsigned char>(bytes_[begin+i]);
            if ((b & 0xc0) != 0x80) { fail(Error::InvalidUtf8); return; }
            cp = (cp << 6) | (b & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            fail(Error::InvalidUtf8); return;
        }
        for (unsigned i = 0; i < length; ++i) emit(static_cast<unsigned char>(bytes_[begin+i]));
        at_ += length;
    }
    void escaped_unicode() noexcept {
        std::uint32_t cp;
        if(!unicode_unit(cp))return;
        if(cp>=0xd800 && cp<=0xdbff) {
            if(bytes_.size()-at_<2 || bytes_[at_]!='\\' || bytes_[at_+1]!='u') {
                fail(Error::InvalidEscape);return;
            }
            at_+=2;
            std::uint32_t low;
            if(!unicode_unit(low))return;
            if(low<0xdc00 || low>0xdfff){fail(Error::InvalidEscape);return;}
            cp=0x10000u+((cp-0xd800u)<<10)+low-0xdc00u;
        }else if(cp>=0xdc00 && cp<=0xdfff){fail(Error::InvalidEscape);return;}
        codepoint(cp);
    }
    bool escaped_simple(char escape) noexcept {
        switch(escape) {
        case '"':emit('"');return true;case '\\':emit('\\');return true;
        case '/':emit('/');return true;case 'b':emit('\b');return true;
        case 'f':emit('\f');return true;case 'n':emit('\n');return true;
        case 'r':emit('\r');return true;case 't':emit('\t');return true;
        default:return false;
        }
    }
    void escaped_character() noexcept {
        ++at_;
        if(at_==bytes_.size()){fail(Error::InvalidEscape);return;}
        const char escape=current();++at_;
        if(escape=='u'){escaped_unicode();return;}
        if(!escaped_simple(escape))fail(Error::InvalidEscape);
    }
    void string(bool key, TextRange& result) noexcept {
        if (!error_) return;
        if (current() != '"' || at_ == bytes_.size()) { fail(Error::InvalidJson); return; }
        token(); if (!error_) return;
        ++at_; const auto raw_begin = at_;
        result.begin = count_.decoded; capture_at_ = 0;
        const auto raw_limit = key ? limits_.raw_key_bytes : limits_.raw_string_bytes;
        const auto decoded_limit = key ? limits_.key_bytes : limits_.string_bytes;
        while (error_ && at_ < bytes_.size() && current() != '"') {
            const auto b = static_cast<unsigned char>(current());
            if (b < 0x20) { fail(Error::InvalidJson); break; }
            if (b == '\\') escaped_character();
            else if (b >= 0x80) utf8();
            else { emit(b); ++at_; }
            if (at_ - raw_begin > raw_limit || count_.decoded - result.begin > decoded_limit) fail(Error::TextLimit);
        }
        result.size = count_.decoded - result.begin;
        if (!error_) return;
        if (at_ == bytes_.size()) { fail(Error::InvalidJson); return; }
        ++at_;
    }
    void number_fraction() noexcept {
        if(at_==bytes_.size() || current()!='.')return;
        ++at_;
        if(at_==bytes_.size() || !digit(current())){fail(Error::InvalidJson);return;}
        while(at_<bytes_.size() && digit(current()))++at_;
    }
    void number_exponent() noexcept {
        if(at_==bytes_.size() || (current()!='e' && current()!='E'))return;
        ++at_;
        if(at_<bytes_.size() && (current()=='-' || current()=='+'))++at_;
        if(at_==bytes_.size() || !digit(current())){fail(Error::InvalidJson);return;}
        while(at_<bytes_.size() && digit(current()))++at_;
    }
    void number() noexcept {
        const auto begin = at_; token(); if (!error_) return;
        if (current() == '-') ++at_;
        if (at_ == bytes_.size() || !digit(current())) { fail(Error::InvalidJson); return; }
        if (current() == '0') ++at_;
        else while (at_ < bytes_.size() && digit(current())) ++at_;
        number_fraction();
        if(!error_)return;
        number_exponent();
        if(!error_)return;
        if (at_ - begin > limits_.number_bytes) fail(Error::TextLimit);
    }
    void literal(std::string_view wanted) noexcept {
        token(); if (!error_) return;
        if (bytes_.substr(at_, wanted.size()) != wanted) { fail(Error::InvalidJson); return; }
        at_ += wanted.size();
    }
    void append_child(Node& parent,std::size_t child,std::size_t previous,
                      TextRange key,std::size_t member_begin) noexcept {
        if(!nodes_)return;
        nodes_[child].key=key;nodes_[child].member_begin=member_begin;
        if(previous==none)parent.first=child;
        else nodes_[previous].next=child;
    }
    void container_members(Node& node,std::size_t index,std::size_t depth,
                           char opening,char closing) noexcept {
        if(current()==closing)return;
        std::size_t previous=none;
        while(error_) {
            TextRange key;std::size_t member_begin=at_;
            if(opening=='{'){string(true,key);whitespace();punctuation(':');whitespace();}
            const auto child=value(index,depth+1);
            if(!error_)break;
            append_child(node,child,previous,key,member_begin);
            previous=child;whitespace();
            if(current()==closing)break;
            punctuation(',');whitespace();
        }
    }
    void container_value(Node& node,std::size_t index,std::size_t depth,char opening) noexcept {
        node.kind=opening=='{'?NodeKind::Object:NodeKind::Array;
        punctuation(opening);whitespace();
        const char closing=opening=='{'?'}':']';
        container_members(node,index,depth,opening,closing);
        punctuation(closing);
    }
    void scalar_value(Node& node,char lead) noexcept {
        if(lead=='"'){node.kind=NodeKind::String;string(false,node.decoded);}
        else if(lead=='-' || digit(lead)){node.kind=NodeKind::Number;number();}
        else if(lead=='t' || lead=='f'){node.kind=NodeKind::Boolean;literal(lead=='t'?"true":"false");}
        else if(lead=='n'){node.kind=NodeKind::Null;literal("null");}
        else fail(Error::InvalidJson);
    }
    bool value_available() noexcept {
        if(!error_)return false;
        if(at_==bytes_.size()){fail(Error::InvalidJson);return false;}
        if(count_.nodes>=limits_.records){fail(Error::RecordLimit);return false;}
        return true;
    }
    std::size_t value(std::size_t parent, std::size_t depth) noexcept {
        if(!value_available())return none;
        const auto index = count_.nodes++;
        Node scratch; Node& n = nodes_ ? nodes_[index] : scratch;
        n.begin = at_; n.member_begin = at_; n.parent = parent;
        const char c = current();
        if(c=='{' || c=='['){
            if(depth>=limits_.depth){fail(Error::DepthLimit);return index;}
            container_value(n,index,depth,c);
        }
        else scalar_value(n,c);
        n.end = at_; return index;
    }
};
} // namespace

struct OwnedProjectSource::Storage {
    std::unique_ptr<char[]> bytes, decoded;
    std::unique_ptr<Node[]> nodes;
    std::size_t size = 0, count = 0, decoded_size = 0, charge = 0;
    std::uint64_t id = 0;
    NodeHandle handle(std::size_t i) const noexcept { return i == none ? NodeHandle{} : NodeHandle{id,i}; }
    bool valid(NodeHandle h) const noexcept { return h.source == id && h.source != 0 && h.index < count; }
    std::string_view raw(const Node& n) const noexcept { return {bytes.get()+n.begin,n.end-n.begin}; }
    std::string_view text(TextRange r) const noexcept { return r.size ? std::string_view(decoded.get()+r.begin,r.size) : std::string_view{}; }
};
struct SourceAccess {
    using Storage = OwnedProjectSource::Storage;
    static const Storage* get(const OwnedProjectSource& s) noexcept { return s.storage_.get(); }
    static void publish(OwnedProjectSource& s, std::unique_ptr<Storage> p) noexcept { s.storage_ = std::move(p); }
};
namespace {
using Storage = SourceAccess::Storage;
std::atomic<std::uint64_t> next_id{1};
std::uint64_t new_id() noexcept {
    auto id = next_id.load(std::memory_order_relaxed);
    while (id != std::numeric_limits<std::uint64_t>::max()) {
        if (next_id.compare_exchange_weak(id,id+1,std::memory_order_relaxed)) return id;
    }
    return 0; // Never wrap and accidentally reuse a provenance identity.
}
bool materialization_charge(std::string_view input,Counts counts,std::size_t& resident,
                            std::size_t& own,const DocumentLimits& limits) noexcept {
    std::size_t node_bytes=0;
    own=sizeof(Storage);
    return multiply(counts.nodes,sizeof(Node),node_bytes) &&
        charge(own,input.size(),limits.peak_bytes) &&
        charge(own,node_bytes,limits.peak_bytes) &&
        charge(own,counts.decoded,limits.peak_bytes) &&
        charge(resident,own,limits.peak_bytes);
}
Diagnostic allocate_materialization(Storage& storage,std::string_view input,Counts counts,
                                    std::unique_ptr<char[]> owned,std::size_t own) noexcept {
    storage.bytes=std::move(owned);
    if(!storage.bytes && !input.empty())storage.bytes.reset(new(std::nothrow) char[input.size()]);
    storage.nodes.reset(new(std::nothrow) Node[counts.nodes]);
    if(counts.decoded)storage.decoded.reset(new(std::nothrow) char[counts.decoded]);
    if((!input.empty() && !storage.bytes) || !storage.nodes ||
       (counts.decoded && !storage.decoded))return {Error::AllocationFailure};
    if(storage.bytes.get()!=input.data())std::memcpy(storage.bytes.get(),input.data(),input.size());
    storage.size=input.size();storage.count=counts.nodes;
    storage.decoded_size=counts.decoded;storage.charge=own;storage.id=new_id();
    if(!storage.id)return {Error::InvalidHandle};
    return {};
}
Diagnostic materialize(std::string_view input, Counts counts, std::size_t resident,
                       std::unique_ptr<char[]> owned, OwnedProjectSource& out,
                       const DocumentLimits& limits) noexcept {
    std::size_t own=0;
    if(!materialization_charge(input,counts,resident,own,limits))return {Error::PeakLimit};
    std::unique_ptr<Storage> storage(new (std::nothrow) Storage);
    if (!storage) return {Error::AllocationFailure};
    const auto allocated=allocate_materialization(*storage,input,counts,std::move(owned),own);
    if(!allocated)return allocated;
    Parser parser({storage->bytes.get(),storage->size},limits,storage->nodes.get(),storage->decoded.get());
    auto result = parser.run(); if (!result) return result;
    SourceAccess::publish(out,std::move(storage)); return {};
}
Diagnostic object(const Storage* s, NodeHandle h) noexcept {
    if (!s || !s->valid(h)) return {Error::InvalidHandle};
    if (s->nodes[h.index].kind != NodeKind::Object) return {Error::InvalidHandle,s->nodes[h.index].begin};
    return {};
}
bool ambiguous(const Storage& s, std::size_t child) noexcept {
    const auto& n = s.nodes[child];
    if (n.parent == none || s.nodes[n.parent].kind != NodeKind::Object) return false;
    const auto key = s.text(n.key);
    for (auto i = s.nodes[n.parent].first; i != none; i = s.nodes[i].next)
        if (i != child && s.text(s.nodes[i].key) == key) return true;
    return false;
}
bool ambiguous_path(const Storage& s, std::size_t index) noexcept {
    for (auto i = index; i != none; i = s.nodes[i].parent) if (ambiguous(s,i)) return true;
    return false;
}
bool parse_integer_digits(std::string_view raw,std::uint64_t max,std::uint64_t& value) noexcept {
    value=0;
    for(char c:raw) {
        if(!digit(c) || value>(max-static_cast<unsigned>(c-'0'))/10u)return false;
        value=value*10u+static_cast<unsigned>(c-'0');
    }
    return true;
}
Diagnostic integer(const OwnedProjectSource& source, NodeHandle h, bool signed_type,
                   std::uint64_t& magnitude, bool& negative) noexcept {
    NodeView v; auto d = source.view(h,v); if (!d) return d;
    if (v.kind != NodeKind::Number) return {Error::IntegerRange,v.offset};
    auto raw = v.raw; negative = raw[0] == '-';
    if (negative) raw.remove_prefix(1);
    if ((negative && (!signed_type || raw == "0")) || raw.empty()) return {Error::IntegerRange,v.offset};
    const auto max = signed_type ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u)
                                 : std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value=0;
    if(!parse_integer_digits(raw,max,value))return {Error::IntegerRange,v.offset};
    magnitude = value; return {};
}
} // namespace

OwnedProjectSource::OwnedProjectSource() noexcept = default;
OwnedProjectSource::~OwnedProjectSource() = default;
OwnedProjectSource::OwnedProjectSource(OwnedProjectSource&&) noexcept = default;
OwnedProjectSource& OwnedProjectSource::operator=(OwnedProjectSource&&) noexcept = default;
std::string_view OwnedProjectSource::bytes() const noexcept { return storage_ ? std::string_view(storage_->bytes.get(),storage_->size) : std::string_view{}; }
NodeHandle OwnedProjectSource::root() const noexcept { return storage_ ? storage_->handle(0) : NodeHandle{}; }
std::size_t OwnedProjectSource::node_count() const noexcept { return storage_ ? storage_->count : 0; }
std::size_t OwnedProjectSource::owned_bytes() const noexcept { return storage_ ? storage_->charge : 0; }
Diagnostic OwnedProjectSource::view(NodeHandle h, NodeView& out) const noexcept {
    if (!storage_ || !storage_->valid(h)) return {Error::InvalidHandle};
    const auto& n = storage_->nodes[h.index];
    out = {n.kind,storage_->raw(n),storage_->text(n.decoded),storage_->text(n.key),n.begin,
           storage_->handle(n.parent),storage_->handle(n.first),storage_->handle(n.next)};
    return {};
}
Diagnostic parse_project_source(std::string_view input, OwnedProjectSource& out, const DocumentLimits& l) noexcept {
    if (!valid_limits(l)) return {Error::InvalidLimits};
    if (input.size() > l.source_bytes) return {Error::SourceLimit};
    Parser parser(input,l); auto result = parser.run(); if (!result) return result;
    return materialize(input,parser.counts(),out.owned_bytes(),{},out,l);
}
static Diagnostic unique_member_index(const Storage& storage,NodeHandle h,std::string_view key,
                                      std::size_t& found) noexcept {
    const auto* s=&storage;
    found=none;
    for (auto i = s->nodes[h.index].first; i != none; i = s->nodes[i].next) {
        if (s->text(s->nodes[i].key) != key) continue;
        if (found != none) return {Error::AmbiguousKey,s->nodes[i].member_begin};
        found = i;
    }
    if (found == none) return {Error::MissingMember,s->nodes[h.index].begin};
    return {};
}
Diagnostic find_unique_member(const OwnedProjectSource& source, NodeHandle h, std::string_view key, NodeHandle& out) noexcept {
    const auto* s=SourceAccess::get(source);
    const auto valid=object(s,h);if(!valid)return valid;
    std::size_t found=none;
    const auto unique=unique_member_index(*s,h,key,found);
    if(!unique)return unique;
    out=s->handle(found);return {};
}
Diagnostic validate_known_keys(const OwnedProjectSource& source, NodeHandle h, std::span<const std::string_view> keys) noexcept {
    const auto* s = SourceAccess::get(source); auto d = object(s,h); if (!d) return d;
    for (auto key : keys) {
        NodeHandle found; d = find_unique_member(source,h,key,found);
        if (d.code == Error::AmbiguousKey) return {Error::DuplicateKnownKey,d.offset};
        if (!d && d.code != Error::MissingMember) return d;
    }
    return {};
}
Diagnostic read_int64(const OwnedProjectSource& s, NodeHandle h, std::int64_t& out) noexcept {
    std::uint64_t value = 0; bool negative = false; auto d = integer(s,h,true,value,negative); if (!d) return d;
    if (negative && value == (std::uint64_t{1} << 63)) out = std::numeric_limits<std::int64_t>::min();
    else out = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    return {};
}
Diagnostic read_uint64(const OwnedProjectSource& s, NodeHandle h, std::uint64_t& out) noexcept {
    std::uint64_t value = 0; bool negative = false; auto d = integer(s,h,false,value,negative); if (!d) return d;
    out = value; return {};
}

namespace {
struct NodePlan {
    std::size_t operation = none;
    std::size_t before_first = none, before_last = none, append_first = none, append_last = none;
    bool changed = false;
};
struct EditPlan {
    std::size_t next = none, key_size = 0;
    char key[1024]{};
};
bool is_container(NodeKind k) noexcept { return k == NodeKind::Object || k == NodeKind::Array; }
bool ancestor(const Storage& s, std::size_t ancestor_index, std::size_t child) noexcept {
    for (auto i = child; i != none; i = s.nodes[i].parent) if (i == ancestor_index) return true;
    return false;
}
void mark(const Storage& s, NodePlan* plan, std::size_t index) noexcept {
    for (auto i = index; i != none; i = s.nodes[i].parent) plan[i].changed = true;
}
void chain(EditPlan* edits, std::size_t index, std::size_t& first, std::size_t& last) noexcept {
    if (last == none) first = index; else edits[last].next = index;
    last = index;
}
// Only changed containers are rebuilt; retained child/member slices bypass all
// numeric/string serialization. Renderer first counts, then writes exact size.
class Renderer {
public:
    Renderer(const Storage& source, const NodePlan* nodes, const EditPlan* plans,
             std::span<const DocumentEdit> edits, std::size_t limit, char* output = nullptr) noexcept
        : source_(source), nodes_(nodes), plans_(plans), edits_(edits), limit_(limit), output_(output) {}
    Diagnostic run() noexcept {
        const auto& root = source_.nodes[0];
        emit({source_.bytes.get(),root.begin}); render(0);
        emit({source_.bytes.get()+root.end,source_.size-root.end}); return error_;
    }
    std::size_t size() const noexcept { return at_; }
private:
    const Storage& source_; const NodePlan* nodes_; const EditPlan* plans_;
    std::span<const DocumentEdit> edits_; std::size_t limit_, at_ = 0;
    char* output_; Diagnostic error_;
    void emit(std::string_view bytes) noexcept {
        if (!error_) return;
        std::size_t end;
        if (!add(at_,bytes.size(),end) || end > limit_) { error_ = {Error::CandidateLimit,at_}; return; }
        if (output_ && !bytes.empty()) std::memcpy(output_+at_,bytes.data(),bytes.size());
        at_ = end;
    }
    void separator(bool& first) noexcept { if (!first) emit(","); first = false; }
    void insertions(std::size_t first_edit, bool& first) noexcept {
        for (auto i = first_edit; i != none && error_; i = plans_[i].next) {
            separator(first); const auto& edit = edits_[i];
            if (edit.kind == EditKind::Insert) emit(edit.replacement);
            else {
                const auto& n = source_.nodes[edit.target.index];
                emit({source_.bytes.get()+n.member_begin,n.end-n.member_begin});
            }
        }
    }
    void render_child(std::size_t child,NodeKind parent_kind,bool& first) noexcept {
        insertions(nodes_[child].before_first,first);
        const auto operation=nodes_[child].operation;
        if(operation!=none && (edits_[operation].kind==EditKind::Remove ||
                               edits_[operation].kind==EditKind::Move))return;
        separator(first);
        const auto& node=source_.nodes[child];
        if(parent_kind==NodeKind::Object)
            emit({source_.bytes.get()+node.member_begin,node.begin-node.member_begin});
        render(child);
    }
    void render_children(const Node& parent,bool& first) noexcept {
        for(auto child=parent.first;child!=none && error_;child=source_.nodes[child].next)
            render_child(child,parent.kind,first);
    }
    void render(std::size_t index) noexcept {
        if (!error_) return;
        const auto& n = source_.nodes[index]; const auto& p = nodes_[index];
        if (p.operation != none && edits_[p.operation].kind == EditKind::Replace) { emit(edits_[p.operation].replacement); return; }
        if (!p.changed || !is_container(n.kind)) { emit(source_.raw(n)); return; }
        emit(n.kind == NodeKind::Object ? "{" : "["); bool first = true;
        render_children(n,first);
        insertions(p.append_first,first); emit(n.kind == NodeKind::Object ? "}" : "]");
    }
};
Diagnostic stage_budget(const Storage& source,const OwnedProjectSource& original,
                        const OwnedProjectSource& output,std::span<const DocumentEdit> edits,
                        const DocumentLimits& limits,std::size_t& resident,
                        std::size_t& replacement_bytes) noexcept {
    replacement_bytes=0;
    for(const auto& edit:edits)
        if(!charge(replacement_bytes,edit.replacement.size(),limits.replacement_bytes))
            return {Error::EditLimit};
    resident=original.owned_bytes();
    if(&original!=&output && !charge(resident,output.owned_bytes(),limits.peak_bytes))
        return {Error::PeakLimit};
    std::size_t plan_bytes=0,edit_bytes=0,borrowed_edit_bytes=0;
    if(!multiply(source.count,sizeof(NodePlan),plan_bytes) ||
       !multiply(edits.size(),sizeof(EditPlan),edit_bytes) ||
       !multiply(edits.size(),sizeof(DocumentEdit),borrowed_edit_bytes) ||
       !charge(resident,plan_bytes,limits.peak_bytes) ||
       !charge(resident,edit_bytes,limits.peak_bytes) ||
       !charge(resident,borrowed_edit_bytes,limits.peak_bytes) ||
       !charge(resident,replacement_bytes,limits.peak_bytes))return {Error::PeakLimit};
    return {};
}
Diagnostic prepare_edit_stage(const OwnedProjectSource& source,const OwnedProjectSource& output,
    std::span<const DocumentEdit> edits,const DocumentLimits& limits,const Storage*& storage,
    std::size_t& resident,std::unique_ptr<NodePlan[]>& plan,
    std::unique_ptr<EditPlan[]>& staged) noexcept {
    if(!valid_limits(limits))return {Error::InvalidLimits};
    storage=SourceAccess::get(source);
    if(!storage)return {Error::InvalidHandle};
    if(edits.size()>limits.edits)return {Error::EditLimit};
    std::size_t replacement_bytes=0;
    const auto budget=stage_budget(*storage,source,output,edits,limits,resident,replacement_bytes);
    if(!budget)return budget;
    plan.reset(new(std::nothrow) NodePlan[storage->count]);
    if(!edits.empty())staged.reset(new(std::nothrow) EditPlan[edits.size()]);
    if(!plan || (!edits.empty() && !staged))return {Error::AllocationFailure};
    return {};
}
struct EditStage {
    const Storage& source;
    std::span<const DocumentEdit> edits;
    const DocumentLimits& limits;
    NodePlan* plan;
    EditPlan* staged;

    Diagnostic target(std::size_t index) noexcept {
        const auto& edit=edits[index];
        if((!edit.target.source && edit.target.index) ||
           (!edit.parent.source && edit.parent.index) ||
           (!edit.before.source && edit.before.index))return {Error::InvalidHandle};
        if(edit.kind!=EditKind::Replace && edit.kind!=EditKind::Remove &&
           edit.kind!=EditKind::Insert && edit.kind!=EditKind::Move)return {Error::InvalidEdit};
        if(edit.kind==EditKind::Insert)
            return edit.target.source?Diagnostic{Error::InvalidEdit}:Diagnostic{};
        if(!source.valid(edit.target))return {Error::InvalidHandle};
        const auto& node=source.nodes[edit.target.index];
        if(source.raw(node)!=edit.expected)return {Error::ExpectedMismatch,node.begin};
        if(ambiguous_path(source,edit.target.index))return {Error::AmbiguousKey,node.member_begin};
        if(plan[edit.target.index].operation!=none)return {Error::ConflictingEdits,node.begin};
        if((edit.kind==EditKind::Remove || edit.kind==EditKind::Move) &&
           node.parent==none)return {Error::InvalidEdit,node.begin};
        plan[edit.target.index].operation=index;
        mark(source,plan,edit.target.index);
        return {};
    }
    Diagnostic replacement(std::size_t index) noexcept {
        const auto& edit=edits[index];
        if(edit.parent.source || edit.before.source)return {Error::InvalidEdit};
        Parser parser(edit.replacement,limits);
        return parser.run();
    }
    Diagnostic removal(std::size_t index) const noexcept {
        const auto& edit=edits[index];
        return edit.parent.source || edit.before.source || !edit.replacement.empty()?
            Diagnostic{Error::InvalidEdit}:Diagnostic{};
    }
    Diagnostic placement_parent(std::size_t index) const noexcept {
        const auto& edit=edits[index];
        if(!source.valid(edit.parent))return {Error::InvalidHandle};
        const auto& parent=source.nodes[edit.parent.index];
        if(!is_container(parent.kind))return {Error::InvalidEdit,parent.begin};
        if(ambiguous_path(source,edit.parent.index))return {Error::AmbiguousKey,parent.member_begin};
        if(edit.before.source && (!source.valid(edit.before) ||
           source.nodes[edit.before.index].parent!=edit.parent.index))return {Error::InvalidHandle};
        if(edit.before.source && ambiguous_path(source,edit.before.index))
            return {Error::AmbiguousKey,source.nodes[edit.before.index].member_begin};
        return {};
    }
    Diagnostic insertion(std::size_t index) noexcept {
        const auto& edit=edits[index];
        const auto& parent=source.nodes[edit.parent.index];
        if(source.raw(parent)!=edit.expected)return {Error::ExpectedMismatch,parent.begin};
        Parser parser(edit.replacement,limits);
        if(parent.kind==NodeKind::Object)
            return parser.member_fragment(staged[index].key,staged[index].key_size);
        return parser.run();
    }
    Diagnostic move(std::size_t index) noexcept {
        const auto& edit=edits[index];
        const auto& parent=source.nodes[edit.parent.index];
        if(!edit.replacement.empty())return {Error::InvalidEdit};
        const auto& node=source.nodes[edit.target.index];
        if(source.nodes[node.parent].kind!=parent.kind ||
           ancestor(source,edit.target.index,edit.parent.index))
            return {Error::InvalidEdit,parent.begin};
        const auto key=source.text(node.key);
        staged[index].key_size=key.size();
        if(!key.empty())std::memcpy(staged[index].key,key.data(),key.size());
        return {};
    }
    Diagnostic placement(std::size_t index) noexcept {
        const auto parent=placement_parent(index);
        if(!parent)return parent;
        const auto& edit=edits[index];
        const auto content=edit.kind==EditKind::Insert?insertion(index):move(index);
        if(!content)return content;
        mark(source,plan,edit.parent.index);
        if(edit.before.source)
            chain(staged,index,plan[edit.before.index].before_first,plan[edit.before.index].before_last);
        else chain(staged,index,plan[edit.parent.index].append_first,plan[edit.parent.index].append_last);
        return {};
    }
    Diagnostic one(std::size_t index) noexcept {
        const auto valid=target(index);
        if(!valid)return valid;
        const auto kind=edits[index].kind;
        if(kind==EditKind::Replace)return replacement(index);
        if(kind==EditKind::Remove)return removal(index);
        return placement(index);
    }
    bool ancestors_clear(std::size_t index) const noexcept {
        for(auto ancestor=index;ancestor!=none;ancestor=source.nodes[ancestor].parent)
            if(plan[ancestor].operation!=none &&
               edits[plan[ancestor].operation].kind!=EditKind::Insert)return false;
        return true;
    }
    Diagnostic conflict(std::size_t index) const noexcept {
        const auto& edit=edits[index];
        if(edit.kind!=EditKind::Insert &&
           !ancestors_clear(source.nodes[edit.target.index].parent))
            return {Error::ConflictingEdits,source.nodes[edit.target.index].begin};
        if(edit.kind!=EditKind::Insert && edit.kind!=EditKind::Move)return {};
        if(!ancestors_clear(edit.parent.index))
            return {Error::ConflictingEdits,source.nodes[edit.parent.index].begin};
        if(edit.before.source && plan[edit.before.index].operation!=none &&
           edits[plan[edit.before.index].operation].kind!=EditKind::Replace)
            return {Error::ConflictingEdits,source.nodes[edit.before.index].begin};
        return {};
    }
    Diagnostic duplicate_existing(std::size_t index,std::string_view key) const noexcept {
        const auto& edit=edits[index];
        for(auto child=source.nodes[edit.parent.index].first;child!=none;
            child=source.nodes[child].next) {
            const auto operation=plan[child].operation;
            if(operation!=none && (edits[operation].kind==EditKind::Remove ||
                                   edits[operation].kind==EditKind::Move))continue;
            if(source.text(source.nodes[child].key)==key)
                return {Error::AmbiguousKey,source.nodes[child].member_begin};
        }
        return {};
    }
    Diagnostic duplicate_planned(std::size_t index,std::string_view key) const noexcept {
        const auto& edit=edits[index];
        for(std::size_t previous=0;previous<index;++previous) {
            const auto& prior=edits[previous];
            if((prior.kind==EditKind::Insert || prior.kind==EditKind::Move) &&
               prior.parent==edit.parent &&
               std::string_view(staged[previous].key,staged[previous].key_size)==key)
                return {Error::AmbiguousKey,source.nodes[edit.parent.index].begin};
        }
        return {};
    }
    Diagnostic duplicate(std::size_t index) const noexcept {
        const auto& edit=edits[index];
        if((edit.kind!=EditKind::Insert && edit.kind!=EditKind::Move) ||
           source.nodes[edit.parent.index].kind!=NodeKind::Object)return {};
        const std::string_view key(staged[index].key,staged[index].key_size);
        const auto existing=duplicate_existing(index,key);
        return existing?duplicate_planned(index,key):existing;
    }
    Diagnostic validate_all() noexcept {
        for(std::size_t index=0;index<edits.size();++index) {
            const auto result=one(index);
            if(!result)return result;
        }
        return {};
    }
    Diagnostic conflicts_all() const noexcept {
        for(std::size_t index=0;index<edits.size();++index) {
            const auto result=conflict(index);
            if(!result)return result;
        }
        return {};
    }
    Diagnostic duplicates_all() const noexcept {
        for(std::size_t index=0;index<edits.size();++index) {
            const auto result=duplicate(index);
            if(!result)return result;
        }
        return {};
    }
};
Diagnostic render_candidate(const Storage& source,const NodePlan* plan,const EditPlan* staged,
                            std::span<const DocumentEdit> edits,const DocumentLimits& limits,
                            std::size_t resident,OwnedProjectSource& output) noexcept {
    Renderer count(source,plan,staged,edits,limits.candidate_bytes);
    auto result=count.run();if(!result)return result;
    std::size_t candidate_charge=resident;
    if(!charge(candidate_charge,count.size(),limits.peak_bytes))return {Error::PeakLimit};
    std::unique_ptr<char[]> bytes(new(std::nothrow) char[count.size()]);
    if(!bytes)return {Error::AllocationFailure};
    Renderer write(source,plan,staged,edits,limits.candidate_bytes,bytes.get());
    result=write.run();if(!result)return result;
    const std::string_view candidate(bytes.get(),count.size());
    Parser parser(candidate,limits);
    result=parser.run();if(!result)return result;
    return materialize(candidate,parser.counts(),resident,std::move(bytes),output,limits);
}
} // namespace

Diagnostic stage_project_edits(const OwnedProjectSource& source,std::span<const DocumentEdit> edits,
                               OwnedProjectSource& out,const DocumentLimits& limits) noexcept {
    const Storage* storage=nullptr;
    std::size_t resident=0;
    std::unique_ptr<NodePlan[]> plan;
    std::unique_ptr<EditPlan[]> staged;
    const auto prepared=prepare_edit_stage(source,out,edits,limits,storage,resident,plan,staged);
    if(!prepared)return prepared;
    EditStage stage{*storage,edits,limits,plan.get(),staged.get()};
    const auto valid=stage.validate_all();if(!valid)return valid;
    const auto conflicts=stage.conflicts_all();if(!conflicts)return conflicts;
    const auto duplicates=stage.duplicates_all();if(!duplicates)return duplicates;
    return render_candidate(*storage,plan.get(),staged.get(),edits,limits,resident,out);
}
} // namespace project_storage
