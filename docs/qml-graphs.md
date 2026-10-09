# QML graph presentation

Graph visualization uses Qt Quick/QML inside the existing dock workspace.
The QtNodes editor, processing graph, DSP, portable runtime and transport do not
depend on QML. This is a graph presentation migration, not a whole-app rewrite.

## Responsibilities

- `NumericViewAdapter` accepts bounded owned delivery, enforces contract and
  revision fences, and projects values through synchronous presentation ports
  in `visualization/quick/numeric_render_ports.h`. It knows no concrete widget.
- `GraphPanel` composes the presentation ports and renderer façades, owns view
  preferences/actions and hosts the shared `common/GraphPanel.qml` controls.
  The workspace continues to own docking and document persistence.
- `QuickPlotModel` publishes bounded viewport geometry and exact strings.
  `plots2d/Plot2D.qml` draws line/points/bars/pie, labels and scalar metrics.
  The image provider supplies a numeric raster, never a QWidget screenshot.
- `SurfaceQuickItem` embeds the retained OpenGL geometry in the Quick scene
  through `QQuickFramebufferObject`. `surface3d/SurfaceView.qml` owns layout,
  controls, input routing and overlays. GUI state and render-thread GL resources
  are separate, synchronized through immutable snapshots and bounded feedback.
- `ui::quick::UiBridge` exposes the single `ui::Theme` palette with Active,
  Inactive and Disabled roles and `ui::text` translations. QML cannot select
  an independent theme palette or rename stable configuration identifiers.

The public QWidget façades are dock integration adapters. The live graph target
no longer uses QChartView, QOpenGLWidget or Qt Data Visualization renderers.
Qt Quick Controls, Qml, QuickWidgets and OpenGL are explicit desktop dependencies.
The composition initializes the OpenGL Quick backend before any Quick widget.
QML resources are linked into the executable; Windows deployment scans their
imports with `windeployqt --qmldir`.

## Data and behavior

Source vectors remain bounded in C++; QML receives viewport min/max geometry
(up to 4098 line/point vertices or 2048 bar stems), preserving both signs and
actual x coordinates. Pie uses at most 64 slices, aggregating the remainder.
Exact scalar values use QString through QML text, never JavaScript numeric
conversion. Matrix snapshot storage is shared immutably by 2D and the indexed
3D transpose. History accumulation belongs only to processing History nodes.

Frequency/time capabilities come from source coordinates. Display choice does
not change samples, source metadata, DSP or revision. Frame zero is newest for
History; generic matrix rows retain their declared order. Camera projection
does not determine tick policy. Pause retains frozen data with one latest
pending snapshot; hidden graphs stop drawing, not processing.

Modes, axis ranges/direction/precision, plane, camera and renderer preferences
retain stable identities. Restore and capability/language refresh do not emit
edits. Independent graph instances share endpoint data without sharing camera
or view state. User configurations are not rewritten by migration tooling.

## Verification scope

Implementation, independent source review, build and automatic acceptance are
tracked separately in [progress-checklist](refactoring/progress-checklist.md).
Offscreen checks use Qt Quick software rendering for deterministic 2D output;
the native OpenGL/FBO path requires its own scenario and evidence. Historical
Widgets/native PASS does not accept the new Quick render pipeline.

`qml_graph_tests` checks the actual QML scenes, routed controls, bounded signed
peaks, exact UInt64 text, raster cell pixels, theme/language, snapshot notifications
and lazy surface preference restoration. `numeric_native_axes_tests` uses the
actual default graph fed by eight synthetic changing tones without playback,
400 History frames, physical/index axes, camera poses, point/slice readouts,
exact GPU buffer readback and owned framebuffer images. Its hide/show and
reparent phases exercise the Quick context lifecycle without desktop capture.
Workspace restart fixtures use separate processes, rather than recreating
QApplication and Qt Quick global rendering state in one process.

Migration acceptance evidence is in
`cmake-build-release/qml-migration-20261009-220014`: release build PASS,
full host run 99/101 followed by 14/14 affected rechecks, giving combined latest
101/101 PASS. Native 2D and the independently reviewed native 3D scenario PASS;
see `acceptance-summary.json`, `host-final.xml`, `host-recheck.xml`,
`native2d-accepted.out.log` and `native3d-final/*.png/json`.
General visual/product acceptance and FPS benchmarking remain separate.

QQuickWidget adds an offscreen rendering pass and disables the threaded render
loop; the FBO surface requires OpenGL. These are deliberate integration choices
while the application retains its dock workspace, not a claim of improved FPS.
See the official [QQuickWidget documentation](https://doc.qt.io/qt-6.10/qquickwidget.html),
[Quick scene graph](https://doc.qt.io/qt-6.10/qtquick-visualcanvas-scenegraph.html)
and [QML deployment](https://doc.qt.io/qt-6.10/qtquick-deployment.html).
