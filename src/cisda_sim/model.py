from __future__ import annotations

from dataclasses import dataclass
from typing import Any


VECTOR_SIZE = 6


def _vector(value: Any, name: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != VECTOR_SIZE:
        raise ValueError(f"{name} must contain six numbers")
    if not all(isinstance(item, (int, float)) for item in value):
        raise ValueError(f"{name} must contain only numbers")
    return tuple(float(item) for item in value)


def _fields(value: Any, name: str) -> dict[str, tuple[float, ...]]:
    if not isinstance(value, dict) or not value:
        raise ValueError(f"{name} must be a non-empty object")
    result: dict[str, tuple[float, ...]] = {}
    for field, items in value.items():
        if not isinstance(field, str) or not isinstance(items, list) or not items:
            raise ValueError(f"{name} has an invalid field")
        if not all(isinstance(item, (int, float)) for item in items):
            raise ValueError(f"{name}.{field} must contain only numbers")
        result[field] = tuple(float(item) for item in items)
    return result


@dataclass(frozen=True)
class MappingTile:
    tile_id: int
    arrival: int
    cost_cycles: int
    gaussian_ids: tuple[int, ...]


@dataclass(frozen=True)
class Gaussian:
    gaussian_id: int
    version: int
    group: int
    history: tuple[int, ...]
    counter: int
    resident: bool
    fields: dict[str, tuple[float, ...]]
    updated_fields: dict[str, tuple[float, ...]]
    impact: tuple[float, ...]
    impact_valid: bool
    impact_ready_ns: int
    coverage_changed: bool

    @property
    def state_bytes(self) -> int:
        return 4 * sum(len(values) for values in self.updated_fields.values())


@dataclass(frozen=True)
class Ray:
    ray_id: int
    tile_id: int
    gaussian_ids: tuple[int, ...]
    cost_cycles: int
    gradient: tuple[float, ...]
    corrected_gradient: tuple[float, ...]
    avoidable_read_bytes: int


@dataclass(frozen=True)
class TrackingTile:
    tile_id: int
    retention_deadline_ns: int


@dataclass(frozen=True)
class Workload:
    name: str
    model: str
    threshold: tuple[float, ...]
    pose_hessian_diagonal: tuple[float, ...]
    mapping_tiles: tuple[MappingTile, ...]
    gaussians: tuple[Gaussian, ...]
    rays: tuple[Ray, ...]
    tracking_tiles: tuple[TrackingTile, ...]


def load_workload(document: Any) -> Workload:
    if not isinstance(document, dict):
        raise ValueError("workload root must be an object")
    required = {
        "format",
        "name",
        "model",
        "threshold",
        "pose_hessian_diagonal",
        "mapping_tiles",
        "gaussians",
        "tracking_tiles",
        "rays",
    }
    if set(document) != required or document["format"] != "cisda-workload/v1":
        raise ValueError("workload does not match cisda-workload/v1")

    mapping_tiles = tuple(
        MappingTile(
            int(item["id"]),
            int(item["arrival"]),
            int(item["cost_cycles"]),
            tuple(int(value) for value in item["gaussians"]),
        )
        for item in document["mapping_tiles"]
    )
    gaussians = tuple(
        Gaussian(
            int(item["id"]),
            int(item["version"]),
            int(item["group"]),
            tuple(int(bit) for bit in item["history"]),
            int(item["counter"]),
            bool(item["resident"]),
            _fields(item["fields"], f"gaussian {item['id']} fields"),
            _fields(item["updated_fields"], f"gaussian {item['id']} updated_fields"),
            _vector(item["impact"], f"gaussian {item['id']} impact"),
            bool(item["impact_valid"]),
            int(item["impact_ready_ns"]),
            bool(item["coverage_changed"]),
        )
        for item in document["gaussians"]
    )
    rays = tuple(
        Ray(
            int(item["id"]),
            int(item["tile"]),
            tuple(int(value) for value in item["gaussians"]),
            int(item["cost_cycles"]),
            _vector(item["gradient"], f"ray {item['id']} gradient"),
            _vector(item["corrected_gradient"],
                    f"ray {item['id']} corrected_gradient"),
            int(item["avoidable_read_bytes"]),
        )
        for item in document["rays"]
    )
    tracking_tiles = tuple(
        TrackingTile(int(item["id"]), int(item["retention_deadline_ns"]))
        for item in document["tracking_tiles"]
    )

    workload = Workload(
        str(document["name"]),
        str(document["model"]),
        _vector(document["threshold"], "threshold"),
        _vector(document["pose_hessian_diagonal"], "pose_hessian_diagonal"),
        mapping_tiles,
        gaussians,
        rays,
        tracking_tiles,
    )
    _validate(workload)
    return workload


def _validate(workload: Workload) -> None:
    if workload.model not in {"SplaTAM", "Gaussian-SLAM", "VTGaussian-SLAM"}:
        raise ValueError("model is not supported")
    if any(value <= 0 for value in workload.threshold):
        raise ValueError("threshold components must be positive")
    if any(value <= 0 for value in workload.pose_hessian_diagonal):
        raise ValueError("pose Hessian diagonal components must be positive")

    tile_ids = [tile.tile_id for tile in workload.mapping_tiles]
    gaussian_ids = [gaussian.gaussian_id for gaussian in workload.gaussians]
    tracking_ids = [tile.tile_id for tile in workload.tracking_tiles]
    ray_ids = [ray.ray_id for ray in workload.rays]
    for values, name in (
        (tile_ids, "mapping tile"),
        (gaussian_ids, "Gaussian"),
        (tracking_ids, "tracking tile"),
        (ray_ids, "ray"),
    ):
        if len(values) != len(set(values)):
            raise ValueError(f"duplicate {name} ID")

    gaussian_set = set(gaussian_ids)
    tracking_set = set(tracking_ids)
    produced: set[int] = set()
    for tile in workload.mapping_tiles:
        if tile.cost_cycles <= 0 or not tile.gaussian_ids:
            raise ValueError("mapping tiles require positive cost and Gaussians")
        if not set(tile.gaussian_ids) <= gaussian_set:
            raise ValueError("mapping tile refers to an unknown Gaussian")
        produced.update(tile.gaussian_ids)
    if produced != gaussian_set:
        raise ValueError("each Gaussian needs at least one mapping source")

    for gaussian in workload.gaussians:
        if gaussian.version < 0 or gaussian.group < 0 or gaussian.group >= 128:
            raise ValueError("Gaussian version or group is out of range")
        if len(gaussian.history) > 4 or any(bit not in (0, 1) for bit in gaussian.history):
            raise ValueError("reuse history must contain at most four bits")
        if gaussian.counter < 0 or gaussian.counter > 3:
            raise ValueError("predictor counter must be in [0, 3]")
        if set(gaussian.fields) != set(gaussian.updated_fields):
            raise ValueError("updated fields must preserve the field layout")
        for field in gaussian.fields:
            if len(gaussian.fields[field]) != len(gaussian.updated_fields[field]):
                raise ValueError("updated fields must preserve vector widths")
    for ray in workload.rays:
        if ray.tile_id not in tracking_set or ray.cost_cycles <= 0:
            raise ValueError("ray has an unknown tile or invalid cost")
        if not ray.gaussian_ids or not set(ray.gaussian_ids) <= gaussian_set:
            raise ValueError("ray refers to an unknown Gaussian")
        if ray.avoidable_read_bytes < 0:
            raise ValueError("avoidable read bytes cannot be negative")
    if {ray.tile_id for ray in workload.rays} != tracking_set:
        raise ValueError("each tracking tile needs at least one ray")
