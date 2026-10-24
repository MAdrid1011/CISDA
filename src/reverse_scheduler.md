# Reverse Dataflow Scheduler Module

## External Interface

`reverse_dataflow_schedule` accepts pending-result candidates with retention
windows, dependency-path time, avoidable bytes, wait age, row address, shared
dependency key, attribute bytes, and tile-contribution bytes. It also receives
the currently available shared-SPM byte credits.

## Internal Mechanism

Eligible work is ranked by bytes per path cycle, then deadline and age. In that
order, the scheduler reserves attribute bytes once per shared dependency key
and contribution bytes once per tile. It propagates credited work to ASIC and
PIM queues and groups equal-priority memory rows. It returns selected tiles in
`priority_order` and window-expired or capacity-blocked tiles in
`fallback_order`, together with the exact byte reservation. `Simulator::run`
commits that reservation to the SPM and applies only `priority_order` to
Mapping source-tile issue and atomic ImpactPIM validation issue; fallback work
retains the regular Gaussian-aware order.
