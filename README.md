# CISDA Simulator

CISDA is a C++20 mechanism-level simulator for continuity-guided,
mixed-granularity 3D Gaussian Splatting SLAM. It executes Mapping and Tracking
on one end-to-end timeline and reports mechanism-level cycle, event, and byte
counts.

The simulator implements:

- Gaussian-dependency-aware online Mapping tile ordering;
- a 24-cluster shared GS array with Gaussian, ray, and tile modes;
- a four-decision history table and 2,048 two-bit reuse counters;
- reuse-window-guided reverse dependency scheduling;
- eight-owner ImpactPIM update, validation, and selective merge flow;
- versioned primary and Tracking-visible Gaussian state;
- pending tile validation, local replacement, and final pose commit.

The source tree is self-contained. Large workload assets stay in the ignored
`local/` directory and are not part of the Git repository.

## Build

Requirements are CMake 3.22 or newer, a C++20 compiler, nlohmann/json 3.11 or
newer, and zlib.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix "$PWD/install"
export PATH="$PWD/install/bin:$PATH"
```

## Workload Assets

The included manifests select the real SplaTAM, Gaussian-SLAM, and
VTGaussian-SLAM workloads on TUM-RGBD `rgbd_dataset_freiburg1_desk`. Before a
run, keep the following local assets under the repository root:

```text
local/
  bundles/{splatam,gaussian_slam,vtgaussian_slam}.json
  captures/{splatam.jsonl.gz,gaussian_slam.jsonl,vtgaussian_slam.jsonl}
  data/TUM_RGBD/rgbd_dataset_freiburg1_desk/
  models/{SplaTAM,Gaussian-SLAM,VTGaussian-SLAM}/
```

Each manifest uses relative paths and validates the model, dataset, compiled
operator workload, and native capture before simulation. See
[Workload Inputs](docs/workloads.md) for the input contract.

## Run

The simulator has one runtime interface:

```bash
cisda-sim WORKLOAD.json
```

For example:

```bash
cisda-sim workloads/splatam.json
```

The other manifests are `workloads/gaussian_slam.json` and
`workloads/vtgaussian_slam.json`. The command writes one
`cisda-simulation-result/v1` JSON document to standard output.

The result contains mechanism-level end-to-end cycles and latency, four-stage
completion cycles, event counts, primary and visible state bytes, one validated
pose commit, and explicit coverage for captured impact, predictor history,
speculation, applied reverse order, and correction. It does not emit comparison
or normalized results.

See [Architecture and Mechanisms](docs/architecture.md) for the simulator
design. The [Mechanism Correspondence](docs/mechanism-correspondence.md) maps
every Section III and Section IV mechanism to its C++ state and result fields.
