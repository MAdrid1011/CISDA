# End-to-End Simulator Module

## External Interface

`Simulator::run` accepts a manifest path and returns one
`cisda-simulation-result/v1` JSON object with mechanism-level timing, event,
byte, and committed-pose values.

## Internal Mechanism

The run method connects ordered Mapping tiles, gradient dependencies,
versioned ImpactPIM decisions, predictive Tracking worksets, reverse
scheduling, shared-array packets, tile replacement, fixed-order reduction, and
the final pose transition on one timeline. Impact observations query and feed
back chronologically. Ray-mode work records pending tile contributions before
ImpactPIM validates the predicted inputs; confirmation retains those results
and advance schedules local replacement. Eligible reverse priority directly
reorders source-tile and validation issue, while fallback work retains regular
order.
