# Gaussian-Aware Ordering Module

## External Interface

`gaussian_aware_order` accepts captured Mapping topology and attribute bytes,
then returns the complete tile issue order and ordering counts.

## Internal Mechanism

The implementation reconstructs every nonempty unfinished source set `R(g)`,
scores candidate sets by completed Gaussians per captured relation cost,
removes issued sources, and rescans. Oldest-ready selection handles the case
without a candidate set.
