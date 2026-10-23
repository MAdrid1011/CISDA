from __future__ import annotations

from dataclasses import dataclass

from .model import Gaussian


@dataclass(frozen=True)
class MergeDecision:
    gaussian_id: int
    kept_visible: bool
    changed_fields: tuple[str, ...]
    visible_bytes_written: int
    version: int


class ImpactPim:
    """Primary-state update and cumulative impact-constrained visible merge."""

    def __init__(self, threshold: tuple[float, ...]) -> None:
        self._threshold = threshold
        self._contributions: dict[int, dict[int, tuple[float, ...]]] = {}
        self._visible_versions: dict[int, int] = {}
        self._primary: dict[int, dict[str, tuple[float, ...]]] = {}
        self._visible: dict[int, dict[str, tuple[float, ...]]] = {}

    def update_and_validate(self, gaussian: Gaussian) -> MergeDecision:
        self._primary[gaussian.gaussian_id] = dict(gaussian.updated_fields)
        visible = self._visible.setdefault(
            gaussian.gaussian_id, dict(gaussian.fields)
        )
        group = self._contributions.setdefault(gaussian.group, {})
        previous = group.get(gaussian.gaussian_id, (0.0,) * 6)
        total_without_previous = tuple(
            sum(values[index] for gaussian_id, values in group.items()
                if gaussian_id != gaussian.gaussian_id)
            for index in range(6)
        )
        candidate = tuple(
            total_without_previous[index] + gaussian.impact[index]
            for index in range(6)
        )
        expected_version = gaussian.version
        visible_version = self._visible_versions.setdefault(
            gaussian.gaussian_id, gaussian.version
        )
        version_valid = visible_version == expected_version
        keep = (
            gaussian.impact_valid
            and version_valid
            and not gaussian.coverage_changed
            and all(candidate[index] <= self._threshold[index]
                    for index in range(6))
        )
        if keep:
            group[gaussian.gaussian_id] = gaussian.impact
            return MergeDecision(
                gaussian.gaussian_id,
                True,
                (),
                0,
                gaussian.version,
            )

        changed = tuple(
            field for field in visible
            if visible[field] != self._primary[gaussian.gaussian_id][field]
        )
        visible_bytes = 4 * sum(
            len(self._primary[gaussian.gaussian_id][field]) for field in changed
        )
        for field in changed:
            visible[field] = self._primary[gaussian.gaussian_id][field]
        group[gaussian.gaussian_id] = (0.0,) * 6
        self._visible_versions[gaussian.gaussian_id] = gaussian.version + 1
        return MergeDecision(
            gaussian.gaussian_id,
            False,
            changed,
            visible_bytes,
            gaussian.version + 1,
        )


class PimUnits:
    UNIT_COUNT = 8
    PERIOD_NS = 5
    UPDATE_VALIDATE_CYCLES = 12

    def __init__(self) -> None:
        self._available = [0] * self.UNIT_COUNT

    def schedule(self, gaussian_id: int, release_ns: int) -> tuple[int, int]:
        owner = gaussian_id % self.UNIT_COUNT
        start = max(release_ns, self._available[owner])
        finish = start + self.UPDATE_VALIDATE_CYCLES * self.PERIOD_NS
        self._available[owner] = finish
        return start, finish
