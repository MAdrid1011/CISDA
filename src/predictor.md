# Reuse Predictor Module

## External Interfaces

`ReusePredictor::query` returns a shared prediction record for one Gaussian
update and Tracking context. `feedback` updates the exact stored counter index
and the Gaussian's actual-decision history.

## Internal Mechanism

The module uses four history bits and 2,048 two-bit saturating counters.
Gaussian-ID chunks XOR-fold to 11 bits, and the low four bits mix with history.
Reuse additionally requires residency and version agreement.
