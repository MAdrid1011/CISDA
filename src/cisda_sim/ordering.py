from __future__ import annotations

from .model import MappingTile


def gaussian_aware_order(tiles: tuple[MappingTile, ...]) -> list[int]:
    """Implement Algorithm 1 from the paper."""
    by_id = {tile.tile_id: tile for tile in tiles}
    unfinished: dict[int, set[int]] = {}
    for tile in tiles:
        for gaussian_id in tile.gaussian_ids:
            unfinished.setdefault(gaussian_id, set()).add(tile.tile_id)

    unissued = set(by_id)
    order: list[int] = []
    while unissued:
        candidate_sets = {
            frozenset(sources)
            for sources in unfinished.values()
            if sources and sources <= unissued
        }
        if candidate_sets:
            def rank(candidate: frozenset[int]) -> tuple[float, int, tuple[int, ...]]:
                completed = sum(
                    1 for sources in unfinished.values()
                    if sources and sources <= candidate
                )
                cost = sum(by_id[tile_id].cost_cycles for tile_id in candidate)
                first_arrival = min(by_id[tile_id].arrival for tile_id in candidate)
                return (-completed / cost, first_arrival, tuple(sorted(candidate)))

            selected = min(candidate_sets, key=rank)
        else:
            selected = frozenset({min(
                unissued,
                key=lambda tile_id: (by_id[tile_id].arrival, tile_id),
            )})
        selected_order = sorted(
            selected,
            key=lambda tile_id: (by_id[tile_id].arrival, tile_id),
        )
        order.extend(selected_order)
        unissued.difference_update(selected)
        for sources in unfinished.values():
            sources.difference_update(selected)
    return order
