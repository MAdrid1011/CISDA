# ImpactPIM Module

## External Interfaces

`ImpactPim` binds primary and visible versions, allocates group budgets,
accounts update programs, validates impact descriptions, returns merge
decisions, services refresh, and drains tagged completions.

## Internal Mechanisms

Gaussian IDs select one of eight owners and four interleaved banks. The module
maintains cumulative six-component records, workload field masks, 32-byte
descriptors, update/validation micro-operations, completion tags, and retained
state across bank-pair refresh.
