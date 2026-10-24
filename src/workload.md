# Workload Ingestion Module

## External Interface

`load_workload` validates a manifest and returns its compiled operators, model
and dataset roots, and decoded native-capture facts.

## Internal Mechanisms

The module reads plain or gzip JSON Lines, decodes little-endian base64 tensors,
resolves shared stable-ID snapshots, reconstructs Mapping source sets and the
scheduled Tracking point-list order, validates tile grids, and decodes the
authoritative committed pose.
