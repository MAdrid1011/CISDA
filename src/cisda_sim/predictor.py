from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Prediction:
    gaussian_id: int
    target_version: int
    context: int
    index: int
    reuse: bool
    tiles: tuple[int, ...]


class ReusePredictor:
    TABLE_SIZE = 2048

    def __init__(self) -> None:
        self._counters = [1] * self.TABLE_SIZE
        self._histories: dict[int, tuple[int, ...]] = {}
        self._pending: dict[tuple[int, int, int], Prediction] = {}

    @staticmethod
    def _fold_id(gaussian_id: int) -> int:
        folded = 0
        value = gaussian_id
        while value:
            folded ^= value & 0x7FF
            value >>= 11
        return folded & 0x7FF

    @classmethod
    def _index(cls, gaussian_id: int, history: tuple[int, ...]) -> int:
        history_bits = 0
        for bit in history[-4:]:
            history_bits = (history_bits << 1) | bit
        folded = cls._fold_id(gaussian_id)
        return (folded & ~0xF) | ((folded ^ history_bits) & 0xF)

    def seed(self, gaussian_id: int, history: tuple[int, ...], counter: int) -> None:
        self._histories[gaussian_id] = history
        self._counters[self._index(gaussian_id, history)] = counter

    def predict(
        self,
        gaussian_id: int,
        target_version: int,
        context: int,
        tiles: tuple[int, ...],
    ) -> Prediction:
        key = (gaussian_id, target_version, context)
        if key in self._pending:
            previous = self._pending[key]
            merged = Prediction(
                previous.gaussian_id,
                previous.target_version,
                previous.context,
                previous.index,
                previous.reuse,
                tuple(sorted(set(previous.tiles) | set(tiles))),
            )
            self._pending[key] = merged
            return merged
        history = self._histories.get(gaussian_id, ())
        index = self._index(gaussian_id, history)
        prediction = Prediction(
            gaussian_id,
            target_version,
            context,
            index,
            len(history) == 4 and self._counters[index] >= 2,
            tuple(sorted(set(tiles))),
        )
        self._pending[key] = prediction
        return prediction

    def feedback(self, prediction: Prediction, kept: bool) -> None:
        key = (prediction.gaussian_id, prediction.target_version, prediction.context)
        if self._pending.get(key) != prediction:
            raise ValueError("predictor feedback does not match a pending decision")
        counter = self._counters[prediction.index]
        self._counters[prediction.index] = min(3, counter + 1) if kept else max(0, counter - 1)
        history = self._histories.get(prediction.gaussian_id, ())
        self._histories[prediction.gaussian_id] = (history + (int(kept),))[-4:]
        del self._pending[key]
