# End-to-End Simulator Module

## External Interface

`Simulator::run` accepts a manifest path and returns one
`cisda-simulation-result/v1` JSON object with absolute timing, event, byte,
mechanism, and committed-pose values.

## Internal Mechanism

The run method connects ordered Mapping tiles, gradient dependencies,
versioned ImpactPIM decisions, predictive Tracking worksets, reverse
scheduling, shared-array packets, tile replacement, fixed-order reduction, and
the final pose transition on one timeline.
