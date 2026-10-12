#include <bdsp_protocol/numeric_control_codec.h>
#include "numeric_control.pb.h"
#include <cbor.h>
#include <pb_common.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <array>
#include <cstring>
#ifdef PB_ENABLE_MALLOC
#error Numeric control requires caller storage
#endif
#ifdef PB_WITHOUT_64BIT
#error Numeric control requires exact 64-bit protobuf fields
#endif
namespace bdsp_protocol::numeric_control {
namespace {
using namespace detail;
const pb_msgdesc_t *descriptor(Schema s) noexcept {
    switch (s) {
    case Schema::Literal:
        return &bdsp_numeric_control_v4_Literal_msg;
    case Schema::Ratio:
        return &bdsp_numeric_control_v4_Ratio_msg;
    case Schema::Extent:
        return &bdsp_numeric_control_v4_Extent_msg;
    case Schema::Coordinates:
        return &bdsp_numeric_control_v4_Coordinates_msg;
    case Schema::Representation:
        return &bdsp_numeric_control_v4_Representation_msg;
    case Schema::Numeric:
        return &bdsp_numeric_control_v4_Numeric_msg;
    case Schema::Structured:
        return &bdsp_numeric_control_v4_Structured_msg;
    case Schema::Contract:
        return &bdsp_numeric_control_v4_Contract_msg;
    case Schema::Policy:
        return &bdsp_numeric_control_v4_Policy_msg;
    case Schema::Dyadic:
        return &bdsp_numeric_control_v4_Dyadic_msg;
    case Schema::Parameter:
        return &bdsp_numeric_control_v4_Parameter_msg;
    case Schema::Oscillator:
        return &bdsp_numeric_control_v4_Oscillator_msg;
    case Schema::MixerWeight:
        return &bdsp_numeric_control_v4_MixerWeight_msg;
    case Schema::Parameters:
        return &bdsp_numeric_control_v4_Parameters_msg;
    case Schema::Port:
        return &bdsp_numeric_control_v4_Port_msg;
    case Schema::Node:
        return &bdsp_numeric_control_v4_Node_msg;
    case Schema::Edge:
        return &bdsp_numeric_control_v4_Edge_msg;
    case Schema::Versions:
        return &bdsp_numeric_control_v4_Versions_msg;
    case Schema::Graph:
        return &bdsp_numeric_control_v4_Graph_msg;
    case Schema::Command:
        return &bdsp_numeric_control_v4_Command_msg;
    case Schema::Resources:
        return &bdsp_numeric_control_v4_Resources_msg;
    case Schema::CapabilityRow:
        return &bdsp_numeric_control_v4_CapabilityRow_msg;
    case Schema::Capabilities:
        return &bdsp_numeric_control_v4_Capabilities_msg;
    case Schema::Diagnostic:
        return &bdsp_numeric_control_v4_Diagnostic_msg;
    case Schema::State:
        return &bdsp_numeric_control_v4_State_msg;
    case Schema::Reply:
        return &bdsp_numeric_control_v4_Reply_msg;
    case Schema::Extension:
        return &bdsp_numeric_control_v4_Extension_msg;
    case Schema::Message:
        return &bdsp_numeric_control_v4_Message_msg;
    default:
        return nullptr;
    }
}
pb_wire_type_t wire_type(pb_type_t type) noexcept {
    switch (PB_LTYPE(type)) {
    case PB_LTYPE_BOOL:
    case PB_LTYPE_VARINT:
    case PB_LTYPE_UVARINT:
    case PB_LTYPE_SVARINT:
        return PB_WT_VARINT;
    case PB_LTYPE_FIXED32:
        return PB_WT_32BIT;
    case PB_LTYPE_FIXED64:
        return PB_WT_64BIT;
    default:
        return PB_WT_STRING;
    }
}
class Counter final : public Archive {
  public:
    unsigned fields = 0;
    using Archive::Archive;
    bool uint_field(unsigned, std::uint64_t &) noexcept override {
        ++fields;
        return true;
    }
    bool int_field(unsigned, std::int64_t &) noexcept override {
        ++fields;
        return true;
    }
    bool bool_field(unsigned, bool &) noexcept override {
        ++fields;
        return true;
    }
    bool text_field(unsigned, std::string_view &, bool) noexcept override {
        ++fields;
        return true;
    }
    bool optional_uint(unsigned, std::uint64_t &, bool &p) noexcept override {
        if (p)
            ++fields;
        return true;
    }
    bool optional_int(unsigned, std::int64_t &, bool &p) noexcept override {
        if (p)
            ++fields;
        return true;
    }
    bool object(unsigned, Schema, void *, Visit, bool *p) noexcept override {
        if (!p || *p)
            ++fields;
        return true;
    }
    bool sequence_size(unsigned, std::size_t &) noexcept override {
        return true;
    }
    bool sequence(unsigned, Schema, void *, std::size_t, ElementVisit) noexcept override {
        ++fields;
        return true;
    }
};
class Writer final : public Archive {
    CodecId codec_;
    Schema schema_;
    std::size_t depth_;
    bool sizing_;
    CborEncoder *cbor_;
    pb_ostream_t *pb_;
    bool ok(CborError e) noexcept {
        return e == CborNoError || (sizing_ && e == CborErrorOutOfMemory) || state.fail(Status::CodecFailure);
    }
    bool key(unsigned tag, pb_wire_type_t wt = PB_WT_VARINT) noexcept {
        if (schema_ == Schema::UIntElement)
            return true;
        if (codec_ == CodecId::Cbor)
            return ok(cbor_encode_uint(cbor_, tag));
        pb_field_iter_t f;
        if (!pb_field_iter_begin(&f, descriptor(schema_), nullptr) || !pb_field_iter_find(&f, tag) ||
            wire_type(f.type) != wt)
            return state.fail(Status::CodecFailure);
        return pb_encode_tag(pb_, wt, tag) || state.fail(Status::CodecFailure);
    }
    pb_wire_type_t type(unsigned tag) noexcept {
        if (schema_ == Schema::UIntElement)
            return PB_WT_VARINT;
        pb_field_iter_t f;
        if (!pb_field_iter_begin(&f, descriptor(schema_), nullptr) || !pb_field_iter_find(&f, tag)) {
            state.fail(Status::CodecFailure);
            return PB_WT_VARINT;
        }
        return wire_type(f.type);
    }

  public:
    Writer(Context &s, CodecId codec, Schema schema, std::size_t depth, bool sizing, CborEncoder *cbor,
           pb_ostream_t *pb)
        : Archive(s), codec_(codec), schema_(schema), depth_(depth), sizing_(sizing), cbor_(cbor), pb_(pb) {}
    bool uint_field(unsigned tag, std::uint64_t &n) noexcept override {
        if (codec_ == CodecId::Cbor)
            return key(tag) && ok(cbor_encode_uint(cbor_, n));
        auto wt = type(tag);
        if (!key(tag, wt))
            return false;
        if (wt == PB_WT_32BIT) {
            if (n > UINT32_MAX)
                return state.fail();
            auto v = static_cast<std::uint32_t>(n);
            return pb_encode_fixed32(pb_, &v) || state.fail(Status::CodecFailure);
        }
        if (wt == PB_WT_64BIT)
            return pb_encode_fixed64(pb_, &n) || state.fail(Status::CodecFailure);
        return pb_encode_varint(pb_, n) || state.fail(Status::CodecFailure);
    }
    bool int_field(unsigned tag, std::int64_t &n) noexcept override {
        if (codec_ == CodecId::Cbor)
            return key(tag) && ok(cbor_encode_int(cbor_, n));
        return key(tag) && (pb_encode_svarint(pb_, n) || state.fail(Status::CodecFailure));
    }
    bool bool_field(unsigned tag, bool &n) noexcept override {
        if (codec_ == CodecId::Cbor)
            return key(tag) && ok(cbor_encode_boolean(cbor_, n));
        return key(tag) && (pb_encode_varint(pb_, n ? 1 : 0) || state.fail(Status::CodecFailure));
    }
    bool text_field(unsigned tag, std::string_view &v, bool binary) noexcept override {
        if (codec_ == CodecId::Cbor)
            return key(tag) &&
                   ok(binary
                          ? cbor_encode_byte_string(cbor_, reinterpret_cast<const std::uint8_t *>(v.data()), v.size())
                          : cbor_encode_text_string(cbor_, v.empty() ? "" : v.data(), v.size()));
        return key(tag, PB_WT_STRING) &&
               (pb_encode_string(pb_, reinterpret_cast<const pb_byte_t *>(v.data()), v.size()) ||
                state.fail(Status::CodecFailure));
    }
    bool optional_uint(unsigned tag, std::uint64_t &n, bool &present) noexcept override {
        return !present || uint_field(tag, n);
    }
    bool optional_int(unsigned tag, std::int64_t &n, bool &present) noexcept override {
        return !present || int_field(tag, n);
    }
    bool map(void *value, Visit f) noexcept {
        if (depth_ > state.limits.depth || depth_ > HardDepth)
            return state.fail(Status::Limit);
        if (codec_ == CodecId::Protobuf)
            return f(*this, value);
        Counter counter(state);
        if (!f(counter, value))
            return false;
        CborEncoder child;
        if (!ok(cbor_encoder_create_map(cbor_, &child, counter.fields)))
            return false;
        Writer w(state, codec_, schema_, depth_, sizing_, &child, nullptr);
        return f(w, value) && ok(cbor_encoder_close_container_checked(cbor_, &child));
    }
    bool object(unsigned tag, Schema schema, void *value, Visit f, bool *present) noexcept override {
        if (present && !*present)
            return true;
        if (codec_ == CodecId::Cbor) {
            if (!key(tag))
                return false;
            Writer w(state, codec_, schema, depth_ + 1, sizing_, cbor_, nullptr);
            return w.map(value, f);
        }
        if (!key(tag, PB_WT_STRING))
            return false;
        auto measure = pb_ostream_t PB_OSTREAM_SIZING;
        Writer m(state, codec_, schema, depth_ + 1, true, nullptr, &measure);
        if (!m.map(value, f) || !pb_encode_varint(pb_, measure.bytes_written))
            return state.fail(Status::CodecFailure);
        Writer w(state, codec_, schema, depth_ + 1, sizing_, nullptr, pb_);
        return w.map(value, f);
    }
    bool sequence_size(unsigned, std::size_t &) noexcept override {
        return true;
    }
    bool sequence(unsigned tag, Schema schema, void *p, std::size_t n, ElementVisit f) noexcept override {
        if (depth_ + 1 > state.limits.depth || depth_ + 2 > HardDepth)
            return state.fail(Status::Limit);
        if (codec_ == CodecId::Cbor) {
            if (!key(tag))
                return false;
            CborEncoder array;
            if (!ok(cbor_encoder_create_array(cbor_, &array, n)))
                return false;
            for (std::size_t i = 0; i < n; ++i) {
                if (schema == Schema::UIntElement) {
                    Writer w(state, codec_, schema, depth_ + 1, sizing_, &array, nullptr);
                    if (!f(w, p, i))
                        return false;
                } else {
                    Counter counter(state);
                    if (!f(counter, p, i))
                        return false;
                    CborEncoder child;
                    if (!ok(cbor_encoder_create_map(&array, &child, counter.fields)))
                        return false;
                    Writer w(state, codec_, schema, depth_ + 2, sizing_, &child, nullptr);
                    if (!f(w, p, i) || !ok(cbor_encoder_close_container_checked(&array, &child)))
                        return false;
                }
            }
            return ok(cbor_encoder_close_container_checked(cbor_, &array));
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (schema == Schema::UIntElement) {
                if (!key(tag))
                    return false;
                Writer w(state, codec_, schema, depth_ + 1, sizing_, nullptr, pb_);
                if (!f(w, p, i))
                    return false;
            } else {
                if (!key(tag, PB_WT_STRING))
                    return false;
                auto measure = pb_ostream_t PB_OSTREAM_SIZING;
                Writer m(state, codec_, schema, depth_ + 2, true, nullptr, &measure);
                if (!f(m, p, i) || !pb_encode_varint(pb_, measure.bytes_written))
                    return state.fail(Status::CodecFailure);
                Writer w(state, codec_, schema, depth_ + 2, sizing_, nullptr, pb_);
                if (!f(w, p, i))
                    return false;
            }
        }
        return true;
    }
};
bool bounded(CborValue &v, std::size_t depth, Context &s) noexcept {
    if (depth > s.limits.depth || depth > HardDepth)
        return s.fail(Status::Limit);
    if (cbor_value_is_container(&v)) {
        if (!cbor_value_is_length_known(&v))
            return s.fail();
        CborValue child;
        if (cbor_value_enter_container(&v, &child) != CborNoError)
            return s.fail();
        while (!cbor_value_at_end(&child))
            if (!bounded(child, depth + 1, s))
                return false;
        return cbor_value_leave_container(&v, &child) == CborNoError || s.fail();
    }
    if (cbor_value_is_tag(&v))
        return s.fail();
    return cbor_value_advance(&v) == CborNoError || s.fail();
}
class Reader final : public Archive {
    CodecId codec_;
    Schema schema_;
    std::size_t depth_;
    std::array<CborValue, 22> cbor_{};
    std::array<std::span<const std::uint8_t>, 22> pb_{};
    std::span<const std::uint8_t> source_;
    std::uint32_t present_ = 0, used_ = 0;
    bool take(unsigned tag) noexcept {
        auto bit = std::uint32_t{1} << tag;
        if (!(present_ & bit))
            return state.fail();
        used_ |= bit;
        return true;
    }
    bool blob(unsigned tag, std::span<const std::uint8_t> &bytes) noexcept {
        if (!take(tag))
            return false;
        auto stream = pb_istream_from_buffer(pb_[tag].data(), pb_[tag].size());
        pb_istream_t child;
        if (!pb_make_string_substream(&stream, &child))
            return state.fail();
        bytes = {static_cast<const std::uint8_t *>(child.state), child.bytes_left};
        return true;
    }

  public:
    Reader(Context &s, CodecId codec, Schema schema, std::size_t depth, const CborValue *cbor,
           std::span<const std::uint8_t> source)
        : Archive(s), codec_(codec), schema_(schema), depth_(depth), source_(source) {
        if (depth > s.limits.depth || depth > HardDepth) {
            state.fail(Status::Limit);
            return;
        }
        if (schema == Schema::UIntElement) {
            if (cbor)
                cbor_[0] = *cbor;
            else
                pb_[0] = source;
            present_ = 1;
            return;
        }
        if (codec == CodecId::Cbor) {
            if (!cbor_value_is_map(cbor) || !cbor_value_is_length_known(cbor)) {
                state.fail();
                return;
            }
            CborValue child;
            if (cbor_value_enter_container(cbor, &child) != CborNoError) {
                state.fail();
                return;
            }
            while (!cbor_value_at_end(&child)) {
                std::uint64_t tag = 0;
                if (!cbor_value_is_unsigned_integer(&child) || cbor_value_get_uint64(&child, &tag) != CborNoError) {
                    state.fail();
                    return;
                }
                if (!tag || tag > max_tag(schema) || tag >= cbor_.size()) {
                    state.fail(Status::UnsupportedField);
                    return;
                }
                auto bit = std::uint32_t{1} << tag;
                if (present_ & bit) {
                    state.fail();
                    return;
                }
                if (cbor_value_advance_fixed(&child) != CborNoError || cbor_value_at_end(&child)) {
                    state.fail();
                    return;
                }
                present_ |= bit;
                cbor_[tag] = child;
                if (cbor_value_advance(&child) != CborNoError) {
                    state.fail();
                    return;
                }
            }
        } else {
            auto stream = pb_istream_from_buffer(source.data(), source.size());
            while (stream.bytes_left) {
                std::uint32_t tag = 0;
                pb_wire_type_t wt;
                bool eof = false;
                if (!pb_decode_tag(&stream, &wt, &tag, &eof) || !tag || tag > max_tag(schema) || tag >= pb_.size()) {
                    state.fail(Status::UnsupportedField);
                    return;
                }
                pb_field_iter_t f;
                if (!pb_field_iter_begin(&f, descriptor(schema), nullptr) || !pb_field_iter_find(&f, tag) ||
                    wire_type(f.type) != wt) {
                    state.fail();
                    return;
                }
                auto bit = std::uint32_t{1} << tag;
                if ((present_ & bit) && PB_HTYPE(f.type) != PB_HTYPE_REPEATED) {
                    state.fail();
                    return;
                }
                auto *start = static_cast<const std::uint8_t *>(stream.state);
                auto remaining = stream.bytes_left;
                if (!pb_skip_field(&stream, wt)) {
                    state.fail();
                    return;
                }
                pb_[tag] = {start, remaining - stream.bytes_left};
                present_ |= bit;
            }
        }
    }
    bool finish() noexcept {
        return state.error == Status::Ok && (present_ == used_ || state.fail());
    }
    bool uint_field(unsigned tag, std::uint64_t &v) noexcept override {
        if (!take(tag))
            return false;
        if (codec_ == CodecId::Cbor)
            return (cbor_value_is_unsigned_integer(&cbor_[tag]) &&
                    cbor_value_get_uint64(&cbor_[tag], &v) == CborNoError) ||
                   state.fail();
        auto stream = pb_istream_from_buffer(pb_[tag].data(), pb_[tag].size());
        auto wt = PB_WT_VARINT;
        if (schema_ != Schema::UIntElement) {
            pb_field_iter_t f;
            if (!pb_field_iter_begin(&f, descriptor(schema_), nullptr) || !pb_field_iter_find(&f, tag))
                return state.fail();
            wt = wire_type(f.type);
        }
        bool ok = false;
        if (wt == PB_WT_32BIT) {
            std::uint32_t n = 0;
            ok = pb_decode_fixed32(&stream, &n);
            v = n;
        } else if (wt == PB_WT_64BIT)
            ok = pb_decode_fixed64(&stream, &v);
        else
            ok = pb_decode_varint(&stream, &v);
        return (ok && stream.bytes_left == 0) || state.fail();
    }
    bool int_field(unsigned tag, std::int64_t &v) noexcept override {
        if (!take(tag))
            return false;
        if (codec_ == CodecId::Cbor)
            return (cbor_value_is_integer(&cbor_[tag]) &&
                    cbor_value_get_int64_checked(&cbor_[tag], &v) == CborNoError) ||
                   state.fail();
        auto stream = pb_istream_from_buffer(pb_[tag].data(), pb_[tag].size());
        return (pb_decode_svarint(&stream, &v) && !stream.bytes_left) || state.fail();
    }
    bool bool_field(unsigned tag, bool &v) noexcept override {
        if (codec_ == CodecId::Cbor) {
            if (!take(tag))
                return false;
            return (cbor_value_is_boolean(&cbor_[tag]) && cbor_value_get_boolean(&cbor_[tag], &v) == CborNoError) ||
                   state.fail();
        }
        std::uint64_t n = 0;
        if (!uint_field(tag, n) || n > 1)
            return state.fail();
        v = n != 0;
        return true;
    }
    bool text_field(unsigned tag, std::string_view &v, bool binary) noexcept override {
        if (codec_ == CodecId::Cbor) {
            if (!take(tag))
                return false;
            auto item = cbor_[tag];
            CborValue next;
            std::size_t n = 0;
            if (!cbor_value_is_length_known(&item) || cbor_value_begin_string_iteration(&item) != CborNoError)
                return state.fail();
            if (binary) {
                const std::uint8_t *p = nullptr;
                if (!cbor_value_is_byte_string(&item) ||
                    cbor_value_get_byte_string_chunk(&item, &p, &n, &next) != CborNoError)
                    return state.fail();
                v = {reinterpret_cast<const char *>(p), n};
            } else {
                const char *p = nullptr;
                if (!cbor_value_is_text_string(&item) ||
                    cbor_value_get_text_string_chunk(&item, &p, &n, &next) != CborNoError)
                    return state.fail();
                v = {p, n};
            }
        } else {
            std::span<const std::uint8_t> bytes;
            if (!blob(tag, bytes))
                return false;
            v = {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }
        return state.store(v, binary ? state.limits.extension_bytes : state.limits.string_bytes, binary);
    }
    bool optional_uint(unsigned tag, std::uint64_t &v, bool &p) noexcept override {
        p = (present_ & (std::uint32_t{1} << tag)) != 0;
        return !p || uint_field(tag, v);
    }
    bool optional_int(unsigned tag, std::int64_t &v, bool &p) noexcept override {
        p = (present_ & (std::uint32_t{1} << tag)) != 0;
        return !p || int_field(tag, v);
    }
    bool object(unsigned tag, Schema schema, void *p, Visit f, bool *present) noexcept override {
        if (present) {
            *present = (present_ & (std::uint32_t{1} << tag)) != 0;
            if (!*present)
                return true;
        }
        const CborValue *item = nullptr;
        std::span<const std::uint8_t> bytes;
        if (codec_ == CodecId::Cbor) {
            if (!take(tag))
                return false;
            item = &cbor_[tag];
        } else if (!blob(tag, bytes))
            return false;
        Reader r(state, codec_, schema, depth_ + 1, item, bytes);
        return state.error == Status::Ok && f(r, p) && r.finish();
    }
    bool sequence_size(unsigned tag, std::size_t &n) noexcept override {
        if (codec_ == CodecId::Cbor) {
            if (!take(tag))
                return false;
            return (cbor_value_is_array(&cbor_[tag]) && cbor_value_get_array_length(&cbor_[tag], &n) == CborNoError) ||
                   state.fail();
        }
        used_ |= present_ & (std::uint32_t{1} << tag);
        n = 0;
        auto stream = pb_istream_from_buffer(source_.data(), source_.size());
        while (stream.bytes_left) {
            pb_wire_type_t wt;
            std::uint32_t t;
            bool eof = false;
            if (!pb_decode_tag(&stream, &wt, &t, &eof) || !pb_skip_field(&stream, wt))
                return state.fail();
            if (t == tag) {
                if (n == SIZE_MAX)
                    return state.fail(Status::Overflow);
                ++n;
            }
        }
        return true;
    }
    bool sequence(unsigned tag, Schema schema, void *p, std::size_t count, ElementVisit f) noexcept override {
        std::size_t i = 0;
        if (codec_ == CodecId::Cbor) {
            CborValue child;
            if (cbor_value_enter_container(&cbor_[tag], &child) != CborNoError)
                return state.fail();
            while (!cbor_value_at_end(&child)) {
                Reader r(state, codec_, schema, depth_ + (schema == Schema::UIntElement ? 1 : 2), &child, {});
                if (i >= count || state.error != Status::Ok || !f(r, p, i++) || !r.finish() ||
                    cbor_value_advance(&child) != CborNoError)
                    return state.fail();
            }
            return i == count || state.fail();
        }
        auto stream = pb_istream_from_buffer(source_.data(), source_.size());
        while (stream.bytes_left) {
            pb_wire_type_t wt;
            std::uint32_t t;
            bool eof = false;
            if (!pb_decode_tag(&stream, &wt, &t, &eof))
                return state.fail();
            auto *start = static_cast<const std::uint8_t *>(stream.state);
            auto remaining = stream.bytes_left;
            if (!pb_skip_field(&stream, wt))
                return state.fail();
            if (t != tag)
                continue;
            std::span<const std::uint8_t> bytes{start, remaining - stream.bytes_left};
            if (schema != Schema::UIntElement) {
                auto s = pb_istream_from_buffer(bytes.data(), bytes.size());
                pb_istream_t sub;
                if (!pb_make_string_substream(&s, &sub))
                    return state.fail();
                bytes = {static_cast<const std::uint8_t *>(sub.state), sub.bytes_left};
            }
            Reader r(state, codec_, schema, depth_ + (schema == Schema::UIntElement ? 1 : 2), nullptr, bytes);
            if (i >= count || state.error != Status::Ok || !f(r, p, i++) || !r.finish())
                return state.fail();
        }
        return i == count || state.fail();
    }
};
bool read(CodecId codec, std::span<const std::uint8_t> input, Context &s, Message &m) noexcept {
    if (codec == CodecId::Protobuf) {
        Reader r(s, codec, Schema::Message, 1, nullptr, input);
        return s.error == Status::Ok && visit_message(r, &m) && r.finish();
    }
    CborParser parser;
    CborValue root;
    if (cbor_parser_init(input.data(), input.size(), 0, &parser, &root) != CborNoError)
        return s.fail();
    auto copy = root;
    if (!bounded(copy, 1, s) || cbor_value_get_next_byte(&copy) != input.data() + input.size())
        return s.fail();
    if (cbor_value_validate(&root, CborValidateShortestIntegrals | CborValidateNoIndeterminateLength |
                                       CborValidateUtf8 | CborValidateNoTags) != CborNoError)
        return s.fail();
    Reader r(s, codec, Schema::Message, 1, &root, {});
    return s.error == Status::Ok && visit_message(r, &m) && r.finish();
}
Result encode_impl(CodecId codec, const Message &source, std::span<std::uint8_t> output,
                   std::span<std::uint8_t> scratch, const Limits &limits) noexcept {
    auto result = validate(source, limits);
    if (!result)
        return result;
    Context s{limits, {}};
    Message m = source;
    CborEncoder cbor;
    cbor_encoder_init(&cbor, nullptr, 0, 0);
    auto pb = pb_ostream_t PB_OSTREAM_SIZING;
    Writer measure(s, codec, Schema::Message, 1, true, &cbor, &pb);
    if (!measure.map(&m, visit_message))
        return {s.error};
    auto size = codec == CodecId::Cbor ? cbor_encoder_get_extra_bytes_needed(&cbor) : pb.bytes_written;
    if (size > limits.wire_bytes)
        return {Status::Limit};
    result.required_wire = size;
    result.required_scratch = size;
    if (output.size() < size || scratch.size() < size) {
        result.status = Status::BufferTooSmall;
        return result;
    }
    cbor_encoder_init(&cbor, scratch.data(), scratch.size(), 0);
    pb = pb_ostream_from_buffer(scratch.data(), scratch.size());
    Writer writer(s, codec, Schema::Message, 1, false, &cbor, &pb);
    if (!writer.map(&m, visit_message))
        return {s.error};
    auto actual = codec == CodecId::Cbor ? cbor_encoder_get_buffer_size(&cbor, scratch.data()) : pb.bytes_written;
    if (actual != size)
        return {Status::CodecFailure};
    std::memcpy(output.data(), scratch.data(), size);
    result.produced = size;
    return result;
}
bool aligned(std::span<std::byte> bytes) noexcept {
    return bytes.empty() || reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(std::max_align_t) == 0;
}
Result decode_impl(CodecId codec, std::span<const std::uint8_t> input, std::span<std::byte> arena,
                   std::span<std::byte> stage, Message &output, const Limits &limits) noexcept {
    if (limits.depth > HardDepth || !aligned(arena) || !aligned(stage))
        return {Status::InvalidArgument};
    if (input.empty())
        return {Status::Malformed};
    if (input.size() > limits.wire_bytes)
        return {Status::Limit};
    Context count{limits, {}, true, false};
    Message m;
    if (!read(codec, input, count, m))
        return {count.error};
    Result result;
    result.required_arena = count.used;
    result.required_scratch = count.used;
    if (stage.size() < count.used) {
        result.status = Status::BufferTooSmall;
        return result;
    }
    Context staging{limits, stage, true, true};
    m = {};
    if (!read(codec, input, staging, m))
        return {staging.error};
    auto checked = validate(m, limits);
    if (!checked)
        return checked;
    result.required_text = checked.required_text;
    if (arena.size() < count.used) {
        result.status = Status::BufferTooSmall;
        return result;
    }
    Context commit{limits, arena, true, true};
    m = {};
    if (!read(codec, input, commit, m))
        return {Status::CodecFailure};
    output = m;
    result.consumed = input.size();
    return result;
}
} // namespace
Result Codec::measure(const Message &m, const Limits &l) const noexcept {
    auto result = encode(m, {}, {}, l);
    if (result.status == Status::BufferTooSmall && result.required_wire != 0)
        result.status = Status::Ok;
    return result;
}
Result CborCodec::encode(const Message &m, std::span<std::uint8_t> out, std::span<std::uint8_t> scratch,
                         const Limits &l) const noexcept {
    return encode_impl(CodecId::Cbor, m, out, scratch, l);
}
Result CborCodec::decode(std::span<const std::uint8_t> in, std::span<std::byte> arena, std::span<std::byte> staging,
                         Message &m, const Limits &l) const noexcept {
    return decode_impl(CodecId::Cbor, in, arena, staging, m, l);
}
Result ProtobufCodec::encode(const Message &m, std::span<std::uint8_t> out, std::span<std::uint8_t> scratch,
                             const Limits &l) const noexcept {
    return encode_impl(CodecId::Protobuf, m, out, scratch, l);
}
Result ProtobufCodec::decode(std::span<const std::uint8_t> in, std::span<std::byte> arena, std::span<std::byte> staging,
                             Message &m, const Limits &l) const noexcept {
    return decode_impl(CodecId::Protobuf, in, arena, staging, m, l);
}
} // namespace bdsp_protocol::numeric_control
