# CISDA Simulator

This repository contains the mechanism-level simulator for **CISDA**, a
3DGS-SLAM architecture for cross-stage speculative dataflow and
impact-constrained in-memory merging.

The release focuses on the architecture mechanisms. It includes compact
workloads for SplaTAM, Gaussian-SLAM, and VTGaussian-SLAM and reports absolute
latency, task, decision, correction, and state-traffic values. It does not
contain application source code, datasets, RTL, platform measurements, or
area and power models.

## Run

Python 3.10 or newer is required.

```bash
python -m venv .venv
. .venv/bin/activate
python -m pip install .
cisda-sim workloads/splatam.json
```

The same command accepts either of the other included workloads:

```text
workloads/gaussian_slam.json
workloads/vtgaussian_slam.json
```

The command writes one `cisda-result/v1` JSON document to standard output.
Reported values are absolute simulator results for the selected compact
workload; they are not paper evaluation tables or baseline comparisons.

## Simulator Scope

The simulator executes one end-to-end mechanism path:

1. Gaussian-aware Mapping tile ordering accelerates complete-gradient
   availability.
2. The reverse dataflow scheduler prioritizes Mapping work that can validate
   retained Tracking results before their reuse window expires.
3. A shared 24-cluster GS array runs Gaussian, ray, and tile packets and tracks
   task-packet mode changes.
4. The continuity-guided predictor shares one old-attribute decision across
   rays that depend on the same Gaussian update.
5. Eight ImpactPIM units update primary state, accumulate six-dimensional pose
   impact, and either confirm the visible state or write changed fields.
6. Tracking contributions remain pending until validation. Failed speculation
   triggers tile-local correction, and structural changes rebuild the affected
   workset.
7. Valid tile contributions are reduced in fixed order before one final pose
   commit.

See [Architecture and Mechanisms](docs/architecture.md) for the module-level
description and the correspondence between the paper dataflow and simulator.

## Output

The result contains:

- total and per-stage completion time in nanoseconds;
- Gaussian-aware and final Mapping tile issue order;
- absolute Mapping, Tracking, prediction, merge, correction, and commit counts;
- primary-state and tracking-visible-state bytes written; and
- the committed six-dimensional pose update for the compact workload.
