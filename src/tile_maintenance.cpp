#include "cisda/tile_maintenance.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace cisda {

void TileMaintenance::begin_context(std::uint64_t map_version,
                                    std::uint64_t pose_context,
                                    std::uint64_t loss_context) {
  if (!slots_.empty()) {
    ++pose_transitions_;
  }
  map_version_ = map_version;
  pose_context_ = pose_context;
  loss_context_ = loss_context;
  slots_.clear();
  rays_.clear();
}

void TileMaintenance::record_ray_result(std::uint32_t tile,
                                        std::uint64_t depth_ordered_entries) {
  if (!rays_.emplace(tile, RayState{depth_ordered_entries, false, false})
           .second) {
    throw std::runtime_error("ray result was recorded twice");
  }
  ++ray_results_;
}

void TileMaintenance::complete_residual_backward(std::uint32_t tile) {
  RayState &ray = rays_.at(tile);
  ray.residual_ready = true;
  ray.backward_ready = true;
  ++residual_evaluations_;
  ++backward_transfers_;
}

void TileMaintenance::record_contribution(std::uint32_t tile,
                                          double captured_gradient_norm,
                                          std::uint64_t pending_dependencies) {
  const RayState &ray = rays_.at(tile);
  if (!ray.residual_ready || !ray.backward_ready) {
    throw std::runtime_error(
        "tile contribution precedes residual/backward completion");
  }
  const Slot slot{map_version_,         pose_context_,
                  loss_context_,        captured_gradient_norm,
                  pending_dependencies, pending_dependencies == 0};
  if (!slots_.emplace(tile, slot).second) {
    throw std::runtime_error("tile contribution was recorded twice");
  }
}

void TileMaintenance::confirm(std::uint32_t tile) {
  Slot &slot = slots_.at(tile);
  if (slot.pending == 0) {
    return;
  }
  --slot.pending;
  slot.valid = slot.pending == 0;
}

void TileMaintenance::replace(std::uint32_t tile, std::uint64_t target_version,
                              double captured_gradient_norm) {
  Slot &slot = slots_.at(tile);
  slot.valid = false;
  slot.contribution = -slot.contribution;
  slot.contribution += captured_gradient_norm;
  slot.map_version = target_version;
  slot.pending = 0;
  slot.valid = true;
  ++replacements_;
  replacement_ops_ += 2;
}

void TileMaintenance::invalidate_loss_dependents(
    const std::vector<std::uint32_t> &tiles) {
  for (const std::uint32_t tile : tiles) {
    const auto slot = slots_.find(tile);
    if (slot != slots_.end() && slot->second.valid) {
      slot->second.valid = false;
      ++loss_invalidations_;
    }
  }
}

void TileMaintenance::commit_pose() {
  std::vector<std::pair<std::uint32_t, double>> ordered;
  ordered.reserve(slots_.size());
  for (const auto &[tile, slot] : slots_) {
    if (!slot.valid || slot.pending != 0 || slot.map_version != map_version_) {
      throw std::runtime_error("pose commit has an invalid tile contribution");
    }
    ordered.emplace_back(tile, slot.contribution);
  }
  std::sort(ordered.begin(), ordered.end());
  double fixed_order_sum = 0.0;
  for (const auto &[tile, contribution] : ordered) {
    static_cast<void>(tile);
    fixed_order_sum += contribution;
    ++reductions_;
  }
  static_cast<void>(fixed_order_sum);
  ++pose_transitions_;
  ++commits_;
}

std::uint64_t TileMaintenance::pending_tiles() const { return slots_.size(); }
std::uint64_t TileMaintenance::validated_tiles() const {
  return std::count_if(slots_.begin(), slots_.end(), [](const auto &entry) {
    return entry.second.valid && entry.second.pending == 0;
  });
}
std::uint64_t TileMaintenance::replacement_contributions() const {
  return replacements_;
}
std::uint64_t TileMaintenance::ray_results() const { return ray_results_; }
std::uint64_t TileMaintenance::residual_evaluations() const {
  return residual_evaluations_;
}
std::uint64_t TileMaintenance::backward_transfers() const {
  return backward_transfers_;
}
std::uint64_t TileMaintenance::versioned_context_records() const {
  return slots_.size();
}
std::uint64_t TileMaintenance::replacement_arithmetic_ops() const {
  return replacement_ops_;
}
std::uint64_t TileMaintenance::loss_dependency_invalidations() const {
  return loss_invalidations_;
}
std::uint64_t TileMaintenance::fixed_order_reductions() const {
  return reductions_;
}
std::uint64_t TileMaintenance::pose_context_transitions() const {
  return pose_transitions_;
}
std::uint64_t TileMaintenance::pose_commits() const { return commits_; }

} // namespace cisda
