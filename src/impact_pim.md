# ImpactPIM Module

## External Interfaces

`ImpactPim` binds primary and visible versions, allocates group budgets,
accounts update programs, atomically validates captured stable-ID groups,
returns merge decisions, services refresh, and drains tagged completions.

## Internal Mechanisms

Gaussian IDs select one of eight owners and four interleaved banks. The module
maintains cumulative six-component pose-impact records, workload field masks,
32-byte descriptors, update/validation micro-operations, completion tags, and
retained state across bank-pair refresh. One group decision updates the visible
version and completion state of every member Gaussian.
