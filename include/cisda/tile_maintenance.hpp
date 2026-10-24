#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cisda {

class TileMaintenance {
public:
  void begin_context(std::uint64_t map_version, std::uint64_t pose_context,
                     std::uint64_t loss_context);
  void record_ray_result(std::uint32_t tile,
                         std::uint64_t depth_ordered_entries);
  void complete_residual_backward(std::uint32_t tile);
  void record_contribution(std::uint32_t tile, double captured_gradient_norm,
                           std::uint64_t pending_dependencies);
  void confirm(std::uint32_t tile);
  void replace(std::uint32_t tile, std::uint64_t target_version,
               double captured_gradient_norm);
  void invalidate_loss_dependents(const std::vector<std::uint32_t> &tiles);
  void commit_pose();

  std::uint64_t pending_tiles() const;
  std::uint64_t validated_tiles() const;
  std::uint64_t replacement_contributions() const;
  std::uint64_t ray_results() const;
  std::uint64_t residual_evaluations() const;
  std::uint64_t backward_transfers() const;
  std::uint64_t versioned_context_records() const;
  std::uint64_t replacement_arithmetic_ops() const;
  std::uint64_t loss_dependency_invalidations() const;
  std::uint64_t fixed_order_reductions() const;
  std::uint64_t pose_context_transitions() const;
  std::uint64_t pose_commits() const;

private:
  struct Slot {
    std::uint64_t map_version{};
    std::uint64_t pose_context{};
    std::uint64_t loss_context{};
    double contribution{};
    std::uint64_t pending{};
    bool valid{};
  };

  struct RayState {
    std::uint64_t ordered_entries{};
    bool residual_ready{};
    bool backward_ready{};
  };

  std::uint64_t map_version_{};
  std::uint64_t pose_context_{};
  std::uint64_t loss_context_{};
  std::unordered_map<std::uint32_t, Slot> slots_;
  std::unordered_map<std::uint32_t, RayState> rays_;
  std::uint64_t replacements_{};
  std::uint64_t ray_results_{};
  std::uint64_t residual_evaluations_{};
  std::uint64_t backward_transfers_{};
  std::uint64_t replacement_ops_{};
  std::uint64_t loss_invalidations_{};
  std::uint64_t reductions_{};
  std::uint64_t pose_transitions_{};
  std::uint64_t commits_{};
};

} // namespace cisda
