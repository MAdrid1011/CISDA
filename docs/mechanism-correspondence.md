# Mechanism Correspondence

Every row below names state exercised from `Simulator::run()`. A zero event
count means the selected native capture did not admit that action; the state
machine and conservative path still execute.

| Paper mechanism | C++ implementation | Run evidence |
| --- | --- | --- |
| III-A frozen Mapping snapshot and tile-owned loss | `simulator.cpp`, `dependency_tables.cpp` | `frozen_snapshot_version`, `owned_loss_terms`, `boundary_dependency_records` |
| III-A complete `R(g)` candidate-set ordering | `ordering.cpp` | `mapping_tiles`, `completed_gradient_groups`, `first_issued_tile` |
| III-A gradient fragments and Gaussian-ID reduction | `dependency_tables.cpp` | `gradient_fragments`, `gradient_reduction_operations`, `single_source_xor_identifications` |
| III-B primary/visible versions and next-pass binding | `impact_pim.cpp` | `primary_version`, `visible_source_version`, `next_pass_snapshot_version`, `version_checks` |
| III-B ready-Gaussian dependency priority | `dependency_tables.cpp` | `ready_gaussians`, `ready_priority_propagations` |
| III-B cumulative six-component impact replacement | `impact_pim.cpp` | `budget_groups`, `reuse_confirmations`, `visible_state_advances` |
| III-C complete depth-ordered Tracking worksets | `workload.cpp`, `shared_array.cpp` | `tracking_rasterizer_sequence`, `depth_ordered_workset_entries` |
| III-C shared reuse prediction and actual feedback | `predictor.cpp` | `predictor_lookups`, `shared_prediction_requests`, `predictor_feedback_updates` |
| III-C versioned residency and attribute fill | `shared_array.cpp` | `residency_hits`, `residency_version_misses`, `fill_requests`, `fill_bytes` |
| III-C pending map/pose/loss contexts and correction | `tile_maintenance.cpp` | `speculative_dependencies`, `corrected_tiles`, `structural_workset_rebuilds` |
| III-D versioned tile slots and deterministic replacement | `tile_maintenance.cpp` | `replacement_contributions`, `replacement_arithmetic_ops` |
| III-D fixed-order reduction and pose transition | `tile_maintenance.cpp` | `fixed_order_reductions`, `pose_context_transitions`, `pose_commits` |
| IV-A 24-cluster reconfigurable GS array | `shared_array.cpp` | lane configuration, mode tasks, `packet_boundary_reconfigurations` |
| IV-A residency groups and successor retention | `shared_array.cpp` | `clusters_per_residency_group`, `shared_projection_rows` |
| IV-B four-bit history and 2,048 two-bit counters | `predictor.cpp` | predictor lookup, prediction, and feedback counts |
| IV-B separate readiness/validation tables and credits | `dependency_tables.cpp` | table entries, buffer high-water marks, credit stalls |
| IV-B reverse paths and supply priority | `reverse_scheduler.cpp` | eligibility, reservations, task merges, ASIC/PIM priority propagation, row groups |
| IV-C eight-owner update and validation programs | `impact_pim.cpp` | descriptors, microprograms, micro-operations, owner count |
| IV-C die/group/bank mapping and field selection | `impact_pim.cpp` | address mappings, bank lanes, selected field writes |
| IV-C tagged completion, late validation, and refresh | `impact_pim.cpp` | completion queue, late entries, drain/precharge/refresh/resume counts |
