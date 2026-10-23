from __future__ import annotations

from dataclasses import dataclass

from .model import MappingTile, Ray, TrackingTile


@dataclass(frozen=True)
class ArrayTask:
    name: str
    mode: str
    release_ns: int
    duration_cycles: int
    gaussian_ids: tuple[int, ...]


@dataclass(frozen=True)
class ArrayResult:
    cluster: int
    start_ns: int
    finish_ns: int


class SharedGsArray:
    CLUSTERS = 24
    CLUSTERS_PER_GROUP = 6
    PERIOD_NS = 2

    def __init__(self) -> None:
        self._available = [0] * self.CLUSTERS
        self._mode: list[str | None] = [None] * self.CLUSTERS
        self.tasks = 0
        self.mode_switches = 0

    def schedule(self, task: ArrayTask) -> ArrayResult:
        if task.mode not in {"gaussian", "ray", "tile"}:
            raise ValueError("unknown GS array mode")
        data_group = min(task.gaussian_ids, default=0) % 4
        first = data_group * self.CLUSTERS_PER_GROUP
        candidates = range(first, first + self.CLUSTERS_PER_GROUP)
        cluster = min(
            candidates,
            key=lambda item: (
                max(task.release_ns, self._available[item]),
                self._mode[item] not in (None, task.mode),
                item,
            ),
        )
        start = max(task.release_ns, self._available[cluster])
        finish = start + task.duration_cycles * self.PERIOD_NS
        if self._mode[cluster] not in (None, task.mode):
            self.mode_switches += 1
        self._mode[cluster] = task.mode
        self._available[cluster] = finish
        self.tasks += 1
        return ArrayResult(cluster, start, finish)


def reverse_dataflow_order(
    base_order: list[int],
    mapping_tiles: tuple[MappingTile, ...],
    rays: tuple[Ray, ...],
    tracking_tiles: tuple[TrackingTile, ...],
    speculative_gaussians: set[int],
) -> tuple[list[int], int]:
    """Rank validation paths by saved bytes per estimated completion time."""
    by_id = {tile.tile_id: tile for tile in mapping_tiles}
    base_rank = {tile_id: index for index, tile_id in enumerate(base_order)}
    deadline = {tile.tile_id: tile.retention_deadline_ns for tile in tracking_tiles}
    source_tiles: dict[int, set[int]] = {}
    for tile in mapping_tiles:
        for gaussian_id in tile.gaussian_ids:
            source_tiles.setdefault(gaussian_id, set()).add(tile.tile_id)

    candidates: list[tuple[int, set[int], list[Ray], int, int]] = []
    for gaussian_id in sorted(speculative_gaussians):
        dependent = [ray for ray in rays if gaussian_id in ray.gaussian_ids]
        if not dependent:
            continue
        producers = source_tiles[gaussian_id]
        estimated_ns = (
            sum(by_id[tile_id].cost_cycles for tile_id in producers)
            * SharedGsArray.PERIOD_NS
            + 60
        )
        reuse_window = min(deadline[ray.tile_id] for ray in dependent)
        candidates.append(
            (gaussian_id, producers, dependent, estimated_ns, reuse_window)
        )

    claimed_rays: set[int] = set()
    unissued = set(by_id)
    order: list[int] = []
    critical = 0
    remaining = list(candidates)
    while remaining:
        def priority(candidate: tuple[int, set[int], list[Ray], int, int]) -> float:
            _, _, dependent, estimated_ns, reuse_window = candidate
            if estimated_ns > reuse_window:
                return 0.0
            saved_bytes = sum(
                ray.avoidable_read_bytes for ray in dependent
                if ray.ray_id not in claimed_rays
            )
            return saved_bytes / estimated_ns

        selected = max(
            remaining,
            key=lambda candidate: (
                priority(candidate),
                -min(base_rank[tile_id] for tile_id in candidate[1]),
                -candidate[0],
            ),
        )
        selected_priority = priority(selected)
        if selected_priority <= 0:
            break
        _, producers, dependent, _, _ = selected
        critical += 1
        claimed_rays.update(ray.ray_id for ray in dependent)
        for tile_id in sorted(producers, key=base_rank.get):
            if tile_id in unissued:
                order.append(tile_id)
                unissued.remove(tile_id)
        remaining.remove(selected)

    order.extend(tile_id for tile_id in base_order if tile_id in unissued)
    return order, critical
