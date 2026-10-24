# Shared GS Array Module

## External Interfaces

`SharedGsArray::issue` dispatches Gaussian, ray, or tile packets to the
preferred six-cluster group. `supply` consumes a captured depth-ordered
Gaussian workset and returns hit, fill, version-miss, and byte counts.

## Internal Mechanisms

The 24 clusters each expose 16 FP32 FMA, four EXP/LOG, and two
reciprocal/square-root lanes. Four residency groups each model a 256-entry CAM
and 32 KiB SRAM with successor retention, version checks, projection sharing,
prefetch fills, and packet-boundary connection switching.
