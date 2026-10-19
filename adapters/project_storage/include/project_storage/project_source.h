#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace project_storage {

enum class Error {
    None, InvalidLimits, InvalidJson, InvalidUtf8, InvalidEscape, DepthLimit,
    RecordLimit, TokenLimit, TextLimit, SourceLimit, CandidateLimit, EditLimit,
    PeakLimit, AllocationFailure, InvalidHandle, MissingMember, DuplicateKnownKey,
    AmbiguousKey, ExpectedMismatch, InvalidEdit, ConflictingEdits, IntegerRange
};
struct Diagnostic {
    Error code = Error::None;
    std::size_t offset = 0;
    explicit operator bool() const noexcept { return code == Error::None; }
};
struct DocumentLimits {
    std::size_t source_bytes = 16u * 1024u * 1024u;
    std::size_t candidate_bytes = 16u * 1024u * 1024u;
    std::size_t depth = 128;
    std::size_t records = 250000;
    std::size_t tokens = 1000000;
    std::size_t number_bytes = 64u * 1024u;
    std::size_t key_bytes = 1024;
    std::size_t raw_key_bytes = 6u * 1024u;
    // A loaded frequency profile is a bounded JSON string parameter. Keep the
    // whole-document/decoded/peak ceilings independent of this per-string cap.
    std::size_t string_bytes = 1024u * 1024u;
    std::size_t raw_string_bytes = 6u * 1024u * 1024u;
    std::size_t decoded_bytes = 4u * 1024u * 1024u;
    std::size_t edits = 8192;
    std::size_t replacement_bytes = 16u * 1024u * 1024u;
    std::size_t peak_bytes = 64u * 1024u * 1024u;
};
enum class NodeKind { Object, Array, String, Number, Boolean, Null };
struct NodeHandle {
    std::uint64_t source = 0;
    std::size_t index = 0;
    friend bool operator==(NodeHandle, NodeHandle) = default;
};
struct NodeView {
    NodeKind kind = NodeKind::Null;
    std::string_view raw;
    std::string_view decoded; // String value only; Number is always raw.
    std::string_view key;     // Decoded member key; may contain embedded NUL.
    std::size_t offset = 0;
    NodeHandle parent, first_child, next_sibling;
};

// Immutable ownership. Handles survive a move, but are never valid for another
// parse/candidate, even if its bytes are equal. Views borrow from this owner.
class OwnedProjectSource {
public:
    OwnedProjectSource() noexcept;
    ~OwnedProjectSource();
    OwnedProjectSource(OwnedProjectSource&&) noexcept;
    OwnedProjectSource& operator=(OwnedProjectSource&&) noexcept;
    OwnedProjectSource(const OwnedProjectSource&) = delete;
    OwnedProjectSource& operator=(const OwnedProjectSource&) = delete;
    std::string_view bytes() const noexcept;
    NodeHandle root() const noexcept;
    std::size_t node_count() const noexcept;
    std::size_t owned_bytes() const noexcept;
    Diagnostic view(NodeHandle, NodeView&) const noexcept;
private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    friend struct SourceAccess;
};

// On any error output is unchanged. Includes old output ownership in peak charge.
Diagnostic parse_project_source(std::string_view, OwnedProjectSource&,
                                const DocumentLimits& = {}) noexcept;
// Direct object-member boundary only; no paths, reflection or schema registry.
Diagnostic find_unique_member(const OwnedProjectSource&, NodeHandle object,
                              std::string_view decoded_key, NodeHandle&) noexcept;
Diagnostic validate_known_keys(const OwnedProjectSource&, NodeHandle object,
                              std::span<const std::string_view>) noexcept;
Diagnostic read_int64(const OwnedProjectSource&, NodeHandle, std::int64_t&) noexcept;
Diagnostic read_uint64(const OwnedProjectSource&, NodeHandle, std::uint64_t&) noexcept;

enum class EditKind { Replace, Remove, Insert, Move };
struct DocumentEdit {
    EditKind kind = EditKind::Replace;
    NodeHandle target; // Replace/remove/move: original value node.
    NodeHandle parent; // Insert/move: original destination object/array.
    NodeHandle before; // Destination child; empty means append.
    std::string_view expected; // Exact target raw; insert: exact parent raw.
    // Insert array: JSON value. Insert object: one JSON member ("key":value).
    // Replace: JSON value. All views are borrowed for this synchronous call.
    std::string_view replacement;
};
// All edits address one immutable snapshot simultaneously. No overlapping target
// edits or edits below a replaced/removed/moved subtree. Explicit moves are
// identity-neutral, same container kind, and retain the complete member/value.
// Changed containers may normalize separator whitespace; untouched subtree bytes
// and every retained lexeme are copied exactly. Candidate is reparsed atomically.
Diagnostic stage_project_edits(const OwnedProjectSource&, std::span<const DocumentEdit>,
                               OwnedProjectSource&, const DocumentLimits& = {}) noexcept;

} // namespace project_storage
