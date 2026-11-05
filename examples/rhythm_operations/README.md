# Library reference graphs

These seven explicit research recipes use the ordinary PreparedGraph executor.
Framing, windowing, FFT, onset evidence, temporal context and beat decisions are
connected stages, with intermediate ports available for inspection.

| Preset ID | Recipe |
| --- | --- |
| `aubio-reference` | [Aubio HFC onset and context tempo](aubio/README.md) |
| `reference-btrack` | [BTrack source window, KISS FFT and predictive beats](btrack/README.md) |
| `madmom-superflux-fixed` | [madmom SuperFlux and fixed tempo beats](madmom/README.md) |
| `madmom-superflux-local` | [madmom SuperFlux and local tempo beats](madmom/README.md) |
| `madmom-superflux-crf` | [madmom SuperFlux and CRF beats](madmom/README.md) |
| `essentia-reference-degara` | [Essentia Degara](essentia/README.md) |
| `essentia-reference-multifeature` | [Essentia MultiFeature](essentia/README.md) |

Build the optional research composition from the repository root:

```powershell
cmake --build --preset release --target rhythm_reference_runner
```

Export fresh versioned project documents, including presentation positions and
labels. The exporter reloads each saved document and checks its graph identity
and node/edge counts. Choose a new directory to preserve previous artifacts:

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --export ./my-reference-projects
```

Run a bounded synthetic capture with no speaker output. `--observe` subscribes to
one declared output and requires at least one available nonempty numeric frame;
its final JSON includes the value, shape and publication counts:

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --preset reference-btrack --synthetic --duration 4000 --observe 34:1
```

Run the saved document through the same canonical project loader:

```powershell
./cmake-build-release/bin/rhythm_reference_runner.exe --project ./examples/rhythm_operations/projects/reference-btrack.json --synthetic --duration 4000 --observe 34:1
```

The optional `--port 18215` exposes the runner's loopback CONTROL/DATA endpoint
for a desktop client. Without `--synthetic`, the runner uses the selected native
capture backend. Duration is in milliseconds; the process tears down its own
capture and executor when the duration expires.

The research executable explicitly links GPL/AGPL source adapters and larger
session-buffer limits. It is excluded from ALL and the portable SDK. These
library-specific stages require this research runtime when applying an exported
project; the ordinary application's default runtime does not supply them.

The runner chooses its installed FFT provider from explicit `backend_id` values
on Fourier/RealFourier nodes. Empty IDs keep builtin execution without a provider;
one exact named ID selects Ooura or KISS. Unknown or conflicting names reject
before engine construction. This selection uses neither preset names nor signal
shape or topology. Each exported recipe retains its explicit provider settings.

Each recipe guide distinguishes runtime checks, comparison against the pinned
original source, and measured limitations. Synthetic agreement alone does not
establish musical tracking quality or native rendering acceptance. Intermediate
numerical failures remain failures under their original tolerances.
