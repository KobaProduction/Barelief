# Independent final acceptance brief

This is the coordinator's assignment checklist, not an acceptance report.
Create three new chats after primary slice findings are closed. Do not reuse
an author or primary reviewer for the corresponding final review. At most three
working/review chats may be active simultaneously.

Every reviewer starts with fresh context. Read AGENTS.md, the full pinned
clean-architecture-ddd-reference.md, architecture.md, architecture-map.md,
network-stream-v2.md and the user's migration requirements. Inspect current
source and the actual diff against 3de4de8, including untracked canonical files.
Do not read author conversations or treat their conclusions as evidence.
Reports describe the source hashes/commit checked, own commands, results and
limits. Keep production and shared tests read-only; use a private build/probes.
Do not run shared Ninja, commit, publish, install SDKs, launch native/device QA
or perform long stress tests. Use synthetic sources and loopback endpoints;
preserve config.json and output/. Save a local report and finish with its path.

## Architecture and portability

Own report: docs/review-final-architecture.md. Check dependency direction,
responsibility, actual shared executor/lifecycle composition, public API and
memory ownership. Verify C++20 portable targets without Qt/miniaudio/OS networking
or ESP-IDF, bounded prepared resources, no mandatory exceptions/RTTI, installed
SDK consumer and firmware source/component topology. Identify unused bridges or
parallel old execution implementations. Source/component host checks do not
prove ESP-IDF cross-build or optimized backend integration.

## Protocol and runtime

Own report: docs/review-final-runtime.md. Check unchanged wire framing/payloads,
caller buffers, partial reads/writes, reconnect and overflow, semantic signal
contracts, unavailable/discontinuity, source clocks, bounded memory and prepared
allocation behavior. Verify Apply continuity, bypass/disconnection/Stop
distinctions, async ownership/subscription/Stop fences and actual distributed
execution. Review desktop UART and ESP transport against the same codec; physical
serial and hardware acceptance are separate.

## Desktop and regressions

Own report: docs/review-final-desktop.md. Check composition, settings and unknown
JSON preservation, themes/localization, default workspace, graph slots/floating
panels, inspectors/splitters/footer, hidden rendering and explicit remote Stop.
Review automated fixture changes for preserved intent and positive ACK/state
checks; no native capture, LAN output or assertion-elided Release tests. Own short
offscreen probes are permitted. Offscreen is not native OpenGL/theme/window-manager
visual acceptance.

## Findings and closure

List each finding with severity, exact source location, requirement violated,
reproducer and expected/actual result. Give READY or NOT READY for assigned scope;
never infer other reviewers' acceptance. Coordinator returns defects to owners,
then reviewer repeats the changed scope independently. Full short desktop and
portable suites, final rebuild/DLL verification and coordinator requirements audit
remain required after these reviews.
