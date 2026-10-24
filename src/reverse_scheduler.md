# Reverse Dataflow Scheduler Module

## External Interface

`reverse_dataflow_schedule` accepts pending-result candidates with retention
windows, dependency-path time, avoidable bytes, wait age, row address, shared
dependency key, and storage-credit state.

## Internal Mechanism

Eligible work is ranked by bytes per path cycle, then deadline and age. The
scheduler merges shared tasks, reserves storage, propagates priority to ASIC
and PIM queues, groups equal-priority memory rows, and returns ineligible work
to regular order.
