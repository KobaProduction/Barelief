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
    void utf8() noexcept {
        const auto begin = at_;
        const auto lead = static_cast<unsigned char>(current());
        unsigned length = 0; std::uint32_t cp = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { length = 2; cp = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { length = 3; cp = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { length = 4; cp = lead & 7; minimum = 0x10000; }
        else { fail(Error::InvalidUtf8); return; }
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
            if (b == '\\') {
                ++at_; if (at_ == bytes_.size()) { fail(Error::InvalidEscape); break; }
                const char escape = current(); ++at_;
                switch (escape) {
                case '"': emit('"'); break; case '\\': emit('\\'); break; case '/': emit('/'); break;
                case 'b': emit('\b'); break; case 'f': emit('\f'); break; case 'n': emit('\n'); break;
                case 'r': emit('\r'); break; case 't': emit('\t'); break;
                case 'u': {
                    std::uint32_t cp; if (!unicode_unit(cp)) break;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (bytes_.size() - at_ < 2 || bytes_[at_] != '\\' || bytes_[at_+1] != 'u') { fail(Error::InvalidEscape); break; }
                        at_ += 2; std::uint32_t low;
                        if (!unicode_unit(low)) break;
                        if (low < 0xdc00 || low > 0xdfff) { fail(Error::InvalidEscape); break; }
                        cp = 0x10000u + ((cp-0xd800u) << 10) + low-0xdc00u;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) { fail(Error::InvalidEscape); break; }
                    codepoint(cp); break;
                }
                default: fail(Error::InvalidEscape); break;
                }
            } else if (b >= 0x80) utf8();
            else { emit(b); ++at_; }
            if (at_ - raw_begin > raw_limit || count_.decoded - result.begin > decoded_limit) fail(Error::TextLimit);
        }
        result.size = count_.decoded - result.begin;
        if (!error_) return;
        if (at_ == bytes_.size()) { fail(Error::InvalidJson); return; }
        ++at_;
    }
    void number() noexcept {
        const auto begin = at_; token(); if (!error_) return;
        if (current() == '-') ++at_;
        if (at_ == bytes_.size() || !digit(current())) { fail(Error::InvalidJson); return; }
        if (current() == '0') ++at_;
        else while (at_ < bytes_.size() && digit(current())) ++at_;
        if (at_ < bytes_.size() && current() == '.') {
            ++at_; if (at_ == bytes_.size() || !digit(current())) { fail(Error::InvalidJson); return; }
            while (at_ < bytes_.size() && digit(current())) ++at_;
        }
        if (at_ < bytes_.size() && (current() == 'e' || current() == 'E')) {
            ++at_; if (at_ < bytes_.size() && (current() == '-' || current() == '+')) ++at_;
            if (at_ == bytes_.size() || !digit(current())) { fail(Error::InvalidJson); return; }
            while (at_ < bytes_.size() && digit(current())) ++at_;
        }
        if (at_ - begin > limits_.number_bytes) fail(Error::TextLimit);
    }
    void literal(std::string_view wanted) noexcept {
        token(); if (!error_) return;
        if (bytes_.substr(at_, wanted.size()) != wanted) { fail(Error::InvalidJson); return; }
        at_ += wanted.size();
    }
    std::size_t value(std::size_t parent, std::size_t depth) noexcept {
        if (!error_) return none;
        if (at_ == bytes_.size()) { fail(Error::InvalidJson); return none; }
        if (count_.nodes >= limits_.records) { fail(Error::RecordLimit); return none; }
        const auto index = count_.nodes++;
        Node scratch; Node& n = nodes_ ? nodes_[index] : scratch;
        n.begin = at_; n.member_begin = at_; n.parent = parent;
        const char c = current();
        if (c == '{' || c == '[') {
            if (depth >= limits_.depth) { fail(Error::DepthLimit); return index; }
            n.kind = c == '{' ? NodeKind::Object : NodeKind::Array;
            punctuation(c); whitespace();
            const char closing = c == '{' ? '}' : ']';
            std::size_t previous = none;
            if (current() != closing) {
                while (error_) {
                    TextRange key; std::size_t member_begin = at_;
                    if (c == '{') { string(true,key); whitespace(); punctuation(':'); whitespace(); }
                    auto child = value(index,depth+1);
                    if (!error_) break;
                    if (nodes_) {
                        nodes_[child].key = key; nodes_[child].member_begin = member_begin;
                        if (previous == none) n.first = child; else nodes_[previous].next = child;
                    }
                    previous = child; whitespace();
                    if (current() == closing) break;
                    punctuation(','); whitespace();
                }
            }
            punctuation(closing);
        } else if (c == '"') { n.kind = NodeKind::String; string(false,n.decoded); }
        else if (c == '-' || digit(c)) { n.kind = NodeKind::Number; number(); }
        else if (c == 't' || c == 'f') { n.kind = NodeKind::Boolean; literal(c == 't' ? "true" : "false"); }
        else if (c == 'n') { n.kind = NodeKind::Null; literal("null"); }
        else fail(Error::InvalidJson);
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
Diagnostic materialize(std::string_view input, Counts counts, std::size_t resident,
                       std::unique_ptr<char[]> owned, OwnedProjectSource& out,
                       const DocumentLimits& limits) noexcept {
    std::size_t node_bytes, own = sizeof(Storage);
    if (!multiply(counts.nodes,sizeof(Node),node_bytes) || !charge(own,input.size(),limits.peak_bytes)
        || !charge(own,node_bytes,limits.peak_bytes) || !charge(own,counts.decoded,limits.peak_bytes)
        || !charge(resident,own,limits.peak_bytes)) return {Error::PeakLimit};
    std::unique_ptr<Storage> storage(new (std::nothrow) Storage);
    if (!storage) return {Error::AllocationFailure};
    storage->bytes = std::move(owned);
    if (!storage->bytes && !input.empty()) storage->bytes.reset(new (std::nothrow) char[input.size()]);
    storage->nodes.reset(new (std::nothrow) Node[counts.nodes]);
    if (counts.decoded) storage->decoded.reset(new (std::nothrow) char[counts.decoded]);
    if ((!input.empty() && !storage->bytes) || !storage->nodes || (counts.decoded && !storage->decoded)) return {Error::AllocationFailure};
    if (storage->bytes.get() != input.data()) std::memcpy(storage->bytes.get(),input.data(),input.size());
    storage->size = input.size(); storage->count = counts.nodes;
    storage->decoded_size = counts.decoded; storage->charge = own; storage->id = new_id();
    if (!storage->id) return {Error::InvalidHandle};
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
Diagnostic integer(const OwnedProjectSource& source, NodeHandle h, bool signed_type,
                   std::uint64_t& magnitude, bool& negative) noexcept {
    NodeView v; auto d = source.view(h,v); if (!d) return d;
    if (v.kind != NodeKind::Number) return {Error::IntegerRange,v.offset};
    auto raw = v.raw; negative = raw[0] == '-';
    if (negative) raw.remove_prefix(1);
    if ((negative && (!signed_type || raw == "0")) || raw.empty()) return {Error::IntegerRange,v.offset};
    const auto max = signed_type ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u)
                                 : std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value = 0;
    for (char c : raw) {
        if (!digit(c) || value > (max-static_cast<unsigned>(c-'0'))/10u) return {Error::IntegerRange,v.offset};
        value = value*10u + static_cast<unsigned>(c-'0');
    }
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
Diagnostic find_unique_member(const OwnedProjectSource& source, NodeHandle h, std::string_view key, NodeHandle& out) noexcept {
    const auto* s = SourceAccess::get(source); auto d = object(s,h); if (!d) return d;
    auto found = none;
    for (auto i = s->nodes[h.index].first; i != none; i = s->nodes[i].next) {
        if (s->text(s->nodes[i].key) != key) continue;
        if (found != none) return {Error::AmbiguousKey,s->nodes[i].member_begin};
        found = i;
    }
    if (found == none) return {Error::MissingMember,s->nodes[h.index].begin};
    out = s->handle(found); return {};
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
    void render(std::size_t index) noexcept {
        if (!error_) return;
        const auto& n = source_.nodes[index]; const auto& p = nodes_[index];
        if (p.operation != none && edits_[p.operation].kind == EditKind::Replace) { emit(edits_[p.operation].replacement); return; }
        if (!p.changed || !is_container(n.kind)) { emit(source_.raw(n)); return; }
        emit(n.kind == NodeKind::Object ? "{" : "["); bool first = true;
        for (auto child = n.first; child != none && error_; child = source_.nodes[child].next) {
            insertions(nodes_[child].before_first,first);
            auto operation = nodes_[child].operation;
            if (operation != none && (edits_[operation].kind == EditKind::Remove || edits_[operation].kind == EditKind::Move)) continue;
            separator(first); const auto& c = source_.nodes[child];
            if (n.kind == NodeKind::Object) emit({source_.bytes.get()+c.member_begin,c.begin-c.member_begin});
            render(child);
        }
        insertions(p.append_first,first); emit(n.kind == NodeKind::Object ? "}" : "]");
    }
};
} // namespace

Diagnostic stage_project_edits(const OwnedProjectSource& source, std::span<const DocumentEdit> edits,
                               OwnedProjectSource& out, const DocumentLimits& l) noexcept {
    if (!valid_limits(l)) return {Error::InvalidLimits};
    const auto* s = SourceAccess::get(source); if (!s) return {Error::InvalidHandle};
    if (edits.size() > l.edits) return {Error::EditLimit};
    std::size_t replacement_bytes = 0;
    for (const auto& e : edits) if (!charge(replacement_bytes,e.replacement.size(),l.replacement_bytes)) return {Error::EditLimit};
    std::size_t resident = source.owned_bytes();
    if (&source != &out && !charge(resident,out.owned_bytes(),l.peak_bytes)) return {Error::PeakLimit};
    std::size_t plan_bytes, edit_bytes, borrowed_edit_bytes;
    if (!multiply(s->count,sizeof(NodePlan),plan_bytes) || !multiply(edits.size(),sizeof(EditPlan),edit_bytes)
        || !multiply(edits.size(),sizeof(DocumentEdit),borrowed_edit_bytes)
        || !charge(resident,plan_bytes,l.peak_bytes) || !charge(resident,edit_bytes,l.peak_bytes)
        || !charge(resident,borrowed_edit_bytes,l.peak_bytes)
        || !charge(resident,replacement_bytes,l.peak_bytes)) return {Error::PeakLimit};
    std::unique_ptr<NodePlan[]> plan(new (std::nothrow) NodePlan[s->count]);
    std::unique_ptr<EditPlan[]> ep;
    if (!edits.empty()) ep.reset(new (std::nothrow) EditPlan[edits.size()]);
    if (!plan || (!edits.empty() && !ep)) return {Error::AllocationFailure};
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        if ((!e.target.source && e.target.index) || (!e.parent.source && e.parent.index)
            || (!e.before.source && e.before.index)) return {Error::InvalidHandle};
        if (e.kind != EditKind::Replace && e.kind != EditKind::Remove && e.kind != EditKind::Insert && e.kind != EditKind::Move) return {Error::InvalidEdit};
        if (e.kind != EditKind::Insert) {
            if (!s->valid(e.target)) return {Error::InvalidHandle};
            const auto& n = s->nodes[e.target.index];
            if (s->raw(n) != e.expected) return {Error::ExpectedMismatch,n.begin};
            if (ambiguous_path(*s,e.target.index)) return {Error::AmbiguousKey,n.member_begin};
            if (plan[e.target.index].operation != none) return {Error::ConflictingEdits,n.begin};
            if ((e.kind == EditKind::Remove || e.kind == EditKind::Move) && n.parent == none) return {Error::InvalidEdit,n.begin};
            plan[e.target.index].operation = i; mark(*s,plan.get(),e.target.index);
        } else if (e.target.source) return {Error::InvalidEdit};
        if (e.kind == EditKind::Replace) {
            if (e.parent.source || e.before.source) return {Error::InvalidEdit};
            Parser replacement(e.replacement,l); auto d = replacement.run(); if (!d) return d;
        } else if (e.kind == EditKind::Remove) {
            if (e.parent.source || e.before.source || !e.replacement.empty()) return {Error::InvalidEdit};
        } else {
            if (!s->valid(e.parent)) return {Error::InvalidHandle};
            const auto& parent = s->nodes[e.parent.index];
            if (!is_container(parent.kind)) return {Error::InvalidEdit,parent.begin};
            if (ambiguous_path(*s,e.parent.index)) return {Error::AmbiguousKey,parent.member_begin};
            if (e.before.source && (!s->valid(e.before) || s->nodes[e.before.index].parent != e.parent.index)) return {Error::InvalidHandle};
            if (e.before.source && ambiguous_path(*s,e.before.index)) return {Error::AmbiguousKey,s->nodes[e.before.index].member_begin};
            if (e.kind == EditKind::Insert) {
                if (s->raw(parent) != e.expected) return {Error::ExpectedMismatch,parent.begin};
                Parser replacement(e.replacement,l); Diagnostic d;
                if (parent.kind == NodeKind::Object) d = replacement.member_fragment(ep[i].key,ep[i].key_size);
                else d = replacement.run();
                if (!d) return d;
            } else {
                if (!e.replacement.empty()) return {Error::InvalidEdit};
                const auto& n = s->nodes[e.target.index];
                if (s->nodes[n.parent].kind != parent.kind || ancestor(*s,e.target.index,e.parent.index)) return {Error::InvalidEdit,parent.begin};
                auto key = s->text(n.key); ep[i].key_size = key.size();
                if (!key.empty()) std::memcpy(ep[i].key,key.data(),key.size());
            }
            mark(*s,plan.get(),e.parent.index);
            if (e.before.source) chain(ep.get(),i,plan[e.before.index].before_first,plan[e.before.index].before_last);
            else chain(ep.get(),i,plan[e.parent.index].append_first,plan[e.parent.index].append_last);
        }
    }
    // Complete plan first, then detect conflicts regardless of edit order.
    for (const auto& e : edits) {
        auto check_ancestors = [&](std::size_t index) noexcept {
            for (auto a = index; a != none; a = s->nodes[a].parent)
                if (plan[a].operation != none && edits[plan[a].operation].kind != EditKind::Insert) return false;
            return true;
        };
        if (e.kind != EditKind::Insert && !check_ancestors(s->nodes[e.target.index].parent)) return {Error::ConflictingEdits,s->nodes[e.target.index].begin};
        if (e.kind == EditKind::Insert || e.kind == EditKind::Move) {
            if (!check_ancestors(e.parent.index)) return {Error::ConflictingEdits,s->nodes[e.parent.index].begin};
            if (e.before.source && plan[e.before.index].operation != none
                && edits[plan[e.before.index].operation].kind != EditKind::Replace) return {Error::ConflictingEdits,s->nodes[e.before.index].begin};
        }
    }
    // Introducing a duplicate is ambiguous. Existing untouched opaque duplicates
    // remain ordered raw members; changing their individual occurrences refuses.
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        if ((e.kind != EditKind::Insert && e.kind != EditKind::Move) || s->nodes[e.parent.index].kind != NodeKind::Object) continue;
        const std::string_view key(ep[i].key,ep[i].key_size);
        for (auto child = s->nodes[e.parent.index].first; child != none; child = s->nodes[child].next) {
            const auto op = plan[child].operation;
            if (op != none && (edits[op].kind == EditKind::Remove || edits[op].kind == EditKind::Move)) continue;
            if (s->text(s->nodes[child].key) == key) return {Error::AmbiguousKey,s->nodes[child].member_begin};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if ((edits[j].kind == EditKind::Insert || edits[j].kind == EditKind::Move) && edits[j].parent == e.parent
                && std::string_view(ep[j].key,ep[j].key_size) == key) return {Error::AmbiguousKey,s->nodes[e.parent.index].begin};
        }
    }
    Renderer count(*s,plan.get(),ep.get(),edits,l.candidate_bytes); auto d = count.run(); if (!d) return d;
    std::size_t candidate_charge = resident;
    if (!charge(candidate_charge,count.size(),l.peak_bytes)) return {Error::PeakLimit};
    std::unique_ptr<char[]> bytes(new (std::nothrow) char[count.size()]); if (!bytes) return {Error::AllocationFailure};
    Renderer write(*s,plan.get(),ep.get(),edits,l.candidate_bytes,bytes.get()); d = write.run(); if (!d) return d;
    const std::string_view candidate(bytes.get(),count.size());
    Parser parser(candidate,l); d = parser.run(); if (!d) return d;
    // resident excludes candidate bytes; materialize charges them once and takes
    // over their ownership without allocating or copying a second byte buffer.
    return materialize(candidate,parser.counts(),resident,std::move(bytes),out,l);
}
} // namespace project_storage
