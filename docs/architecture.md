# Architecture and Mechanisms

## Execution Model

CISDA treats Mapping and Tracking as one dependency-driven execution graph.
Mapping tiles progressively complete Gaussian gradients. A Tracking ray may
start with version-matched visible attributes before the corresponding primary
state update completes, but its tile contribution cannot enter the pose solve
until every speculative dependency is confirmed or corrected.

The simulator uses nanoseconds as the common timeline for the 500 MHz GS array
and 200 MHz ImpactPIM units. The included workloads are compact dependency
traces intended to exercise the mechanisms, not captured images or published
evaluation data.

## Stage I: Gaussian-Aware Tile Ordering

`ordering.py` implements the paper's online ordering algorithm. For each
Gaussian, it tracks the complete set of unissued Mapping tiles that still
produce gradient fragments. Candidate tile sets are ranked by the number of
Gaussian gradients they complete divided by their estimated cost. When no
complete candidate set exists, the oldest ready tile is selected.

The issued order may further prioritize a tile on the reverse path from a
pending Tracking contribution. This preserves the Gaussian-aware base order as
the deterministic fallback.

## Stage II: Impact-Constrained State Merge

`impact_pim.py` separates the primary Mapping state from the Tracking-visible
state. Every completed Gaussian gradient updates primary state. ImpactPIM then
replaces that Gaussian's prior contribution in its group and compares the
six-dimensional cumulative bound with the configured translation and rotation
threshold. Each compact workload has one active impact group, so its local
budget equals the global threshold and the sum-of-group-budgets rule holds
directly.

A valid version, coverage, and impact description within the bound produces a
reuse confirmation and leaves visible attributes unchanged. An invalid context
or exceeded component advances the visible version and writes only changed
fields. Eight owner-local units model the paper configuration and serialize
updates that map to the same owner.

## Stage III: Predictive Ray Rendering

`predictor.py` implements a four-decision history per Gaussian and 2,048
two-bit saturating counters. Gaussian-ID bits are XOR-folded into eleven bits;
the low four bits are combined with the short history. A valid history and a
set counter MSB predict reuse.

`simulator.py` creates one prediction record for each Gaussian update and
merges all dependent tiles into it. Speculative inputs are admitted only when
the old visible attributes are resident and version matched. Rays preserve
depth-ordered Gaussian lists from the workload. Feedback is applied once per
shared prediction after the actual ImpactPIM decision.

## Stage IV: Tile Result Maintenance

Ray gradients are retained by tile together with their map and validation
dependencies. A confirmation keeps the existing contribution. An advance
reruns only affected rays with target-version attributes; a coverage change
also records a workset rebuild. Each tile emits exactly one current
contribution. Tile contributions are reduced by tile ID, and the six pose
components are solved against the workload's diagonal Hessian only after all
required contributions are valid.

## Shared GS Array

`scheduler.py` models 24 independently configurable compute clusters arranged
as four six-cluster residency groups. Each task packet selects one of three
connections:

- `gaussian`: attribute broadcast and projection;
- `ray`: depth-ordered color, depth, and transmittance composition; or
- `tile`: local derivative and hierarchical reduction work.

The issuer favors the residency group associated with the packet's Gaussian
IDs and accounts for packet-boundary mode changes. Mapping, Tracking, and
correction packets use the same cluster pool.

## Reuse-Window-Guided Scheduling

The reverse scheduler starts from tiles containing speculative rays, follows
their Gaussian dependencies to Mapping producers, and estimates the remaining
validation path. A producer receives priority only when that path can finish
within the tile's retention window. Eligible producers are ranked by unique
avoidable attribute bytes divided by estimated completion time; ties use the
Gaussian-aware order.

## Workload Variants

The three included traces preserve the model-specific state layouts used by
the mechanisms:

| Workload | ImpactPIM-visible fields represented |
| --- | --- |
| SplaTAM | position, color, isotropic scale, opacity |
| Gaussian-SLAM | position, rotation, scale, color, opacity |
| VTGaussian-SLAM | color, opacity, radius with fixed view binding |

Each trace includes a confirmed reuse, a visible-state advance, a
non-speculative dependency, and tile-level final validation. The
VTGaussian-SLAM trace additionally exercises a coverage-changing correction.

## Reported Values

The public result is intentionally compact. It contains absolute completion
times, mechanism event counts, bytes written to primary and visible state, and
the final pose update. The simulator does not produce baseline reproduction,
normalized speedup, energy, power, area, or parameter-sweep results.
