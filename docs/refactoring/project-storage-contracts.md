# P3 project storage and typed projection

Authority: accepted Gate1 contracts, implementation-plan.md and ProposedSlice3
ownership, plus coordinator authorization for independent storage work on
2026-10-07. Full AGENTS.md, architecture map and pinned
[Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md)
read before implementation. This reference defines responsibilities, not a new schema.

## API handoff to coordinator / T3 / K3

`desktop::project_processing_dto(QJsonObject)` returns an explicit result with
`application::ProcessingDTO processing` and `QString error`. It reads graph v2,
processing v1, keeps graph/node/port IDs and admits only domain descriptor
Processing and ApplicationAdapter parameters. Presentation and OpaqueExtension
fields (including nested JSON) never enter the DTO. Network legacy `type` is
translated to registered `data_type`; all unknown JSON remains in the local input.
After mapping, existing domain parameter validation rejects invalid enum/range/
geometry/cross-field values, symmetrically with the serializer. Admission against
actual device capabilities remains in the application service. Local drafts and
presentation values are not subjected to this processing parameter validation.

`desktop::processing_dto_document(ProcessingDTO)` returns processing-only graph
JSON or an error. It strips view selectors, emits registered fields and canonical
edges, checks supported version/identity/finite parameters before returning output.
This is an adapter representation, not a new wire schema. K3 consumes accepted
application DTO, and must use domain descriptors without duplicating classification.

`capture_project_display_settings(QJsonObject)` captures a local draft after
checking unambiguous graph/node/kind provenance; draft processing need not be valid.
`merge_project_display_settings(accepted DTO, captured settings)` returns local
display/extension settings merged with typed processing, matching graph ID + node
ID + stable model kind. Foreign graphs and reused node IDs with another kind get
no old layout. Both functions are pure adapters; no Prepare/revision/connection/
device persistence/runtime history is invoked or stored.
Connection extensions are retained only for a unique exact pair of node+port
identities whose endpoint model kinds still match; rewired edges get no old fields.

T3 alone associates captured settings with device/epoch/request/revision and calls
merge on the matching typed accepted reply. Reject/timeout/disconnect/new requests
are T3-owned pending-state transitions. P3 does not integrate remote callers until
their handoff. Existing `project_processing_graph` JSON compatibility helper keeps
its Gate1 opaque-field behavior and is **not** the strict typed control API.
Neither processing representation is a complete project for saving.

## One document

Existing schema_version=1, graph version=2, processing_version=1 remain independent.
Logical processing/applied (`pipeline_graph`), draft (`scene_graph`), presentation
(`ui`), presets and app/device fields retain their existing physical keys. No second
JSON or new competing envelope is introduced. `configuration::active_path` returns
one absolute exe-adjacent default or explicit path; successful SaveAs changes it.
Unknown fields, malformed opaque draft values, explicit defaults and old preset IDs
remain local. Runtime frames/history/session tokens are never synthesized by P3.

`configuration::save_atomic` validates candidate and existing destination, refuses
malformed/future versions, uses QSaveFile with direct-write fallback disabled and
commits once. Up/down migrations keep their recorded exact inverse or explicitly
refuse lossy edits. Tests use temporary directories only.

## Initial snapshot evidence / integration (historical)

Initial characterization compiled and passed config_document_tests,
config_migration_tests and graph_document_adapter_tests in the isolated
build-refactor-project-storage-slice3 directory, using compiler/Qt from existing
build-refactor-integration cache. New display test RED is recorded in display-red.log.
The final batch `./build-refactor-project-storage-slice3/run-targeted.ps1`
compiled current sources and ran **6/6 PASS**, exit 0:
config_document_tests, config_migration_tests, project_atomic_save_tests,
graph_document_adapter_tests, project_display_settings_tests and the existing
CONFIGURATION_MANAGER_TESTS variant of config_migration_tests. Assertions enabled
(-UNDEBUG). No Ninja, app launch, capture, speakers, sockets, user config or output.
`targeted-final.log` records each compile/run exit. Source freeze hashes are in
`source-hashes.txt`. Windows temp-file handle denial reproduced an actual atomic
commit failure and proved original byte preservation; after handle release the
same candidate saved successfully. Future/malformed source and future candidate
refusals preserve originals. Existing migration/SaveAs/default-path tests passed.
All 26 descriptor kinds are covered by typed parameter roundtrip/membership checks.
`edge-preservation-red.log` is the executed assertion failure before the local edge
extension preservation fix; the final display test passes it and port-rewire refusal.

Only graph_document.h/.cpp, new project_display_settings.h/.cpp, the two new test
sources and this document were changed by P3. ConfigManager/config migration/
config_document/application composition were retained as accepted Gate1 behavior.

I owns all CMake changes: graph_document_adapter now needs application/include
(prefer PUBLIC graph_application) for accepted ProcessingDTO header; register
src/project_display_settings.cpp in that adapter, and new
project_display_settings_tests + project_atomic_save_tests (Qt Core, adapter as
needed). Whole desktop build/deployment and short affected integrations follow
source freeze under I's single build ownership. Author GREEN is not acceptance.

Status: **P3 INDEPENDENT STORAGE SOURCE FROZEN / AUTHOR 6 GREEN / I INTEGRATION
AND INDEPENDENT REVIEW PENDING**. No claim about remote pending-layout behavior,
whole desktop integration, codecs, native rendering or Gate2/Gate3 acceptance.

Workflow lessons: consume the existing schema and domain classification; validate
boundary/provenance before merge or persistence; combine target checks and hand
one frozen source batch to the integration owner.

## S1/S2 consolidated repair

Independent build-gate3-storage-review/review-report.md found two P2 defects in
the initial snapshot: inbound parameter validation asymmetry and a mutable Qt
value-reference alias assignment. Coordinator released the P3 source freeze for
one repair package. `project_processing_dto` now validates mapped processing nodes
through domain::parameters_valid after descriptor filtering. Network legacy type
is materialized as an owning QJsonValue before inserting data_type; explicit
data_type retains precedence. Existing legacy reader, raw storage/migration,
capture/merge and public APIs remain unchanged.

The consolidated repair evidence, commands, root causes, adjacent-path audit,
limitations and latest freeze manifest are recorded in
../../build-refactor-project-storage-slice3/repair-report.md. The original
independent oracles remain unchanged and the initial source-hashes.txt/RED evidence
remain historical; source-hashes-repair.txt is the repair freeze manifest.
