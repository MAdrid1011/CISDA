# Tile Result Maintenance Module

## External Interfaces

`TileMaintenance` opens a map/pose/loss context, records one versioned
contribution per tile, resolves confirmations, replaces invalid contributions,
expands loss invalidation, and commits the pose.

## Internal Mechanism

Replacement invalidates the old slot and records subtraction plus addition.
Commit requires every target-version slot to be valid and dependency-free,
then reduces captured contributions in numeric tile order before the pose-state
transition.
