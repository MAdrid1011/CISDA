# Reverse Dataflow Scheduler Module

## External Interface

`reverse_dataflow_schedule` accepts pending-result candidates with retention
windows, dependency-path time, avoidable bytes, wait age, row address, shared
dependency key, and storage-credit state.

## Internal Mechanism

Eligible work is ranked by bytes per path cycle, then deadline and age. The
scheduler merges shared tasks, reserves storage, propagates priority to ASIC
and PIM queues, and groups equal-priority memory rows. It returns eligible,
credited tiles in `priority_order` and window-expired or credit-blocked tiles
in `fallback_order`. `Simulator::run` applies only `priority_order` to Mapping
source-tile issue and atomic ImpactPIM validation issue; fallback work retains
the regular Gaussian-aware order.
