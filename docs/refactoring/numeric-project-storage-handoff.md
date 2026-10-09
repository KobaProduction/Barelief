# N0 / N2 numeric project storage handoff

2026-10-07, Europe/Moscow. **LOSSLESS FOUNDATION SOURCE READY; N2 MAPPING PENDING.**
The original DOC-only proposal is retained below; section10 records the subsequent
explicitly authorized three-file foundation slice and its actual evidence.
Accepted bounded P7 storage/projection
source freeze, tests, JSON schema, CMake and user files remain unchanged. R/D are
implementing foundation/pure DSP; their live headers are inputs to coordination,
not a final accepted numeric interface. Historical P7 PASS does not prove JSON64.

Authority fully read: AGENTS.md, updated [implementation-plan](implementation-plan.md),
[numeric-data-contracts](numeric-data-contracts.md), [R foundation](numeric-foundation-handoff.md),
[D DSP](numeric-dsp-handoff.md), [K control/storage](numeric-control-storage-handoff.md),
[T delivery](numeric-delivery-handoff.md), and the full pinned
[Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Every future implementation/review dispatch must pass that URL and full-reading
requirement. The guide is responsibility guidance, not a pattern checklist.

## 1. Concrete decision: one owned lossless document, Qt projections

Choose a small project-specific JSON source/tree adapter with **owned original
UTF-8 bytes and spans for every token/subtree**, before any QJson projection.
Existing C++ standard facilities and Qt Core/file APIs suffice; no new parser
dependency, install, database, secondary config or generic parser framework.
Read-only dependency inventory found Qt Core on graph_document_adapter and no
configured alternate lossless JSON parser. This is a source/design decision, not
an implementation or parser conformance result.

Portable document code belongs to the outer document adapter, not domain or DSP.
It recognizes JSON syntax, decoded object keys, source spans and explicit edits;
it knows no NodeKind, dtype meaning, kernel, session, filesystem or Qt object.
Domain/R owns numeric contracts, structural policy/value validation and parameter
classification. Desktop JSON mapping consumes those types and emits typed DTOs.
Qt supplies UI projections, path/file handling and atomic commit. There is no
domain dependency on the document adapter or portable application JSON port.

The retained object is an ordered tree, not a map of double-valued JSON leaves:

- Immutable owned source bytes; node records contain kind, checked begin/end
  offsets, child/member indices and decoded member-name handles. No QJsonValueRef
  or string_view into temporary input survives the read call.
- Number leaves retain their complete lexical slice, including integer digits,
  fraction, exponent spelling and negative zero. Unknown numbers are never
  parsed to float, including 18446744073709551615, 9007199254740993 or 1e9999.
- Strings/keys validate JSON escape grammar and UTF-8, including surrogate-pair
  decoding. Known-key duplicate detection compares decoded names, so escaped
  `\u0064type` is the same key as `dtype`. Syntax errors, truncation, trailing
  input and exhausted limits yield diagnostics without a partial document.
- Objects retain order and duplicate members rather than accepting Qt's implicit
  selection. Unknown duplicate members may survive as untouched opaque subtrees;
  no edit/projection chooses one ambiguously.
- Caller source release/move cannot invalidate the document; returned slices
  borrow only from its immutable ownership. All operations are control/file path,
  never prepared processing or callback work.

This scanner needs only JSON value/string/number/container grammar and a bounded
ordered tree. It does not implement queries, reflection, codecs, arbitrary number
arithmetic or a general DOM framework. Integer conversion uses checked digit
accumulation/standard fixed-width facilities; no double intermediate or bigint
runtime dependency. Arithmetic on unknown tokens is unnecessary.

## 2. Read, edit and save pipeline

1. Read bytes once into an owned snapshot; validate syntax/limits and capture all
   lexemes. Identify project/graph/version/registered-field paths from schema
   dispatch. Reject duplicate known singleton keys before QJson conversion.
2. Classify versions from raw integer tokens with checked bounds, not toInt(default).
   Build any migration as a separate lossless candidate. The snapshot remains the
   storage source until an explicit successful save; loading never rewrites it.
3. Decode only registered fields from token nodes into R/application DTOs. Decode
   UI/app known fields into a separate Qt projection. Unknown fields stay in the
   lossless tree; no opaque JSON is forwarded as typed processing extensions.
4. Keep the original projected UI snapshot separately. Register actual field edits
   and encode those edits, rather than serializing the entire normalized UI model.
   Unchanged/default-normalized properties must not replace explicit raw values.
5. Stage a typed edit set and render a candidate. Unchanged subtrees emit original
   byte slices verbatim. Changed containers combine owned encoded replacements
   with original member/value slices; numeric tokens in untouched descendants
   remain exact. Reparse/check candidate and all recognized versions before I/O.
6. For save-in-place, compare the current destination bytes to the source snapshot
   before writing; external changes produce `SourceChanged`, no automatic merge
   or overwrite. For SaveAs, retain source provenance and validate the destination
   version/syntax before explicit replacement. Future/malformed destination refuses.
7. One QSaveFile replacement, direct-write fallback disabled. Write/commit failure
   keeps destination bytes, active path and document baseline unchanged. Only
   success publishes the already checked candidate as the new owned baseline;
   successful SaveAs changes the single active absolute path.

A second destination check just before commit can detect changes during staging;
QSaveFile alone is not a cross-process compare-and-swap lock. There is no promise
of exclusion against a writer racing after that final check. No new lock service
or database is proposed; coordinated external edits require explicit reload/reapply.

**No lost lexeme escape:** a QJsonObject-only input cannot demonstrate the spelling
or value of an already rounded unknown number. Numeric-project save from such an
input returns `MissingLosslessProvenance`, before QSaveFile. Existing legacy APIs
may continue reading/creating proven-safe in-memory legacy fixtures; production
whole-file writes must route through this source-backed path. No fallback re-dump.
An unchanged file can be retained byte-for-byte even when an unknown number is
outside IEEE range. Qt projections omit opaque nodes that Qt cannot represent;
they must not parse a whole source file containing those numbers just for prefs.

An opaque/ambiguous subtree replacement without source provenance is likewise
refused. Intentional removal/replacement requires an explicit edit operation with
the expected old subtree and an owning lossless replacement or authoritative
typed known-field replacement. A graph editor's generic whole-QJson dump is not
evidence that unknown node/parameter tokens were preserved.

## 3. Exact known numbers and policy literals

Root-approved encoding direction, shared with K, not a new numeric authority:

| Known value | Canonical JSON representation | Decode/validation |
| --- | --- | --- |
| Int64 | `{"dtype":"int64","decimal":"-9223372036854775808"}` | ASCII signed decimal, checked signed magnitude; min special case, no negation overflow |
| UInt64 | `{"dtype":"uint64","decimal":"18446744073709551615"}` | ASCII unsigned decimal, checked UInt64 bound |
| Float32 NumericLiteral | `{"dtype":"float32","bits":"80000000"}` | Exactly 8 lowercase hex digits; copy own binary32 bits, never widen through double |
| Float64 NumericLiteral | `{"dtype":"float64","bits":"8000000000000000"}` | Exactly 16 lowercase hex digits; own binary64 bits |
| Existing bounded IDs/versions/int16 exponent/uint32 fields | Existing JSON integer tokens where that version declares them | Exact checked lexical conversion to the declared width, not truncating toInt |

Decimal grammar: zero `0`, otherwise nonzero first digit followed by digits;
signed negative values have `-` and nonzero magnitude. Reject plus, leading zeros,
integer `-0`, whitespace, fraction/exponent, wrong tag/member or overflow. A tagged
64-bit known value must not arrive as a bare JSON number in the new version.
Within a recognized literal, duplicate dtype/decimal/bits keys and mutually
exclusive known payload fields are errors; extra optional unknown members remain
local source extensions, never an alternate value. Do not reject an unrelated
opaque object's `dtype` key as though it were a known NumericLiteral.

64-bit extent bounds, rational grid/clock components and configured count/capacity
fields follow their declared signed/unsigned widths. Node int32/port uint16 IDs
keep widths and identities. Schema numbers are not converted to tagged literal
objects. Runtime sequence/epoch/revision/token/count/history samples are not added
to project persistence merely because their widths now exist in DATA/control.

R's live NumericLiteral is variant<int64_t,uint64_t,float,double>. Mapping uses
the actual alternative's own width with bit extraction/insertion, no arithmetic
or formatting through a universal floating container. Preserve negative zero,
subnormal and nonfinite payload bits when permitted structurally. Replay/equality
must use R's exact tag+bit/value equality, not default float/variant equality;
NaN and signed zero make numeric equality inadequate. R confirmed the seam in
libs/signal_contracts/include/signal_contracts/numeric_policy.h:
literal_equal(NumericLiteral, NumericLiteral) noexcept compares variant tag and
own-width IEEE bits or exact integers; validate_literal structurally permits all
IEEE bit patterns and rejects a valueless variant. Active policy validity belongs
to D at Prepare. P consumes these helpers rather than creating another registry.
This receipt establishes the boundary; R's consolidated tested freeze is pending.

Persist **all** policy fields, including inactive gain/offset/dyadic exponents and
NaN/±Inf replacements. No erasure on Identity or NonFinite::Reject. Structural
tags/enums/widths are always validated. Only active fields receive R/D's semantic
policy checks: finite replacements, supported exponent/scaling, saturation/loss
permission and supported combinations. A preserved inactive value cannot enable
behavior. Unknown active required enum/policy/contract rejects Apply with a node/
field diagnostic; an opaque or invalid local draft may still be saved unchanged.

Small-width numeric field validity and Converter/Reduction meanings come from
R's configured descriptor/policy DTO, not P's former double ParameterDescriptor
table. Use the same authoritative validation on inbound and outbound typed
projection, after excluding Presentation/Opaque. Device caps remain in application.

## 4. Duplicate keys, huge integers and opaque merge provenance

Known singleton paths include root schema_version, graph version,
processing_version, graph_id, nodes/connections, typed parameter/contract fields,
policy discriminants and literal members, plus known app/UI edit targets. Any
duplicate on such a decoded path is `DuplicateKnownKey`; no first/last-wins Apply
or Save. This applies even when both values are numerically equal or a key is
escaped. Repeated IDs in arrays are separately checked by existing domain/display
provenance rules, not confused with repeated object keys.

Unknown duplicate keys are stored verbatim but cannot be addressed by a simple
key-only edit. A lossless NodeHandle identifies a unique source occurrence; known
parents that must be decoded cannot be ambiguous. If a later version promotes
an opaque key to known and finds duplicates, migration refuses atomically.

Unknown nested numbers under root/node/parameters/UI/presets/device/array/old
migration records retain their exact tokens. A number side table indexed only by
JSON path is insufficient for duplicate keys, reordered arrays and reused node
IDs; this is why the chosen source/tree owns opaque subtrees and stable edit
handles. It also preserves strings/booleans/null and arbitrary nested topology.

Graph replacement reconciles known members by graph ID + node ID + stable kind;
edges by both node+port endpoints and unchanged endpoint kinds, unique match only.
Presets use stable preset IDs with ambiguity refusal; a duplicate name is not an ID.
Arrays not carrying schema-defined identity are unchanged as slices or explicitly
replaced, never merged by index after reorder. Removed entities require explicit
edit intent; foreign/reused identities receive no old opaque/display values.
Local draft remains independent from accepted applied processing and UI settings.
Unknown opaque raw tokens are never imported from a remote processing reply.

| Array/subtree operation | Preservation proof or refusal |
| --- | --- |
| Reorder schema-identified nodes/presets | Unique stable identity and expected source handles; move complete retained slices, then patch only registered changed members |
| Remove an entity or replace an opaque array | Explicit operation names the exact expected old subtree; replacement owns valid lossless bytes and passes all candidate checks. Unchanged descendants remain slices unless removal/replacement explicitly includes them |
| Replace from a generic Qt array/object | Refuse when unknown descendants lack provenance, even if projected values compare equal; an index match or rounded numeric comparison is insufficient |

Legacy known integer conversion must use retained source tokens or refuse when
only a rounded Qt value survives. Current graph2 processing1 has bounded int32/
uint32 IDs/parameters and double-valued legacy numeric settings; it has no known
UInt64 policy field to discover by guessing. New-looking unknown keys stay opaque.
An explicitly defined legacy migration can promote only schema-known fields and
their raw tokens. It cannot recover the intended source of an already rounded
document or silently turn an extension into a new Converter.

## 5. Versions and explicit migration/refusal table

Root approved reserving schema2/graph3/processing2 as independent version axes.
This is a document decision, not authorization to change schema or source code.
Existing keys remain:
schema_version; pipeline_graph applied; scene_graph draft; ui; core/wled_ar_server;
presets or ui.pipeline_presets; devices; migration records and unknown extensions.

| Axis | Existing actual boundary | Approved reservation / reason |
| --- | --- | --- |
| Project | unversioned legacy → schema_version 1 | schema_version 2 marks exact known-field representation and source-backed lossless whole-file save; can contain graph2/processing1 without upgrading it |
| Graph document | version1 legacy → version2 current topology | version3 represents configured numeric/structured contracts and exact policy fields while retaining node/edge/layout keys |
| Processing semantics | processing_version1 legacy Float32 graph | processing_version2 consumes R's numeric contract/policy graph; determined by R/S/root, not by JSON shape |
| Control / DATA / structured | Independent K/T/device boundaries | Control schema2, DATA payload4, delivery grammar1 and registry3 remain separate axes; no project version implies kernel or device support |

Version membership is a reviewed combination table, not arbitrary cross-product.
Planned admission table: schema1 contains legacy graph1/2 processing1; schema2 may retain
legacy graph2 processing1 and supports graph3 processing2 after the semantic DTO
handoff. Unsupported combinations refuse Apply/migration/write with location.
No automatic numeric graph upgrade for a theme edit or default change.

| Explicit transition | Admission and result |
| --- | --- |
| Unversioned project → schema1; graph1 → graph2 | Retain existing topology migration/default semantics/IDs and explicit provenance record, now transform lossless subtrees rather than rounded QJson. Reject exhausted IDs/collisions/version errors. Existing exact inverse stays available. |
| Schema1 → schema2 | Keep physical keys and all opaque/UI/default/draft/preset tokens. Add version only as explicit migration; do not rewrite graphs to numeric by itself. Build source/mapped provenance as lossless subtrees, not JSON re-dump. |
| Graph2/processing1 → graph3/processing2 | Explicit migration using R's declared legacy Float32/semantic/channel/layout mapping and registered parameters. No invented kernel, Converter, normalization, channel merge or default4096→2048. Views' channel/merge stay local. Unknown or unresolvable processing kind/contract refuses applied upgrade; draft remains opaque. |
| Schema2 → schema1 | Exact inverse when the recorded source/mapped candidate still matches (including lexical unknown values); otherwise a reviewed field-by-field representability proof. Numeric-only fields or loss cannot be erased merely because current values fit double. Without proof return Unsupported/LossyDowngrade. |
| Graph3/processing2 → graph2/processing1 | Initially exact recorded inverse only. New dtype/Converter/explicit policies/layout/schema not expressible in legacy refuses; never cast/narrow or drop inactive fields. Do not accept an integer contract merely because one literal is Float32-representable. |
| Future/malformed known project/graph/processing version | Refuse Apply and overwrite including prefs/autosave/SaveAs replacement. Preserve source bytes and active state; do not normalize null/string/fraction/zero/huge version to default. |

Each transition builds a temporary complete candidate for local/per-device
applied/draft/presets and their provenance before publication. Migration may leave
opaque processing-invalid draft subtrees untouched; a requested draft conversion
that cannot preserve them returns refusal rather than partial success. Explicit
future/malformed versions on recognized draft/preset graphs still block writes;
a nested unrelated extension called version is opaque. A later field failure
must leave prior candidate, loaded source, working graph and file unchanged.

Known output requires canonical tagged literals; existing unknown lexical spelling
is part of inverse preservation. Recorded inverses use exact source/mapped token
tree comparison, not QJson double equality or a hash alone. A changed unknown
number must not pass an inverse because both lexemes project to the same double.
Migration provenance may duplicate source subtrees but is bounded and accounted;
it never includes DSP history, delivery buffers or session state.

## 6. Working state, paths and external edits

One config.json beside the executable by default, resolved to an absolute path.
Explicit CLI --config or user Open/SaveAs takes priority; cwd is not a second
default. Open publishes a new active path/baseline only after a valid supported
read. Failed Open/SaveAs keeps the old path and baseline. SaveAs writes the same
complete source-backed project to its requested path, not a processing projection.

File changes do not become working graph changes. An external editor requires
explicit reload/import and explicit Prepare/Apply through application/T; successful
accepted Apply is the sole source for applied processing updates. Draft/UI edits
can save without changing processing revision, reconnecting or device persistence.
Unknown tokens remain owned by the local document across accepted processing
reply merge. Rejected/timeout/stale remote Apply never replaces applied or loses
the local draft; request/epoch/revision association remains T-owned.

Saving must not serialize an entire current runtime graph/history or optimistic
editor state as applied. Keep an owned lossless project baseline, draft edits,
display edits and typed accepted-processing update separate until staged merge.
No shadow JSON sidecar or autosave that overwrites a changed external document.

## 7. Proposed public seams, files and sole owners

The project_source foundation is now live as recorded in section10. Other names
are concrete proposals for the next source handoff, not live headers.
Portable storage adapter APIs are result-returning, bounded by explicit
DocumentLimits, with owned source/tree output and error/path/offset. No exceptions
as error flow, no domain JSON interfaces. They can allocate on the bounded file/
control path; this is not a no-heap DSP/codec API promise.

| Proposed seam | Responsibility / result |
| --- | --- |
| parse_project_source(span<const byte>, DocumentLimits) → OwnedProjectSource/result | Syntax/lexemes/ordered source tree before Qt; copies/owns source, publishes only on success |
| read_decimal64(TokenView, signedness) → exact value/result | Checked raw integer grammar/range, scoped to recognized legacy integer fields; no policy authority |
| stage_project_edits(source, span<DocumentEdit>, limits) → candidate/result | Expected subtree handles + explicit insert/replace/remove; untouched slice preservation, ambiguous/missing provenance refusal |
| migrate_project_source(source, target versions, limits) → candidate/result | Explicit adapter version chain; R supplies semantic legacy→numeric mapping and validation |
| project_numeric_processing(source graph handle, R/application DTO, error) | Descriptor allowlist + exact typed mapping in both directions; no opaque escape; authoritative parameter/policy validation |
| encode_numeric_processing(R/application DTO) → owned typed edit subtree/result | Canonical exact known literals/contract fields; attach only matching local provenance when merging |
| QtProjectView(source) | Read-only registered UI/app Qt projection + baseline; new edits return to source-backed writer |
| ConfigManager source-backed load/save/SaveAs | Qt files/path/QSaveFile; source-changed/future/malformed/missing-provenance refusal; success-only baseline/path update |

Proposed future ownership (requires root source assignment):

| Owner | Exact proposed files/zones |
| --- | --- |
| P new portable adapter | adapters/project_storage/include/project_storage/project_source.h and src/project_source.cpp for narrow syntax/tree/edit handling, no Qt/domain; no new third-party package |
| P new desktop numeric mapping | adapters/desktop/include/desktop/numeric_project_document.h and src/numeric_project_document.cpp; typed literal helpers private here, consuming R DTOs |
| P existing released zones | adapters/desktop/include/config/config_document.h, config_migration.h; include/desktop/graph_document.h, project_display_settings.h; src/graph_document.cpp, project_display_settings.cpp; gui_config/include/core_app/config_manager.h + src/config_manager.cpp; apps/desktop_gui/include/core_app/core_application.h + src/core_application.cpp only lossless baseline/edit/path composition |
| P tests under future grant | Existing config_document/config_migration/graph_document_adapter/project_atomic_save/project_display_settings tests; new tests/project_source_tests.cpp and numeric_project_document_tests.cpp; console_config tests only agreed socket-free fixture; no new seam in product for tests |
| R | signal_contracts data_contract/data_frame/numeric_policy + domain configured numeric graph/descriptors, literal exact equality/structural validation, owning semantic DTO and explicit legacy mapping; live headers are not accepted |
| D | Kernel support/conversion/policy active semantics; P does not implement DSP or arithmetic policies |
| K | Numeric BOTH control DTO/codecs/schema/generated + shared reviewed boundary field tables; no project parser or opaque JSON |
| T | Pending device/epoch/request/revision and accepted processing update, owned numeric delivery/DATA/capture; no local lossless document writes |
| S/R application | Numeric ProcessingDTO/capabilities/admission/replay/state; no Qt/source-tree dependency |
| I | Every CMake target/test/export/install/source manifest, composition dependency wiring and consolidated app build/short checks after freeze; portable adapter need not be exported as DSP SDK |

Old strict APIs retain explicit legacy version dispatch; new numeric mapping does
not overload graph2/processing1 with unreviewed fields. Final signatures must use
R/S's actual owning DTO rather than assuming existing ProcessingDTO already supports
Converter/contracts. UI model serialization changes outside the listed composition
zone require root's exact caller grant; no ownership is inferred from this table.

R confirmed that N1a does not yet provide the owning numeric graph DTO. N1b owns
domain/include/domain/graph_definition.h and proposed processing_contracts.h for
resolution/owning parameters. Current DataContract channel/clock/schema spans are
borrowed. P/K boundaries must deep-own their DTO backing storage and rebase views
before application use; persisted documents never retain borrowed pointers.

Proposed file-path limits, separate from wire/DSP budgets:

| Resource | Loader ceiling |
| --- | --- |
| Original source / complete candidate | 16 MiB each |
| Container nesting / tree records / lexical tokens | 128 / 250,000 / 1,000,000 |
| Raw number token | 64 KiB; unknown tokens remain uninterpreted |
| Decoded key / raw escaped key | 1 KiB / 6 KiB |
| Decoded string / raw escaped string | 64 KiB / 384 KiB |
| Aggregate decoded text | 4 MiB |
| Edit records / owned replacement bytes | 8,192 / 16 MiB aggregate |
| Total owned peak | 64 MiB including original/tree, candidate/reparse tree, decoded text, patches and migration duplication |

Every checked size/offset/index allocation is charged before reserve; worst-case
tree record sizeof/alignment and migration source duplication must fit peak or
refuse. Counting/preflight precedes bounded arena allocation; allocation failure
returns a diagnostic without partial publication, using no exceptions as control
flow. This needs no general allocator framework. These
are configurable project-loader ceilings awaiting root/I approval, not device
defaults or promises that every ceiling can be simultaneously saturated. No
blind reuse of T's900000-byte message limit or K's codec arena. Larger existing
projects get diagnostic/no overwrite, never truncation or fallback Qt parsing.

## 8. Targeted characterization/TDD and acceptance plan — NOT RUN

First capture old behavior from current P7 source before moving reads/writes:
stable root keys/schema1/graph2/processing1, resolver ID-over-name, default and
explicit paths, legacy topology+inverse records, malformed opaque draft, old
UI normalization, per-device/preset/unknown merge, SaveAs success/failure and
QSaveFile commit denial. These characterize compatibility, not unknown64 exactness.

Then failing targeted tests for each new behavior, with literal source strings
and independent expected tokens/bit patterns, not encoder-as-own-oracle:

| Target / oracle | Exact cases and required outcome |
| --- | --- |
| project_source_tests | Valid grammar; strings/escapes/surrogates/UTF-8, nested object/array, trailing/truncated/comment/invalid number; malformed input/limits leaves output unchanged. Owned source survives caller release/move. Unknown duplicate subtree retained but ambiguous edit refuses. |
| numeric_project_document_tests exact64 | Int64 −9223372036854775808/9223372036854775807, UInt64 0/18446744073709551615; ±9007199254740991/9007199254740992/9007199254740993 (2^53±1),2^24±1. Decode/save/reload/DTO exactly same tag/value; overflow±one, plus/leading-zero/integer−0/exponent/fraction/wrong tag refuses before publication. |
| numeric literal bits | Float32 00000000/80000000/00000001/7f800000/7fc00001 and Float64 0000000000000000/8000000000000000/0000000000000001/7ff0000000000000/7ff8000000000001; lowercase width exact, uppercase/short/long/wrong branch refusal. Own-width bits roundtrip; active finite replacement rejects nonfinite, inactive literal persists bit-exact. R equality/replay tests belong R/S. |
| Whole-file lexical preservation | Unknown 9007199254740993,18446744073709551615,−9223372036854775809,1234567890123456789012345678901234567890,1e9999,−0,1.2300E+004 in root/nested object/array/node/UI/preset/device/migration records. Theme/edit/reorder save preserves each unchanged token exactly; normalized Qt display/default never replaces it. QJson-only/missing provenance save refuses with bytes/path unchanged. |
| Duplicate known keys | Equal and unequal schema_version/version/processing_version/graph_id/dtype/decimal/bits/scaling/registered params plus escaped equivalent key. Fail before Qt last-wins; unknown same-name opaque key untouched. Promote duplicated unknown→known during migration fails atomically. |
| Version/migration | Legacy absence vs null/string/fraction/zero/negative/huge/future; every approved combination; project upgrade without processing upgrade; R legacy mapping across stereo/channel/layout/kind/IDs. Exact inverses include unknown token spelling, explicit defaults and inactive fields. Partial migration fails on later device/preset and keeps whole original. Numeric-only/unrepresentable downgrade refuses. |
| Draft/applied/presets/projection | Invalid numeric draft stores unchanged; future/malformed recognized draft version protects file. Accepted typed graph alone updates applied, preserving separate draft/UI/preset IDs/opaque tokens; same graph/node/kind/unique edge match only. Foreign/reused/out-of-order/rejected update no old layout; T pending-state oracle is separate. Typed projection cannot carry UI/opaque fields in either direction. |
| Atomic/SaveAs/external edit | Real temp-file replacement denial + failed write/commit; original byte equality; success retry. SaveAs creates requested dir, updates absolute path only on success, preserves original source, refuses future/malformed destination. External change after load and during staging triggers SourceChanged/no overwrite; explicit reload/reapply leaves processing stopped/working until separately accepted. No cross-process CAS claim. |
| Limits and integration | Checked offset/depth/tree/text/source/candidate/provenance expansion and 32-bit narrowing; insufficient bound fails without partial tree/file/path mutation. After one frozen batch I builds app/DLLs + short config/projection/CLI socket-free integration, then fresh independent exact64/storage review. Legacy token/codec PASS not numeric acceptance. |

Tests use temporary directories/configs only, never user config.json/output.
No native UI, speakers, sockets, full suite, install or build is authorized by this
document-only assignment. At implementation stage processing test tones follow
the5–10 synthetic-tone rule without playback, if a pipeline case is needed.

## 9. Decisions, open issues and doc freeze

Chosen now: owned lexeme/tree before Qt; explicit source-backed edit set;
canonical known64 decimal/own-width float bits; inactive policy exact preservation;
single old-key file; whole-candidate migration/atomic refusal; source-changed guard;
no generic parser/dependency/database/implicit runtime reapply.

Open before dependent N2 source implementation:

1. Root's schema2/graph3/processing2 reservations are approved. Final combination
   admission and new JSON member names still require R's N1b owning domain DTO
   and the reviewed adapter field table; presence of version2 proves no support.
2. R/S freeze the owning numeric graph/parameter DTO and tested structural/active
   policy boundaries. R confirmed literal_equal/validate_literal, but their
   consolidated receipt and float copy/nonfinite payload tests remain pending.
3. Root approves proposed exact file ownership/loader envelopes; I measures new
   source/tree/migration peak and handles CMake integration. No source permission
   follows from this document, even if the portable adapter is independent of R.
4. Root assigns any UI caller changes needed to retain lossless graph edits instead
   of full QJson dumps. If a caller cannot furnish provenance, save must refuse
   until that boundary is updated; a private parser cannot fix a lossy caller.

Direct concrete questions were sent only to R and root; K/T were not awakened.
The original DOC-only pass used static reads/rg and was frozen as SHA256
33918C48D3BCA0A52BA6C0A90AE54DFD9DE82D4B9C267ED01DF12120E85329E5. Root accepted
that direction and then authorized the independent foundation slice below.

## 10. Actual lossless foundation freeze and author evidence

2026-10-07: root explicitly assigned only these new source/test files plus this
handoff document, with an isolated build-refactor-project-source directory:

- adapters/project_storage/include/project_storage/project_source.h
- adapters/project_storage/src/project_source.cpp
- tests/project_source_tests.cpp

AGENTS and the complete pinned DDD guide were reread before code. No subagents,
dependencies, installs, branch, commit, shared Ninja build or user files were used.
P7, numeric mapping/migrator, domain, ConfigManager, GUI, JSON schema and all CMake
files received no author edits. This slice has no application integration yet;
I owns that build/check stage after fresh independent review.

Actual public boundary: move-only OwnedProjectSource owns UTF-8 bytes, flat ordered
value nodes and decoded text; NodeView exposes borrowed raw/decoded/key views and
offset/parent/first-child/next-sibling handles. Handles contain a monotonically
allocated snapshot ID plus index, survive owner moves and reject another parse or
candidate; the ID never wraps. They are process-local provenance, not persisted
graph IDs. Unknown number tokens never enter a floating parser.

parse_project_source does an allocation-free syntax/count pass and then allocates
exact-sized source/node/text ownership with nothrow new. It validates strict JSON
numbers/literals/delimiters, whitespace, UTF-8, escapes, surrogate pairing and all
configured envelopes. Raw key/string limits exclude surrounding quote bytes;
decoded limits count UTF-8 bytes. Tree records are values, with object key metadata
on their value record; token count also includes keys and punctuation. No JSON
pointer engine, reflection, mutable generic value tree or schema registry exists.

find_unique_member and validate_known_keys operate on one explicitly addressed
object and decoded key names. Parsing preserves ordered unknown duplicates;
known-key validation diagnoses DuplicateKnownKey, lookup diagnoses AmbiguousKey.
Individual edits through a duplicated member or duplicated ancestor refuse, as
do ambiguous destination anchors. Explicit replacement of the whole unambiguous
parent remains possible with exact expected bytes. read_int64/read_uint64 use
checked canonical decimal accumulation only; integer -0/fraction/exponent and
overflow refuse without changing the caller's scalar output.

stage_project_edits addresses one immutable baseline simultaneously. Replace,
remove and move require exact target raw bytes; insert requires exact parent raw
bytes. Object inserts supply one JSON member fragment, array inserts a JSON value.
Move preserves the complete member/value slice between containers of the same
kind; it makes no assumption about application identity. All optional handles
must be canonical empty or belong to the source. Ancestor overlap, duplicate
targets, descendant/cyclic destinations, moved/removed anchors and introduced
duplicate object keys refuse independently of edit order. Untouched opaque
duplicates remain byte-exact. Changed containers rebuild delimiters/separators;
retained member key/colon prefixes and unchanged subtrees keep their exact bytes.
The unchanged root and outside-root whitespace are retained. No-edit staging is
byte-exact. A complete candidate is counted, rendered, reparsed and published only
on success; output may alias source, and every error retains old ownership/handles.

The accepted upper envelopes from section7 are implemented as configurable lower
ceilings, with larger-than-envelope values rejected as InvalidLimits. Checked
size multiplication/addition charges sizeof(Storage), exact source bytes,
sizeof(Node)*count and decoded bytes before allocation. Stage additionally charges
retained distinct output, sizeof(NodePlan)*source_count, sizeof(EditPlan)*edit_count,
borrowed DocumentEdit records and replacement bytes, then candidate bytes/tree/text
including reparsing. The candidate byte buffer transfers ownership without a
second byte copy. Existing output is retained and charged until successful
publication. There is no simultaneous-saturation guarantee. Peak means admitted
payload/storage allocations plus conservatively charged edits; allocator metadata,
bounded recursion stack and unrelated caller allocations are outside this ledger.
I must measure integrated process peak. Allocation failure returns a diagnostic;
the production translation unit compiles with exceptions disabled.

The authored tests use literal source strings and literal expected lexemes/errors,
not writer output as decoder oracle. Nine groups cover caller release/owner move,
foreign handles/offsets, exact huge integers/1e9999/-0/exponent spelling, malformed
and truncated JSON/UTF-8/Unicode, decoded escaped-key duplicates, fixed-width
min/max/overflow and both sides of 2^53, same/different-container moves, insert and
remove, opaque duplicates, expected-byte mismatch, order-independent conflicts,
late-batch rollback, alias output, every configurable resource class and exact
retained-output peak admission. Test-only global allocation interception fails
each actual nothrow allocation: four parse allocations and six staging allocations;
all failures preserve bytes/handles, and the next allocation threshold succeeds.
No fault-injection API was added to product code.

| Actual log under build-refactor-project-source | Build / run evidence |
| --- | --- |
| red-build.log, red-run.log, red-exits.log | Actual direct current-source stub build0 / run1; six behavioral groups failed before implementation |
| green1-build.log, green1-run.log | build0 / run1, five groups passed; remaining assertion assumed removal of source prefix spaces |
| green2-build.log, green2-run.log | build0 / run0, six groups passed after correcting that literal whitespace expectation; stricter conversion warnings then fixed explicitly |
| green3-build.log, green3-run.log | build0 / run0, eight groups including per-allocation failure and depth/key boundary cases; warning-free with Werror |
| final-build.log, final-run.log, final-exits.log | Final current-source build0 / run0, nine groups; no stale executable fallback |

Initial RED test hash is saved in red-test-sha256.txt. The sole initial six-group
oracle correction retained original spaces after key colons; it did not weaken
lexeme/ownership/error checks. Later additions expanded fault/boundary coverage.
author-before.txt records the three source/test paths absent and the prior doc
hash; author-after.txt records the final four-file SHA256 freeze. run-final.ps1
contains the reproducible direct compile/run command using the already installed
CLion MinGW compiler. Output executable has a distinct final name and executes
only after successful compilation; build and run exits are separate. No CMake
configuration or app/UI/socket/audio test was performed.

This is author unit evidence, not independent acceptance. Risks/open boundaries:
direct key lookup/duplicate edit checks scan addressed object members and are not
a performance guarantee for pathological large edit batches; no load/FPS benchmark
was run. Budget refusal and allocation faults are covered, integrated allocator
overhead/process peak is not measured. Error offsets for supplied fragments are
relative to that fragment; future numeric mapping must attach its field/edit path.
Known-key registration, version admission/migration, graph identity reconciliation,
canonical tagged numeric mapping, file atomicity/external edits and GUI baseline
ownership remain separate N2 work. Root receives this source freeze for fresh
independent review, then I integration; P returns idle after that report.

## 11. V1 source-backed numeric project block (P author handoff)

The later V1 block implements the complete cold storage path above the accepted
source foundation: typed node/graph snapshots, load, draft edits, Save/Save As,
explicit legacy migration and exact supported inverse. The storage adapter owns
raw source; portable DTOs own only registered typed behavior. Unknown numbers and
descendants never pass through a whole-project Qt JSON roundtrip.

Owned implementation/test files are `numeric_project_document.h/.cpp`, the two
`core_app/config_manager` files, `legacy_processing_adapter.h/.cpp`, and the two
focused tests. I registers these sources in existing desktop/domain targets and
firmware manifests; P does not edit composition roots or run shared Ninja.

- Canonical wide integers use dtype/decimal; Float32/Float64 use exact bit text.
  Contracts include shape, semantics, coordinates, representation and clocks.
- Node/graph helpers are the shared clipboard/undo/editor serialization seam.
  Source staging reconciles stable node/port/parameter/edge identities and keeps
  unrelated applied graph, UI, camera, presets and opaque source slices.
- `ConfigManager::loadNumeric` reads canonical graphs; its explicit ingress/limits
  overload upgrades supported graph1/2 or standalone old projects in memory only.
  Known future/ambiguous/malformed source refuses before fallback/write.
  `read_project_graph_edition` supplies validated startup classification.
  Edition0 denotes valid schema0/1 settings-only input eligible for legacy
  defaults; schema2 without a scene draft remains an error.
- `readNumericSettings` is a safe core/ui/WLED read projection. `numericSettingsDelta`
  compares normalized known UI state; pass its result as presentation
  `application_settings`, together with positions/editor_view, to numeric Save.
  Unknown unsafe leaves remain source-owned. Existing array/preset structural
  edits or opaque replacement return an explicit error, rather than silently
  dropping the user edit. New absent settings containers can be inserted.
- Save publishes path/source/baseline only after QSaveFile commit; externally
  changed active source, unsupported destination, or replacement failure keeps
  file and owners unchanged. Explicit Save As can replace a supported existing
  destination. The final precommit recheck is best effort, not cross-process CAS.
- Legacy mapping is a single portable helper using explicit caller contracts and
  the common resolver. Mean/RMS produce Float64, history count UInt64. Generated
  scalar outputs continue into declared old sinks. Optional trusted previous
  canonical contracts allow disconnected inputs only for the same graph/node/
  kind/input ID. Caller must exclude device/clock changes; current parameters and
  output contracts are resolved again. Missing/foreign baseline refuses.
- Graph migration provenance restores original lexical source only when the
  entire mapped graph is unchanged. Standalone project inverse also restores
  outer whitespace. Edited/unsupported inverse refuses loss or narrowing.

Author evidence is in `build-refactor-numeric-project`: initial mapper and shared
legacy helper stubs were RED (build0/run1); focused current storage tests and
legacy helper tests have separate build/run logs and exits. The focused package
includes exact bounds/bits and all parameter alternatives, unknown huge numeric
tokens, real Windows denied atomic replacement plus retry, Save As/path rollback,
future/external edits, old graph1/2 load/save/inverse, standalone inverse, settings
and layout persistence, and explicit unsupported settings-edit rollback. The
portable helper test covers the actual old transform chain, missing ingress,
unsupported representation/resource limits, Mean/RMS-to-sink and trusted
disconnected-baseline identity/refusal cases.

These are author checks, not independent acceptance. I still owns the integrated
application build and focused integration checks; root owns fresh independent
review. No GUI/native rendering, speaker playback, network or long benchmark was
run by P. The source foundation ledger is bounded; total mapper/Qt allocation
overhead and process peak are not claimed to be measured.

V1 follow-up closes the existing `ui.pipeline_presets` array refusal recorded by
I. The registered writer reconciles stable preset IDs (unique names only for old
ID-less records), graph node/port/parameter/edge identities, and nested typed-node
strings against owning source. Surviving unknown slices and unchanged lexical
numbers remain raw; explicit removal affects the selected preset only. The safe
settings projection retains identified entries whose unknown leaves cannot be
represented by Qt. Preset version/identity, graph editions and typed snapshots
validate before publication. Other unsupported opaque/structural settings edits
retain their refusal boundary. No editor/composition/portable helper changes.

Evidence under `build-refactor-numeric-project`: `presets-characterization.log`
records actual first-save success / atomic second-save refusal; `presets-red-*`
records build0/run1 with the new second-save success assertion failing. The current
focused package verifies append/edit/reorder/remove, Save/reload/Save As, exact
UInt64/Int64/float bits, opaque numeric lexemes at preset/graph/node/view and inside
typed strings, applied/UI isolation, and duplicate/future/malformed/external-change
rollback. I owns the affected real editor save/load/remove/restart integration and
application rebuild; root owns combined independent acceptance. Earlier array
refusal descriptions above are historical and superseded only for this registered
path, not a general permission to rewrite opaque arrays through QJson.

The first follow-up fixture used a visual graph2 envelope; I's unchanged real
editor oracle exposed that `NumericEditorGraphModel::save()` actually stamps the
visual envelope graph3/processing2. The corrected owner fixture executed RED at
the first preset Save (`visual-presets-red-*`, build0/run1). Preset admission now
distinguishes canonical nodes from visual `internal-data` nodes, validates the
corresponding encoding, and refuses mixed representations. Canonical decoding is
unchanged. Current focused tests use the actual visual3 fields and retain a
separate canonical3 opaque preset; I/reviewer own the real editor rereview.

Current direct user override supersedes the compatibility scope above: the
implementation now accepts only document2/graph3/processing2, preserves unknown
source within that format, and rejects old editions without migration or overwrite.
Migration/inverse APIs and old ConfigManager load/save/Save As are removed. The
portable legacy mapper and its test were deleted after R removed its usage and I
removed registration. No legacy preparation/contextual domain extension was added.
Canonical and actual numeric visual3 presets remain supported distinct adapter
representations. Recognized graph structure, typed snapshots, stable preset IDs
and editions validate before source publication. Optional presentation
`applied_graph` updates only the explicitly supplied successfully applied snapshot;
`read_numeric_applied` returns an optional owning DTO without performing Apply.
Absent updates retain the applied raw source. `numeric_project_path` preserves
path priority without depending on old format migration helpers. Intermediate
tests were stopped by the user; mixed historical test fixtures remain for scoped
cleanup in the final verification phase after all canonical parts are connected.
The historical GREEN logs above do not describe or accept this current changed
implementation. I owns the final composed application build and verification.

## FINAL canonical storage verification — 2026-10-07

Author verification of the connected current API, not independent acceptance.
Legacy-only migration/vertical expectations were removed from the mixed numeric
storage tests. Retained scenarios use document2/graph3/processing2. Added checks
cover separate draft/applied reload and atomic failed publication, exact owning
canonical/visual3 editor snapshots and edges, registered UI vector lexical
preservation and stable-ID list reconciliation. The actual CoreApplication preset
first/repeated save/load/remove/SaveAs/restart oracle keeps its original expected
counts and now checks UInt64 max, Int64 min, Float32 NaN bits and Float64 minus zero.

I built the current targets in `build-refactor-integration`. Its final storage
CTest log records document and ConfigManager PASS. Direct author runs also exited
zero (`build-refactor-numeric-project/final-document.log`, `final-config.log`).
The two actual application scenarios first timed out because the deployed bin
contained only the Windows platform plugin. The same unchanged binaries passed
with the installed offscreen plugin path; no product or expectation relaxation:

- `final-application-retry.stdout.log`: canonical application/presets/restart PASS.
- `final-rejected-retry.stdout.log`: rejected startup PASS.
- Associated `*.stderr.log` retain Qt diagnostic output.

Repeatable environment: Qt 6.10.2 MinGW bin on PATH, QT_QPA_PLATFORM=offscreen,
QT_PLUGIN_PATH=C:/Qt/6.10.2/mingw_64/plugins and
QT_QPA_PLATFORM_PLUGIN_PATH=C:/Qt/6.10.2/mingw_64/plugins/platforms.
Executables are in `build-refactor-integration/bin`: numeric_project_document_tests,
numeric_config_manager_tests, numeric_application_tests, and the latter with
`--rejected`. I owns shared build/CTest environment and obsolete helper registration
cleanup. Both independent reviewers received commands and embedded source fixtures.
Offscreen results establish the bounded editor/storage scenario, not native 3D.
## Current storage/preset inventory — 2026-10-08

This packet is implementation-ready; the historical checks above do not accept
its new presentation/routing scope. I owns current Release configuration, build
and execution; G/U own the actual GUI view/tab/preset switching oracle.

| Previous source | Current disposition and retained predicates |
| --- | --- |
| `project_source_tests.cpp` | Retain `project_source_tests`: bounded owning parser, exact raw lexemes, duplicate lookup, edit transaction and allocation-failure rollback. |
| `numeric_project_document_tests.cpp` | Retain `numeric_project_document_tests` and macro target `numeric_config_manager_tests`; add preset display snapshot save/reload, empty known-state replacement, applied isolation, future/malformed rollback and separate node adapter full replacement/clear/partial draft/unknown raw fields. |
| `numeric_application_tests.cpp` | Retain default, `numeric_application_rejected` and `numeric_application_first_run`: actual preset save/load/delete/restart, exact numeric policy, separate applied state, startup file protection and silent first-run delivery. New actual view/tab switch acceptance belongs to G/U and I workflow fixtures. |
| `console_config_tests.cpp` | Adapt and register same target: canonical TestSignal project, stable ID priority over name, unique name, ambiguous/missing/no-config refusal, actual CLI read-only behavior and no implicit registry connection, malformed/future/raw graph document refusal, explicit serial option errors. Old raw visual2 UART fixture is retired; canonical transport recovery is the transport owner's engine/stream transport scope. |
| `config_document_tests.cpp` | Retire `configuration::resolve/overlay/load` API and standalone v1/v2 acceptance. Useful identity/no-fallback predicates are in `console_config_tests`; unknown fields and disk roundtrip in storage/config-manager suites; invalid preset and protected startup in storage/app suites. |
| `config_migration_tests.cpp` | Retire every migration/up/down/inverse/generated-node/restored-legacy assertion and old manager load/save API. Retain ID-before-name via CLI; malformed/future versions, atomic output/source/file refusal, Save As active path rollback, raw field preservation and settings-only canonical project behavior via storage/config-manager/app suites. Old root/UI preset precedence and schema1 recovery are not canonical API behavior. |
| `graph_document_adapter_tests.cpp` | Retire legacy GraphDefinition/GainParameters/NetworkParameters and visual1/2 DTO helpers. Retain typed canonical/visual3 identity, parameter exactness, duplicate/mixed/malformed rejection and processing exclusion of UI/adapter fields via storage suites and numeric processing contracts/editor tests. |
| `project_display_settings_tests.cpp` | Retire old ProcessingDTO capture/merge, legacy type aliases and network parameters inside DSP. Retain same-ID/type layout, changed type/deleted node cleanup, changed edge extension cleanup, foreign graph refusal, unknown raw preservation and processing/UI separation in `retained_layout_identity_oracles` plus existing storage cases. Per-node parameter admission remains numeric processing/runtime owner scope rather than storage admission. |
| `project_atomic_save_tests.cpp` | Retire old schema1 writer/overlay. Retain actual Windows replacement denial and retry, baseline/path/source rollback, external modification/deletion refusal, malformed/future target protection and Save As rollback in storage/config-manager suites. User files are never fixtures. |
| `application_lifecycle_tests.cpp` | Current source is already canonical: NumericGraphPreparationPort/GraphLifecycle fake ports, prepare/apply/start/stop fault rollback, pending retry/discard, memory overflow, connect/disconnect intent. Retain, do not classify obsolete. R/application owns it; I must register/link graph_application/dataflow_runtime/desktop_numeric_defaults if not already registered. |

Removed registrations do not authorize deleting historical source or evidence.
Current source changes are limited to P storage/config guard/tests/format docs;
CLI product changes and CMake registrations belong to I. No user config/output,
shared Ninja, desktop capture, input or foreground-window actions were used.

Independent inventory review owner fixes (source-only, pending I execution):
`console_config_tests` now places the name-colliding record first and gives it a
separate canonical graph rejected by runtime admission (self edge plus occupied
input). Direct selection of its ID must return 3, while the colliding stable ID
and its unique name must return 0. This distinguishes wrong name-first selection.
`manager_save_as_protected_targets` in `numeric_config_manager_tests` covers
existing malformed JSON, future99 and malformed canonical graph destinations:
exact target bytes, source, disk baseline, active path and manager path survive
refusal; supported destination replacement permits retry, followed by real
reload of the edited draft/layout and retained unknown raw fields.

## Central functional inventory ownership handoff — 2026-10-08

Root assigned P exclusive classification ownership; I acknowledged and stopped
JSON writes. P reconciled the existing baseline167, current rg155 and delivered
sources into 170 unique paths. There are zero missing baseline/current paths,
zero duplicate paths and zero pending owner dispositions. New materials are
numeric_shutdown_tests, numeric_desktop_host_tests and the generated Python cache.
P merged exact U/G/R/T/K/E owner predicates and I app/worker packets, current
source declarations and read-only generated CTest registrations (86 names in the
recorded snapshot). Production/CMake/user files were not changed for this audit.

Frozen classification: `cmake-build-release/functional-test-inventory.json`
SHA256 `AE7B07D1B028A1F484543D52007BE77DFCABC597EA74A35C8F42BE5F0EE5415F`.
P transfers subsequent JSON ownership to I for final current run attachments and
to the independent reviewer for semantic audit feedback. P makes no further JSON
edits after this handoff without a new ownership assignment.

The source ledger distinguishes actual canonical adaptations and retained tests,
unsupported old API/grammar with exact replacement predicates, historical
fixtures/evidence, build/CI/support material, fake-IDF interfaces, bundled
upstream hardware tests, generated cache and explicitly excluded native/music/
Linux actions. Paths/hashes inherited from earlier source snapshots are marked
not-final-freeze; active I worker source hashes were not refreshed. Only the
frozen classification JSON hash above is current handoff identity.

Audit gaps produced concrete source actions: I registered current bounded
performance_tests and rewrote runtime_tests as current LatestDelivery coalescing/
cancellation/poll/timer lifetime checks. G added exact old camera/Line/Freq
predicates to graph_panel_axes_tests::retained_renderer_oracles, called from main.
All these source changes require final integrated execution. Old runtime portable
History/FFT/lifecycle predicates remain subject to R/I semantic audit; registration
or a file's legacy name does not prove coverage or obsolescence.

Every final integrated run and independent acceptance field remains pending; no
old author PASS was promoted. SDK host/fake component software results, offscreen
charts, fake UART and internal silent sources never imply ESP-IDF hardware,
native GL, physical audio or actual remote CI success. CI's historical native tone
and non-preset build commands are recorded as limits, not authorized local runs.
Root's full classification+execution+independent objective remains OPEN.

## Console fixture repair, 2026-10-08

Actual I functional-ui CTest recorded console_config_tests exit1 with blank output.
P reproduced that binary unchanged. The fixture supplied unknown `aggregator`
through registered application_settings writing, which correctly refuses that
section before starting any CLI process. The corrected fixture composes only
registered settings, then reads a canonical source containing the unknown section;
its no-implicit-registry and byte-for-byte read-only checks remain intact.
Release-visible std::fprintf diagnostics now record child arguments, expected and
actual status, output, and fixture exceptions.

After I rebuilt the first repair, P direct execution reached the UART case:
`--serial-port __codex_missing_uart__ --duration 100`, normal exit3 versus stale
expected2. Earlier canonical load, stable-ID precedence over a distinct collision
preset, runtime rejection of that graph, display-name selection, ambiguous/missing
preset and missing-config cases completed successfully. Current serial_config.h
admits an explicit UART processing endpoint without input_node; main.cpp returns3
when that endpoint cannot reach applied state within duration. The final fixture
expects runtime3 here, retaining invalid baud and conflicting transports as2.
Final source SHA256:
`BD009417E1D7E1086148B3C66AA62E5486885602E0C609FFC380400CD9260A01`.
I owns final rebuild and execution; no whole console PASS or independent acceptance
is claimed at this source handoff. No CLI production defect was established.

P also clarified shutdown reading: read_numeric_presentation returns graph/layout
presentation only; read_numeric_application_settings supplies ui preferences and
workspace. I repaired its fixture and reports current workflow/shutdown PASS.
For G workspace Save refusal, P retained graph_views object validation and sent G
the mutable read-only operator[] candidates in node_views.cpp; missing keys can be
inserted as null and copied into capture. G owns renderer/capture repair and its
final topology acceptance; P does not weaken malformed-source admission.

## Shipped examples and autosave follow-up, 2026-10-08

The supplied UI entrypoints are missing-file first-run (numeric_test_graph),
Default (numeric_default_graph with AudioSource), Generator example (actual
TestSourceCommand replacing the default source), and My presets /
builtin-test-signal (create_numeric_startup_project's canonical snapshot).
The generic node catalog belongs to U; historical tests/fixtures/test-signal.json
and config.pre-numeric.json are not current supplied UI presets and were not
rewritten or admitted as legacy input.

I built the first new probe; missing QCoreApplication include was repaired in the
P helper. Diagnostic build2/run2 reached real positive changing PCM/Window/Windowed/
StereoFFT/SpectrumMerge/Mel/History and view inputs for missing-file and Default,
then failed an overly strict edge-vector ordering assertion after source replacement.
The oracle now checks an exact edge permutation (same endpoints, ports and count).
Actual I shipped-examples-diagnostic-build3.log build0 and
shipped-examples-diagnostic-run3.log exit0 verified all four UI selections followed
by a separate NumericEngine Apply/Run with injected eight changing silent tones.
History rows/count/ordinal were 5, 7, 7, 5 respectively; every processing stage and
view input had at least five finite positive changing observations. Default's
requested SystemOutput adapter identity was retained by the injected provider;
no physical device was opened. All twelve nodes and eleven links were retained,
with twelve distinct positions. This is bounded backend/canonical-source evidence,
not acceptance of the current CoreApplication's GUI Run workflow.

The final P extension --shipped-examples-ui adds actual named Run/Stop clicks in
that CoreApplication, all eleven stage/History/view callbacks from its own engine,
exact epoch/revision and contract fences, running/stopped state, count/ordinal and
warning clearing. Autosave tests separately retain saved draft versus previously
applied graph with auto-apply false, and exercise the actual auto-apply checkbox,
registered literal editingFinished and UI timer with auto-apply true. Invalid
registered edits and explicit Run preserve the old Running graph; successful
recovery clears the warning in both actual EN/RU language selections. Position-only
saves must preserve processing revision even after waiting past the timer interval.
These new UI/autosave assertions await I integration of U Run/Stop/capability/status
changes and actual execution. No default graph product change or whole acceptance
is claimed from the separate backend probe.

Actual save path inspected: CoreApplication::_save_configuration stages the draft
and the presenter's last accepted applied graph separately, clears only storage
modification state, and never sends Apply. Not-applied status comes from export/
admission/failed Apply; editor warning clearing previously depended on its own
request ID and ignored the Applied status branch. U owns the status repair.

Readonly current configuration diagnosis, 2026-10-08: actual
cmake-build-release/bin/config.json SHA256
B8917DAB7BBD02868CFFAD7474C216DAD7358DA6F27C87311EB41E6527501EC3
contains scene_graph 13 nodes/9 links with TestSignal1 output Int64 Scalar,
rank Scalar, channel[0], extent1; Converter14 input matches that scalar but
its output is Float32 MelEnergy Vector50. TestSignal requires PCM; the Converter
cannot change semantics, rank or dimensions. Source links 1->Window2 and
1->AudioView9 are absent. Saved pipeline_graph remains the supported 12-node/
11-link Float32 PCM stereo256 full chain. This establishes edited saved draft
versus previous saved applied state, not the live user's process state.

Both ui.autosave_enabled and ui.pipeline_auto_apply are absent in this source.
Actual ConfigManager defaults resolve both to true (config_manager.cpp113/133;
types.h18/33); an earlier tentative false auto-apply default was explicitly
corrected to root/I. Thus autosave can persist this inadmissible draft while
application admission refuses replacing the saved valid applied snapshot.
No user file was written or live process queried/controlled. U owns preventing
new invalid registered edits and presenting the existing invalid draft clearly.

Final current P author/integration evidence, 2026-10-08:
I startup-editor-repair-build.log completed 11/11 build0;
startup-editor-repair-ctest.log actual 5/5 PASS, 15.94s (P's two modes plus U's
three editor cases). CTest log SHA256
BCA25E5A928A58A48B2A72BE53CD4AA0B0B2112338D1CFE813861AEC1CC2191B.
The prior startup-editor-integration 3/5 result remains historical RED; it
established four own-CoreApplication GUI examples PASS before autosave failed.
The final unchanged P predicates pass after U repaired authoritative invalid Apply
status and its toolbar fixture; P added exact diagnostic lines without dropping
real timer, draft/applied, rejection, language or recovery assertions.

Current GUI missing-file / Default / Generator / builtin-preset cases use one
CoreApplication's service epoch 1073617108394700 and revisions 2/3/4/5. Final
History row/count ordinals were 7/5/6/8 respectively. All eleven processing/view
stages had at least five finite positive changing observations, under exact port
contracts and epoch/revision fences. Actual named Run/Stop entered Running then
Stopped; no frames were synthesized into GUI signals. Full output is retained in
startup-editor-repair-ctest.xml and Testing/Temporary/LastTest.log (the latter is
transient). Default uses the real requested SystemOutput adapter identity with
internal silent eight-tone injection; physical capture/native GL are unverified.

Autosave OFF now uses the actual autosave checkbox and registered amplitude
editingFinished, then waits for the real autosave timer to change disk bytes and
persist the edited draft while the old applied snapshot/revision remain unchanged.
Position changes likewise use the ordinary nodePositionUpdated/save timer and
verify exact stored coordinates, without manually emitting a save signal.
Autosave ON plus auto-apply ON uses the actual checkbox, registered edit and
apply timer, in EN and RU. Invalid registered Window edits are atomic; explicit
Apply/Run of an unsupported loaded draft keeps the previous Running revision6 and
applied graph. Valid recovery clears the warning; presentation-only timer saves
never produce a new processing revision. All these assertions passed current I
execution; no author PASS is promoted to independent acceptance.

Frozen P source/binary handoff:
- tests/numeric_application_tests.cpp:
  808B5FB6A8C1995671703EBDB1F12B996F5EC43E3761FC4365CA642D54AA6DC0
- tests/shipped_examples_probe.h:
  49CE567605E69AD000A31B71683BE3994FD7B78AA52005CA9EABF69B5A51A4A2
- tests/shipped_examples_gui_probe.h:
  BD7DD1CD82E8EC78F02E6A56FE6314AF60A6B988E5EA8CD01A14B31F80A713CB
- cmake-build-release/bin/numeric_application_tests.exe:
  4C59B2C1086D9D96ECB7082220E02E04E8705F8520CCCAC84FF84BD11FE2D005

P makes no further source writes after this handoff unless independent findings
require owner fixes. I owns final common app build/ledger; independent bounded
shipped-example/autosave acceptance is pending. The user's source file and live
process were neither modified nor controlled. No P default graph/product fix was
needed: the supplied canonical examples were supported; U fixes new node defaults,
invalid contract edits, explicit execution and diagnostics.

Independent bounded acceptance, 2026-10-08:
Reviewer a4d independently reran five current canonical cases, each exit0:
numeric_editor_gesture_tests, numeric_editor_model_tests, editor_tests,
numeric_application_tests --shipped-examples and --shipped-examples-ui.
All 29 startup-editor-final-hashes.json entries matched before and after review.
The final canonical application build was I 22/22 exit0; its selected regression
run startup-editor-final-ctest.log/XML was 15/15 PASS, 41.48s. P binary remained
4C59B2C1086D9D96ECB7082220E02E04E8705F8520CCCAC84FF84BD11FE2D005;
core_app hash was
42DA7B0C84971F2744F544C30EF927E9B666F15084B95A58DB6EDD81A1A51044.

Reviewer accepted the supplied examples/manual ports/admissibility/Save-vs-Apply
scope: four own-CoreApplication Run/Stop paths, all eleven stages finite positive
and changing plus History count; exact second-output QtNodes gestures; reported
Int64 Scalar refusal and explicit repair; atomic registered forms and backend
admission; real autosave timer draft/applied distinction, stable presentation-only
revision, invalid Apply/Run preserving Running, RU/EN auto-apply recovery.
No actionable source finding remains in this assigned bounded scope.
Evidence: cmake-build-release/review/desktop-review.md final bounded section and
review/run-examples-final-*.log. This accepts only the listed source/execution
scope: Default capture used synthetic injection, native/hardware and global backend
acceptance are not implied. Root owns checklist acceptance; I owns the sole ledger.
