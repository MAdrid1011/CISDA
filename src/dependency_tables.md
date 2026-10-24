# Dependency Tables Module

## External Interfaces

`DependencyTables` tracks Mapping fragment readiness, Tracking result
validation, shared request entries, and SPM/Frame Tile Buffer credits. Tile
completion returns ready Gaussians in descending unblocked-consumer priority.

## Internal Mechanisms

Each Gaussian entry stores an unfinished-source count and XOR. Tracking
requests merge by update/context key. Separate per-tile validation counts keep
speculative results pending until the matching actual decision arrives.
