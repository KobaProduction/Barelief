# Typed pipeline runtime

`GraphProgram::compile` validates an immutable execution plan. Version 1 documents
migrate to explicit Window → WindowFunction → StereoFFT → BarkWeighting stages.
Sources, histories, views and NetworkOutput instances can have independent state.
The standard graph uses real `AudioSource` with `source=0` (system output), never
TestSignal. The constructor opens no device. Applying a valid graph automatically
starts its enabled local sources, including the first identical default Apply.
Explicit Stop suppresses automatic starts until an explicit Start. Coordinate-only
changes preserve running producers and histories. Capture intent survives a graph
without local sources or an unavailable adapter and resumes when a local source returns.

Every node has boolean `internal-data.enabled`, defaulting to true for legacy
documents. False gates the output and all dependent nodes, without bypassing a
transform or relaxing port validation. Disabled and disconnected branches with a
known input shape receive real zero frames (`available=true`) on their retained
upstream clock. Histories retain prior rows and append zero rows; unknown shape or
clock remains unavailable until data establishes the contract. Apply preserves
compatible node state and view snapshots; changed dimensions or source identity
reset only dependent states. Independent branches retain their histories.
WindowFunction selects
real symmetric Hann, Hamming, Blackman or Rectangular coefficients; Hann preserves
the legacy numerical implementation.

The current application has **one configured WLED adapter**. Compilation rejects
graphs containing more than one WLED node with an explicit error; it does not
silently ignore additional sinks. This restriction does not limit multiple sources,
histories, views or network outputs. The WLED sink converts configured Mel bands
to its legacy 50-value format on the same Mel-frequency axis and preserves the
MajorPeak value in Hz. Empty/unavailable input sends explicit silence.

Adapters use `feed_typed(node, frame, expected_epoch, expected_ingress_serial)`.
Capture both fence values when enqueueing work. `clear_ingress` invalidates only
that branch's serial, clears dependent snapshots, and publishes payload-free
`available=false` output markers. Resumed data carries discontinuity; live window
output sequence counters continue. Producer incarnation and receiver binding are
separate. Runtime Stop/apply changes the receiver epoch.

`subscribe_typed` supports multiple observers on the producing thread; observers
enqueue bounded work and must not invoke commands or mutate subscriptions.
`unsubscribe_typed` waits for in-flight calls. `latest_views` and `ViewFrame::views`
carry snapshots keyed by node ID. `graph_id()` preserves the document's stable
identity, defaulting to `main` for old documents. `node_definitions()` supplies
the shared port types, parameter defaults and scalar constraints for the editor
and compiler.
