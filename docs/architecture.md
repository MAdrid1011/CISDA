# Architecture and Mechanisms

## End-to-End Dataflow

CISDA represents Mapping and Tracking as one dependency graph. Mapping tile
fragments make individual Gaussian updates ready. ImpactPIM updates primary
state and resolves which version Tracking may observe. Tracking ray results
remain pending until every predicted input is confirmed or replaced. Only
validated tile contributions enter the final pose reduction.

The four stages use three granularities:

| Stage | Unit | Function |
| --- | --- | --- |
| I | image tile | Mapping render, loss, backward pass, gradient fragments |
| II | Gaussian | state update, impact validation, visible-state merge |
| III | ray | versioned projection and depth-ordered composition |
| IV | image tile | contribution validation, replacement, pose reduction |

`src/simulator.cpp` connects all four stages to one 500 MHz ASIC timeline and
the 200 MHz PIM domain. A run completes only after Stage IV records one pose
commit.

## Stage I: Gaussian-Aware Tile Ordering

`src/ordering.cpp` consumes the native rasterizer's stable Gaussian IDs,
point-list indices, and complete tile ranges. For every Gaussian, it maintains
the unissued source set `R(g)`. Every distinct nonempty `R(g)` is a candidate
tile set. Candidates are scored by the number of Gaussian gradients completed
by the set divided by the sum of its captured relation costs. The winning set
is issued in arrival order, removed from all source sets, and all candidates
are rescored. An oldest-ready selection handles tiles that contain no remaining
Gaussian source.

This stage preserves full source completion: a Gaussian cannot enter its state
update merely because one of several contributing tiles has finished.
`src/dependency_tables.cpp` records every emitted fragment, reduces fragments
by Gaussian ID, and removes the issuing tile from the unfinished-source XOR.
It exposes a Gaussian only after the XOR and source count both reach zero.
Tile loss ownership and boundary-neighbor dependencies remain attached to the
frozen Mapping snapshot and captured pose context.

## Stage II: Impact-Constrained State Merge

`src/impact_pim.cpp` separates primary Mapping state from the version visible
to Tracking. Every gradient-ready update writes primary attributes and
optimizer state. Gaussian IDs shard update commands across eight PIM owners.

For a valid pose, coverage, and target-version description, the state engine
atomically validates the captured stable-ID group and its six-component pose
update difference. Translation components are expressed in meters and
relative-rotation components in radians. The default component bounds are
`1e-4`, matching the default pose-impact threshold. If every component remains
within the group budget, all member Gaussians retain their visible attributes
and receive versioned confirmations. Otherwise, all members advance together
and the required fields return to the ASIC. Missing coefficients, stale
versions, or invalid coverage always take this conservative advance path.

Each observed Gaussian has an explicit primary and visible version record.
The current pass continues to read the frozen source version, while completed
updates bind the next Mapping pass to the target primary version. Ready
Gaussians are ordered by the number of rendering and validation consumers they
unblock. Per-group six-component budgets are allocated from one global bound,
so their component-wise sum does not exceed that bound.

`src/workload.cpp` obtains the six-dimensional description either from a
captured pose-update delta or from the relative pose between native baseline
and exact-withdrawal Tracking solves. A group-level observation is never split
into fabricated per-Gaussian coefficients. Its one actual keep/advance result
feeds every member Gaussian's predictor history.

## Stage III: Predictive Ray Rendering

`src/predictor.cpp` implements one four-bit actual-decision history per
Gaussian and a shared table of 2,048 two-bit saturating counters. Gaussian-ID
bits are XOR-folded into 11 bits; the low four folded bits are XORed with the
history. A prediction may select old attributes only when:

- all four history bits are valid;
- the selected counter's most significant bit is set;
- the visible attributes are resident; and
- their version matches the request.

Impact observations execute in captured frame, iteration, and sequence order.
Each query receives its actual ImpactPIM outcome before the next observation
is queried. One target query is shared by all dependent tile requests for the
same Gaussian and Tracking context. Feedback updates the stored counter index
once and shifts the actual decision into that Gaussian's history. Cold
histories wait for actual attributes; speculation begins only after four real
prior decisions have populated the history.

The scheduled Tracking rasterizer supplies the complete point-list order for
each tile. `src/shared_array.cpp` consumes that order directly, checks the CAM
entry's map and pose versions, extends residency for later consumers, and
issues a fill for every missing or mismatched row. Each pending contribution
retains its map, pose, and loss contexts until its input and validation
dependencies are resolved. Ray-mode rendering records that pending
contribution before ImpactPIM returns the actual keep-or-advance decision, so
confirmation retains the speculative result while advance selects correction.

## Stage IV: Tile Result Maintenance

Tracking results carry map version, pose context, loss context, and pending
Gaussian decisions. A confirmation validates the existing tile contribution.
An advance invalidates contributions that consumed old attributes and schedules
ray-mode replacement work with the target version. Coverage or depth-order
changes require a new ordered workset before replacement.

The tile table keeps one current contribution per tile. Contributions are
reduced in deterministic tile order after all validation dependencies clear;
the pose commit is the terminal event of the simulation. The committed value
is decoded from the capture's authoritative final native pose record, preserving
its quaternion/translation or camera-to-world matrix representation.
`src/tile_maintenance.cpp` implements the versioned slot transition. A
correction invalidates the old slot, subtracts the old contribution, adds the
new contribution, and marks the target-version slot valid. A structural
response rebuilds the affected workset before replacement. Loss-dependent
expansion remains scoped to captured dependency evidence.

## Shared GS Array

`src/shared_array.cpp` models 24 compute clusters in four six-cluster residency
groups. Every packet chooses one connection mode:

- `gaussian` broadcasts attributes for projection;
- `ray` performs depth-ordered color, depth, and transmittance composition;
- `tile` performs local derivatives and hierarchical reductions.

The issuer selects the earliest available cluster in the packet's preferred
residency group. A mode change is counted only at a packet boundary. Mapping,
Tracking, and replacement work use the same cluster pool.

Each cluster models 16 FP32 FMA lanes arranged as 4 by 4, four EXP/LOG lanes,
and two reciprocal/square-root lanes. Each six-cluster group owns a 256-entry
residency CAM and a 32 KiB Gaussian Data SRAM. The four groups share the
128 KiB SPM. A separate 16 KiB Frame Tile Buffer streams RGB-D tile inputs.

## Dependency Tables and Storage Credits

`src/dependency_tables.cpp` maintains distinct input-readiness and
result-validation tables. Requests with the same Gaussian update and Tracking
context share one entry. Source counts plus the unfinished-source XOR identify
the last producer, and consumer counts propagate priority when an update
becomes ready.

SPM and Frame Tile Buffer reservations precede issue. Completion and correction
space is reserved before speculative work. Candidates without credits return
to regular scheduling; eligible candidates propagate priority to both the ASIC
issuer and PIM queue. Shared input/version tasks merge, and equal-priority
near-memory requests group by row.

## Reverse Dependency Scheduling

`src/reverse_scheduler.cpp` begins with pending speculative tile results and
walks backward through Gaussian validation to missing Mapping gradients. Each
candidate carries a remaining retention window, longest unfinished path,
deduplicated avoidable attribute bytes, and wait time. A candidate is eligible
only when its path fits within the retention window. Eligible candidates are
ranked by avoidable bytes per path cycle, then by nearer deadline and longer
wait. Shared reads are counted once.

When the predictor admits no speculative input, there is no pending reuse
result to prioritize; the scheduler correctly reports zero candidates while
the normal Gaussian-aware order remains active.

For admitted candidates, `priority_order` is operational rather than advisory.
It first promotes the candidate's unfinished Mapping source tiles in the
shared-array issue stream, then orders the corresponding atomic ImpactPIM
validation groups. Window-expired or credit-blocked candidates are returned in
`fallback_order` for accounting only; they receive no issue priority.
Noncandidate and fallback work follows in the original Gaussian-aware order.
The result reports both prioritized Mapping tiles and applied validation
issues.

## ImpactPIM Organization

`src/impact_pim.cpp` models eight owners, with two units per LPDDR5 die and one
four-bank group per owner. Gaussian IDs select the die, unit, bank, and row;
existing and target-version state use four-bank interleaving. Every update has
a 32-byte descriptor binding its Gaussian, version, gradient readiness, and
validation targets.

The update entry writes primary state and optimizer history. The validation
entry executes move, difference, absolute-value, reduction,
multiply-accumulate, comparison, and field-selection operations using the
workload field layout. The write selector advances only selected visible fields
when a valid mask is available and conservatively selects the full workload
layout when the native capture has no field mask. Completion records carry
version and context tags; late descriptions re-enter validation against the
target primary state.

Refresh arbitration drains and precharges the target bank pair, records the
refresh, and resumes retained program state after recovery. Other owners remain
independently addressable. Same-priority descriptors are grouped by row before
dispatch.

## Timing and State Accounting

`src/workload.cpp` decodes native JSON Lines records, including gzip streams
and little-endian base64 tensors. The simulator checks block conservation and
uses captured Gaussian-block relations, examined and blended ray operations,
active pixels, optimizer objects, and scalar counts. ASIC and PIM work overlap
on one timeline; completion traffic is charged before the pose commit.

The output is deliberately compact. It reports end-to-end and stage cycles,
updated objects and scalars, state bytes, PIM commands, prediction and merge
events, shared-array mode tasks, tile validation, and the final commit count.
`mechanism_coverage` separately records whether the selected capture supplied
valid impact descriptions, valid predictor histories, speculative work,
applied reverse ordering, and a correction event. A zero action count therefore
describes the selected observation window rather than claiming that the path
was exercised.
