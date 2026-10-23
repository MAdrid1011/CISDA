from __future__ import annotations

from collections import defaultdict
from typing import Any

from .impact_pim import ImpactPim, MergeDecision, PimUnits
from .model import Gaussian, Ray, Workload, load_workload
from .ordering import gaussian_aware_order
from .predictor import Prediction, ReusePredictor
from .scheduler import ArrayTask, SharedGsArray, reverse_dataflow_order


PROJECTION_CYCLES = 8
TILE_REDUCTION_CYCLES = 4
POSE_SOLVE_CYCLES = 18


def _sum_vectors(vectors: list[tuple[float, ...]]) -> tuple[float, ...]:
    return tuple(sum(vector[index] for vector in vectors) for index in range(6))


def run_workload(document: Any) -> dict[str, Any]:
    workload = load_workload(document)
    gaussians = {item.gaussian_id: item for item in workload.gaussians}
    mapping_tiles = {item.tile_id: item for item in workload.mapping_tiles}
    rays_by_tile: dict[int, list[Ray]] = defaultdict(list)
    for ray in workload.rays:
        rays_by_tile[ray.tile_id].append(ray)

    predictor = ReusePredictor()
    predictions: dict[int, Prediction] = {}
    for gaussian in workload.gaussians:
        predictor.seed(gaussian.gaussian_id, gaussian.history, gaussian.counter)
        tiles = tuple(sorted({
            ray.tile_id for ray in workload.rays
            if gaussian.gaussian_id in ray.gaussian_ids
        }))
        predictions[gaussian.gaussian_id] = predictor.predict(
            gaussian.gaussian_id,
            gaussian.version + 1,
            0,
            tiles,
        )

    speculative = {
        gaussian_id for gaussian_id, prediction in predictions.items()
        if prediction.reuse and gaussians[gaussian_id].resident
    }
    base_order = gaussian_aware_order(workload.mapping_tiles)
    issue_order, critical_paths = reverse_dataflow_order(
        base_order,
        workload.mapping_tiles,
        workload.rays,
        workload.tracking_tiles,
        speculative,
    )

    array = SharedGsArray()
    old_projection_finish: dict[int, int] = {}
    for gaussian_id in sorted(speculative):
        old_projection_finish[gaussian_id] = array.schedule(ArrayTask(
            f"project-old-{gaussian_id}", "gaussian", 0, PROJECTION_CYCLES,
            (gaussian_id,),
        )).finish_ns

    mapping_finish: dict[int, int] = {}
    for tile_id in issue_order:
        tile = mapping_tiles[tile_id]
        mapping_finish[tile_id] = array.schedule(ArrayTask(
            f"mapping-{tile_id}", "tile", 0, tile.cost_cycles,
            tile.gaussian_ids,
        )).finish_ns

    sources: dict[int, list[int]] = defaultdict(list)
    for tile in workload.mapping_tiles:
        for gaussian_id in tile.gaussian_ids:
            sources[gaussian_id].append(tile.tile_id)

    pim_units = PimUnits()
    decision_finish: dict[int, int] = {}
    scheduled_decisions: list[tuple[int, Gaussian]] = []
    for gaussian in workload.gaussians:
        gradient_ready = max(mapping_finish[tile_id] for tile_id in sources[gaussian.gaussian_id])
        release = max(gradient_ready, gaussian.impact_ready_ns)
        _, finish = pim_units.schedule(gaussian.gaussian_id, release)
        decision_finish[gaussian.gaussian_id] = finish
        scheduled_decisions.append((finish, gaussian))

    impact_pim = ImpactPim(workload.threshold)
    decisions: dict[int, MergeDecision] = {}
    for _, gaussian in sorted(scheduled_decisions,
                              key=lambda item: (item[0], item[1].gaussian_id)):
        decision = impact_pim.update_and_validate(gaussian)
        decisions[gaussian.gaussian_id] = decision
        predictor.feedback(predictions[gaussian.gaussian_id], decision.kept_visible)

    new_projection_finish: dict[int, int] = {}
    for gaussian in workload.gaussians:
        if gaussian.gaussian_id not in speculative or not decisions[gaussian.gaussian_id].kept_visible:
            release = decision_finish[gaussian.gaussian_id]
            new_projection_finish[gaussian.gaussian_id] = array.schedule(ArrayTask(
                f"project-new-{gaussian.gaussian_id}", "gaussian", release,
                PROJECTION_CYCLES, (gaussian.gaussian_id,),
            )).finish_ns

    ray_finish: dict[int, int] = {}
    corrected_finish: dict[int, int] = {}
    corrected_rays: set[int] = set()
    structural_rebuilds = 0
    for ray in sorted(workload.rays, key=lambda item: item.ray_id):
        operand_ready = []
        for gaussian_id in ray.gaussian_ids:
            if gaussian_id in speculative:
                operand_ready.append(old_projection_finish[gaussian_id])
            else:
                operand_ready.append(new_projection_finish[gaussian_id])
        ray_finish[ray.ray_id] = array.schedule(ArrayTask(
            f"ray-{ray.ray_id}", "ray", max(operand_ready), ray.cost_cycles,
            ray.gaussian_ids,
        )).finish_ns

        advanced = [
            gaussian_id for gaussian_id in ray.gaussian_ids
            if gaussian_id in speculative and not decisions[gaussian_id].kept_visible
        ]
        if advanced:
            corrected_rays.add(ray.ray_id)
            structural_rebuilds += int(any(
                gaussians[gaussian_id].coverage_changed for gaussian_id in advanced
            ))
            release = max(
                ray_finish[ray.ray_id],
                *(new_projection_finish[gaussian_id] for gaussian_id in advanced),
            )
            corrected_finish[ray.ray_id] = array.schedule(ArrayTask(
                f"correct-{ray.ray_id}", "ray", release, ray.cost_cycles,
                ray.gaussian_ids,
            )).finish_ns

    tile_finish: dict[int, int] = {}
    tile_gradients: dict[int, tuple[float, ...]] = {}
    for tile in sorted(workload.tracking_tiles, key=lambda item: item.tile_id):
        rays = sorted(rays_by_tile[tile.tile_id], key=lambda item: item.ray_id)
        contribution_ready = max(
            corrected_finish.get(ray.ray_id, ray_finish[ray.ray_id]) for ray in rays
        )
        validation_ready = max(
            decision_finish[gaussian_id]
            for ray in rays for gaussian_id in ray.gaussian_ids
        )
        gaussian_ids = tuple(sorted({
            gaussian_id for ray in rays for gaussian_id in ray.gaussian_ids
        }))
        tile_finish[tile.tile_id] = array.schedule(ArrayTask(
            f"reduce-{tile.tile_id}", "tile",
            max(contribution_ready, validation_ready), TILE_REDUCTION_CYCLES,
            gaussian_ids,
        )).finish_ns
        tile_gradients[tile.tile_id] = _sum_vectors([
            ray.corrected_gradient if ray.ray_id in corrected_rays else ray.gradient
            for ray in rays
        ])

    global_gradient = _sum_vectors([
        tile_gradients[tile_id] for tile_id in sorted(tile_gradients)
    ])
    pose_delta = tuple(
        -global_gradient[index] / workload.pose_hessian_diagonal[index]
        for index in range(6)
    )
    pose_start = max(tile_finish.values())
    elapsed_ns = pose_start + POSE_SOLVE_CYCLES * SharedGsArray.PERIOD_NS

    confirms = sum(decision.kept_visible for decision in decisions.values())
    advances = len(decisions) - confirms
    return {
        "format": "cisda-result/v1",
        "workload": workload.name,
        "model": workload.model,
        "elapsed_ns": elapsed_ns,
        "stage_completion_ns": {
            "mapping_gradient": max(mapping_finish.values()),
            "impact_validation": max(decision_finish.values()),
            "ray_rendering": max(
                max(ray_finish.values()),
                max(corrected_finish.values(), default=0),
            ),
            "pose_commit": elapsed_ns,
        },
        "gaussian_aware_tile_order": base_order,
        "issued_mapping_tile_order": issue_order,
        "pose_delta": [round(value, 9) for value in pose_delta],
        "counts": {
            "mapping_tiles": len(workload.mapping_tiles),
            "mapping_gradient_fragments": sum(
                len(tile.gaussian_ids) for tile in workload.mapping_tiles
            ),
            "gaussians_updated": len(workload.gaussians),
            "tracking_rays": len(workload.rays),
            "tracking_tiles_committed": len(workload.tracking_tiles),
            "speculative_gaussians": len(speculative),
            "speculative_rays": sum(
                any(gaussian_id in speculative for gaussian_id in ray.gaussian_ids)
                for ray in workload.rays
            ),
            "reuse_confirmations": confirms,
            "visible_state_advances": advances,
            "corrected_rays": len(corrected_rays),
            "structural_workset_rebuilds": structural_rebuilds,
            "reverse_priority_mapping_tiles": critical_paths,
            "shared_array_tasks": array.tasks,
            "shared_array_mode_switches": array.mode_switches,
            "impact_pim_updates": len(workload.gaussians),
            "pose_commits": 1,
        },
        "bytes": {
            "primary_state_written": sum(
                gaussian.state_bytes for gaussian in workload.gaussians
            ),
            "visible_state_written": sum(
                decision.visible_bytes_written for decision in decisions.values()
            ),
        },
    }
