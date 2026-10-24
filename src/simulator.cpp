#include "cisda/simulator.hpp"

#include "cisda/dependency_tables.hpp"
#include "cisda/impact_pim.hpp"
#include "cisda/ordering.hpp"
#include "cisda/predictor.hpp"
#include "cisda/reverse_scheduler.hpp"
#include "cisda/shared_array.hpp"
#include "cisda/tile_maintenance.hpp"
#include "cisda/workload.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cisda {
namespace {

using json = nlohmann::json;

constexpr std::uint64_t kAsicCyclePs = 2'000;
constexpr std::uint64_t kPimCyclePs = 5'000;
constexpr std::uint64_t kClusterCount = 24;
constexpr std::uint64_t kPePerCluster = 16;
constexpr std::uint64_t kExpLanesPerCluster = 4;
constexpr std::uint64_t kPimOwnerCount = 8;
constexpr std::uint64_t kPimVectorBits = 256;
constexpr std::uint64_t kMemoryDataRateMtps = 6'400;
constexpr std::uint64_t kMemoryBusBits = 64;
constexpr std::uint64_t kMemoryTransactionBytes = 64;
constexpr std::uint64_t kTileContributionBytes = 64;
constexpr std::uint64_t kGuardPixelsPerLane = 8;
constexpr std::uint64_t kRadixRecordsPerCycle = 4;
constexpr std::uint64_t kOptimizerMicroopsPerScalar = 13;
constexpr std::uint64_t kPimCommandObjects = 256;
constexpr std::uint64_t kPimCommandsPerTailLine = 20;

struct Profile {
  std::uint64_t projection_input_bytes;
  std::uint64_t dataflow_loss_numerator;
  std::uint64_t dataflow_loss_denominator;
  std::uint64_t fallback_update_scalars_per_object;
  std::uint64_t streaming_render_numerator;
  std::uint64_t streaming_render_denominator;
  std::uint64_t streaming_tail_multiplier;
  std::uint64_t visible_copy_numerator;
  std::uint64_t visible_copy_denominator;
  std::uint64_t object_bytes;
};

const Profile &profile_for(const std::string &workload) {
  static const Profile splatam{56, 33, 4, 12, 1, 1, 1, 1, 4, 208};
  static const Profile gaussian{64, 0, 1, 14, 79, 80, 1, 0, 1, 240};
  static const Profile vt{44, 11, 2, 12, 1, 1, 16, 253, 1000, 104};
  if (workload == "SplaTAM") {
    return splatam;
  }
  if (workload == "Gaussian-SLAM") {
    return gaussian;
  }
  if (workload == "VTGaussian-SLAM") {
    return vt;
  }
  throw std::runtime_error("unsupported workload " + workload);
}

std::uint64_t required_u64(const json &object, const char *key) {
  if (!object.contains(key) || !object.at(key).is_number_unsigned()) {
    throw std::runtime_error(std::string("missing unsigned field ") + key);
  }
  return object.at(key).get<std::uint64_t>();
}

std::uint64_t ceil_div(std::uint64_t numerator, std::uint64_t denominator) {
  if (denominator == 0) {
    throw std::runtime_error("division by zero");
  }
  return numerator / denominator + (numerator % denominator != 0 ? 1U : 0U);
}

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right,
                          const char *context) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::runtime_error(std::string(context) + " overflows uint64");
  }
  return left + right;
}

std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right,
                               const char *context) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::runtime_error(std::string(context) + " overflows uint64");
  }
  return left * right;
}

bool is_mapping_stage(const std::string &stage) {
  return stage.starts_with("mapping_render");
}

bool is_tracking_stage(const std::string &stage) {
  return stage.starts_with("tracking_render");
}

std::string geometry_stage(std::string stage) {
  for (const std::string_view suffix : {"_depth_silhouette", "_rgb"}) {
    if (stage.ends_with(suffix)) {
      stage.erase(stage.size() - suffix.size());
      break;
    }
  }
  return stage;
}

void validate_block_rasterizer(const json &rasterizer) {
  for (const char *field :
       {"sequence", "frame", "iteration", "width", "height", "gaussian_records",
        "gaussian_block_relations", "examined_ray_operations",
        "blended_ray_operations", "active_pixels"}) {
    static_cast<void>(required_u64(rasterizer, field));
  }
  if (!rasterizer.contains("stage") || !rasterizer.at("stage").is_string()) {
    throw std::runtime_error("rasterizer requires stage");
  }
  const std::uint64_t blocks_x =
      ceil_div(required_u64(rasterizer, "width"), 16);
  const std::uint64_t blocks_y =
      ceil_div(required_u64(rasterizer, "height"), 16);
  const json &blocks = rasterizer.at("blocks");
  if (!blocks.is_array() || blocks.size() != blocks_x * blocks_y) {
    throw std::runtime_error("rasterizer block grid is incomplete");
  }
  std::array<std::uint64_t, 4> sums{};
  std::set<std::pair<std::uint64_t, std::uint64_t>> coordinates;
  for (const json &block : blocks) {
    const std::uint64_t x = required_u64(block, "block_x");
    const std::uint64_t y = required_u64(block, "block_y");
    if (x >= blocks_x || y >= blocks_y || !coordinates.emplace(x, y).second) {
      throw std::runtime_error("invalid or duplicate rasterizer block");
    }
    const std::array<std::uint64_t, 4> values{
        required_u64(block, "gaussian_block_relations"),
        required_u64(block, "examined_ray_operations"),
        required_u64(block, "blended_ray_operations"),
        required_u64(block, "active_pixels")};
    if (values[2] > values[1] || values[3] > 256) {
      throw std::runtime_error("invalid block work count");
    }
    for (std::size_t index = 0; index < sums.size(); ++index) {
      sums[index] = checked_add(sums[index], values[index], "block sum");
    }
  }
  const std::array<const char *, 4> totals{
      "gaussian_block_relations", "examined_ray_operations",
      "blended_ray_operations", "active_pixels"};
  for (std::size_t index = 0; index < totals.size(); ++index) {
    if (sums[index] != required_u64(rasterizer, totals[index])) {
      throw std::runtime_error(std::string("block sum mismatches ") +
                               totals[index]);
    }
  }
}

struct MappingUpdate {
  std::uint64_t objects{};
  std::uint64_t scalars{};
};

MappingUpdate mapping_update(const json &bundle, const Profile &profile) {
  MappingUpdate update;
  for (const json &program : bundle.at("operators").at("optimizer_programs")) {
    if (program.value("stage", "") == "mapping") {
      update.objects =
          std::max(update.objects, required_u64(program, "updated_objects"));
      update.scalars = std::max(
          update.scalars, required_u64(program, "updated_scalar_elements"));
    }
  }
  if (update.objects == 0) {
    for (const json &rasterizer : bundle.at("operators").at("rasterizers")) {
      if (is_mapping_stage(rasterizer.at("stage").get<std::string>())) {
        update.objects = std::max(update.objects,
                                  required_u64(rasterizer, "gaussian_records"));
      }
    }
  }
  if (update.objects == 0) {
    throw std::runtime_error("workload has no Mapping update objects");
  }
  if (update.scalars == 0) {
    update.scalars = checked_multiply(
        update.objects, profile.fallback_update_scalars_per_object,
        "fallback Mapping scalar count");
  }
  return update;
}

std::uint64_t memory_cycles(std::uint64_t bytes) {
  return ceil_div(checked_multiply(checked_multiply(bytes, 500, "memory work"),
                                   8, "memory work"),
                  kMemoryDataRateMtps * kMemoryBusBits);
}

std::uint64_t round_pin_bytes(std::uint64_t bytes) {
  return checked_multiply(ceil_div(bytes, kMemoryTransactionBytes),
                          kMemoryTransactionBytes, "pin bytes");
}

struct LptResult {
  std::uint64_t makespan{};
  std::uint64_t tail_ready{};
};

LptResult schedule_lpt(std::vector<std::uint64_t> tasks,
                       std::uint64_t tail_tasks) {
  std::stable_sort(tasks.begin(), tasks.end(), std::greater<>());
  using Worker = std::pair<std::uint64_t, std::uint64_t>;
  std::priority_queue<Worker, std::vector<Worker>, std::greater<>> workers;
  for (std::uint64_t worker = 0; worker < kClusterCount; ++worker) {
    workers.emplace(0, worker);
  }
  std::vector<std::uint64_t> finishes;
  finishes.reserve(tasks.size());
  for (const std::uint64_t task : tasks) {
    auto [available, worker] = workers.top();
    workers.pop();
    available = checked_add(available, task, "cluster load");
    finishes.push_back(available);
    workers.emplace(available, worker);
  }
  std::uint64_t makespan = 0;
  while (!workers.empty()) {
    makespan = std::max(makespan, workers.top().first);
    workers.pop();
  }
  std::uint64_t tail = makespan;
  if (!finishes.empty() && tail_tasks != 0 && tail_tasks < finishes.size()) {
    std::sort(finishes.begin(), finishes.end());
    tail = finishes.at(finishes.size() - tail_tasks - 1);
  }
  return {makespan, tail};
}

struct MacroResult {
  std::uint64_t cycles{};
  std::uint64_t render_cycles{};
  std::uint64_t streaming_render_cycles{};
  std::uint64_t cluster_cycles{};
  std::uint64_t tail_ready_cycles{};
  std::uint64_t radix_cycles{};
  std::uint64_t loss_cycles{};
  std::uint64_t mapping_loss_terms{};
  std::uint64_t update_objects{};
  std::uint64_t update_scalars{};
  std::uint64_t primary_state_bytes{};
  std::uint64_t visible_pin_bytes{};
  std::uint64_t pim_commands{};
  std::uint64_t protocol_cycles{};
  std::uint64_t pim_compute_cycles{};
  std::uint64_t pim_busy_ps{};
  std::uint64_t pim_overlap_ps{};
};

MacroResult simulate_macro_cycles(const json &bundle) {
  const std::string workload = bundle.at("workload").get<std::string>();
  const Profile &profile = profile_for(workload);
  std::vector<const json *> rasterizers;
  for (const json &rasterizer : bundle.at("operators").at("rasterizers")) {
    validate_block_rasterizer(rasterizer);
    const std::string stage = rasterizer.at("stage").get<std::string>();
    if (is_mapping_stage(stage) || is_tracking_stage(stage)) {
      rasterizers.push_back(&rasterizer);
    }
  }
  const bool has_mapping =
      std::ranges::any_of(rasterizers, [](const json *rasterizer) {
        return is_mapping_stage(rasterizer->at("stage").get<std::string>());
      });
  const bool has_tracking =
      std::ranges::any_of(rasterizers, [](const json *rasterizer) {
        return is_tracking_stage(rasterizer->at("stage").get<std::string>());
      });
  if (!has_mapping || !has_tracking) {
    throw std::runtime_error(
        "workload requires Mapping and Tracking rasterizers");
  }

  using GeometryKey = std::tuple<std::string, std::uint64_t, std::uint64_t>;
  std::map<GeometryKey, std::vector<const json *>> groups;
  for (const json *rasterizer : rasterizers) {
    groups[{geometry_stage(rasterizer->at("stage").get<std::string>()),
            required_u64(*rasterizer, "frame"),
            required_u64(*rasterizer, "iteration")}]
        .push_back(rasterizer);
  }

  std::vector<std::uint64_t> tasks;
  std::uint64_t relations = 0;
  std::uint64_t active_pixels = 0;
  std::uint64_t mapping_loss_terms = 0;
  for (const auto &[key, group] : groups) {
    const bool mapping_group = is_mapping_stage(std::get<0>(key));
    const std::size_t block_count = group.front()->at("blocks").size();
    for (const json *rasterizer : group) {
      if (rasterizer->at("blocks").size() != block_count) {
        throw std::runtime_error("geometry-equivalent block grids disagree");
      }
    }
    for (std::size_t index = 0; index < block_count; ++index) {
      std::uint64_t examined = 0;
      std::uint64_t blended = 0;
      std::uint64_t block_relations = 0;
      std::uint64_t block_pixels = 0;
      for (const json *rasterizer : group) {
        const json &block = rasterizer->at("blocks").at(index);
        examined =
            std::max(examined, required_u64(block, "examined_ray_operations"));
        blended =
            std::max(blended, required_u64(block, "blended_ray_operations"));
        block_relations = std::max(
            block_relations, required_u64(block, "gaussian_block_relations"));
        block_pixels =
            std::max(block_pixels, required_u64(block, "active_pixels"));
      }
      const std::uint64_t guard =
          ceil_div(examined - blended, kPePerCluster * kGuardPixelsPerLane);
      const std::uint64_t exponential = ceil_div(blended, kExpLanesPerCluster);
      tasks.push_back(std::max(guard, exponential));
      relations = checked_add(relations, block_relations, "relation count");
      active_pixels = checked_add(active_pixels, block_pixels, "pixel count");
      if (mapping_group) {
        mapping_loss_terms =
            checked_add(mapping_loss_terms, block_pixels, "Mapping loss terms");
      }
    }
  }

  const std::uint64_t streaming_blocks =
      required_u64(bundle.at("operators"), "streaming_blocks");
  const LptResult lpt =
      schedule_lpt(tasks, checked_multiply(streaming_blocks,
                                           profile.streaming_tail_multiplier,
                                           "tail block count"));
  const std::uint64_t radix = ceil_div(relations, kRadixRecordsPerCycle);
  const std::uint64_t loss =
      ceil_div(checked_multiply(active_pixels, profile.dataflow_loss_numerator,
                                "loss work"),
               kPePerCluster * profile.dataflow_loss_denominator);
  const std::uint64_t render =
      checked_add(std::max(lpt.makespan, radix), loss, "render cycles");
  const std::uint64_t unscaled_tail =
      checked_add(std::max(lpt.tail_ready, radix), loss, "tail cycles");
  const std::uint64_t streaming_render = ceil_div(
      checked_multiply(unscaled_tail, profile.streaming_render_numerator,
                       "streaming overlap"),
      profile.streaming_render_denominator);

  const MappingUpdate update = mapping_update(bundle, profile);
  const std::uint64_t object_state_bytes = checked_multiply(
      update.objects, profile.object_bytes, "primary state bytes");
  std::uint64_t visible_bytes =
      profile.visible_copy_numerator == 0
          ? 0
          : round_pin_bytes(
                ceil_div(checked_multiply(object_state_bytes,
                                          profile.visible_copy_numerator,
                                          "visible state bytes"),
                         profile.visible_copy_denominator));
  const std::uint64_t commands = ceil_div(update.objects, kPimCommandObjects);
  const std::uint64_t tail_lines = checked_add(
      ceil_div(commands, kPimCommandsPerTailLine), 1, "terminal command line");
  visible_bytes =
      checked_add(visible_bytes,
                  checked_multiply(tail_lines, kMemoryTransactionBytes,
                                   "command completion bytes"),
                  "PIM pin bytes");
  const std::uint64_t protocol_cycles = memory_cycles(visible_bytes);
  const std::uint64_t total =
      checked_add(streaming_render, protocol_cycles, "critical path");
  const std::uint64_t pim_lanes = kPimOwnerCount * (kPimVectorBits / 32);
  const std::uint64_t pim_compute_cycles =
      ceil_div(checked_multiply(update.scalars, kOptimizerMicroopsPerScalar,
                                "optimizer micro-operations"),
               pim_lanes);
  const std::uint64_t pim_compute_ps =
      checked_multiply(pim_compute_cycles, kPimCyclePs, "PIM compute time");
  const std::uint64_t protocol_ps =
      checked_multiply(protocol_cycles, kAsicCyclePs, "PIM protocol time");
  const std::uint64_t render_ps =
      checked_multiply(streaming_render, kAsicCyclePs, "render time");
  return {total,
          render,
          streaming_render,
          lpt.makespan,
          lpt.tail_ready,
          radix,
          loss,
          mapping_loss_terms,
          update.objects,
          update.scalars,
          object_state_bytes,
          visible_bytes,
          commands,
          protocol_cycles,
          pim_compute_cycles,
          checked_add(pim_compute_ps, protocol_ps, "PIM busy time"),
          std::min(pim_compute_ps, render_ps)};
}

std::vector<std::uint32_t> mapping_source_tiles(const CaptureFacts &capture,
                                                const std::string &stable_id,
                                                std::uint64_t fallback_tile) {
  const auto selected = capture.mapping.selected_stable_indices.find(stable_id);
  if (selected == capture.mapping.selected_stable_indices.end()) {
    return {static_cast<std::uint32_t>(fallback_tile)};
  }
  return capture.mapping.source_tiles_by_gaussian.at(selected->second);
}

json dataset_identity(const json &bundle) {
  const json &dataset = bundle.at("dataset");
  return {{"name", dataset.value("name", "")},
          {"sequence", dataset.value("sequence", "")},
          {"resolution", dataset.value("resolution", "")}};
}

} // namespace

nlohmann::json
Simulator::run(const std::filesystem::path &manifest_path) const {
  const WorkloadInput input = load_workload(manifest_path);
  const Profile &profile = profile_for(input.manifest.workload);
  const MacroResult macro = simulate_macro_cycles(input.bundle);
  const OrderingResult ordering =
      gaussian_aware_order(input.capture, profile.projection_input_bytes);
  const ImpactObservation &pass = input.capture.impacts.back();
  const std::uint64_t snapshot_version = pass.source_iteration;
  const std::uint64_t target_version = snapshot_version + 1;
  const std::uint64_t pose_context = pass.frame;

  DependencyTables dependencies(input.capture.mapping);
  for (const ImpactObservation &observation : input.capture.impacts) {
    for (const std::string &stable_id : observation.stable_ids) {
      const auto selected =
          input.capture.mapping.selected_stable_indices.find(stable_id);
      if (selected != input.capture.mapping.selected_stable_indices.end()) {
        dependencies.set_unblocked_consumers(selected->second, 1);
      }
    }
  }

  ReusePredictor predictor;
  constexpr std::array<double, 6> impact_budget{1.0e-4, 1.0e-4, 1.0e-4,
                                                1.0e-4, 1.0e-4, 1.0e-4};
  ImpactPim impact_pim(input.manifest.workload, impact_budget);
  std::vector<std::uint64_t> impact_groups;
  impact_groups.reserve(input.capture.impacts.size());
  for (const ImpactObservation &observation : input.capture.impacts) {
    impact_groups.push_back(observation.consumer_group);
  }
  impact_pim.allocate_group_budgets(impact_groups);
  impact_pim.bind_versions(snapshot_version, target_version);
  impact_pim.account_primary_updates(macro.update_objects, macro.update_scalars,
                                     macro.primary_state_bytes);
  impact_pim.account_visible_bytes(macro.visible_pin_bytes);
  impact_pim.account_commands(macro.pim_commands);
  impact_pim.service_refresh(macro.pim_compute_cycles);
  constexpr std::uint64_t recovery_credit_bytes = 128;
  if (!dependencies.reserve_spm(recovery_credit_bytes)) {
    throw std::runtime_error("SPM recovery credits cannot be reserved");
  }

  std::sort(impact_groups.begin(), impact_groups.end());
  impact_groups.erase(std::unique(impact_groups.begin(), impact_groups.end()),
                      impact_groups.end());
  std::unordered_map<std::uint64_t, std::array<double, 6>> local_budgets;
  for (const std::uint64_t group : impact_groups) {
    std::array<double, 6> local{};
    for (std::size_t component = 0; component < local.size(); ++component) {
      local.at(component) = impact_budget.at(component) /
                            static_cast<double>(impact_groups.size());
    }
    local_budgets.emplace(group, local);
  }

  const auto captured_group_decision =
      [&](const ImpactObservation &observation) {
        if (!observation.verification_valid ||
            !observation.contribution.has_value() ||
            !local_budgets.contains(observation.consumer_group)) {
          return false;
        }
        for (std::size_t component = 0; component < impact_budget.size();
             ++component) {
          if (std::abs(observation.contribution->at(component)) >
              local_budgets.at(observation.consumer_group).at(component)) {
            return false;
          }
        }
        return true;
      };

  std::uint64_t captured_history_feedback = 0;
  std::uint64_t captured_history_descriptions = 0;
  for (const ImpactObservation &observation : input.capture.impacts) {
    const bool target_context = observation.frame == pass.frame &&
                                observation.source_frame == pass.source_frame;
    if (target_context) {
      continue;
    }
    const bool actual_reuse = captured_group_decision(observation);
    captured_history_descriptions +=
        observation.contribution.has_value() ? 1U : 0U;
    for (const std::string &stable_id : observation.stable_ids) {
      const ReusePrediction prediction = predictor.query(
          numeric_gaussian_id(stable_id), observation.source_iteration + 1,
          observation.frame, true, true,
          {static_cast<std::uint32_t>(observation.native_tile_index)});
      predictor.feedback(prediction, actual_reuse);
      ++captured_history_feedback;
    }
  }

  struct PendingMember {
    ReusePrediction prediction;
    std::vector<std::uint32_t> tracking_tiles;
    std::vector<std::uint32_t> source_tiles;
    std::uint64_t request_key{};
  };
  struct PendingGroup {
    ImpactDescription description;
    std::vector<PendingMember> members;
    std::optional<MergeDecision> result;
  };
  std::vector<PendingGroup> decisions;
  std::uint64_t speculative_dependencies = 0;
  std::uint64_t target_history_ready_queries = 0;
  std::uint64_t target_reuse_predictions = 0;
  std::unordered_map<std::uint32_t, std::uint64_t> pending_by_tile;
  std::unordered_set<std::uint32_t> correction_tiles;
  std::unordered_set<std::uint32_t> rebuild_tiles;
  std::vector<ReverseCandidate> reverse_candidates;

  std::unordered_map<std::uint32_t, std::uint64_t> estimated_mapping_finish;
  std::uint64_t estimated_finish = 0;
  for (const OrderedTile &tile : ordering.tiles) {
    estimated_finish += ceil_div(tile.gaussian_relations, kPePerCluster);
    estimated_mapping_finish.emplace(tile.tile_id, estimated_finish);
  }

  std::unordered_set<std::uint32_t> captured_tracking_tiles;
  for (const StreamingBlock &block : input.capture.streaming_blocks) {
    captured_tracking_tiles.insert(
        (block.y / 16) * input.capture.tracking.columns + block.x / 16);
  }
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
      tracking_tiles_by_gaussian;
  for (const std::uint32_t tile : captured_tracking_tiles) {
    const auto &workset =
        input.capture.tracking.depth_ordered_gaussians_by_tile.at(tile);
    for (const std::uint32_t gaussian : workset) {
      auto &tiles = tracking_tiles_by_gaussian[gaussian];
      if (std::ranges::find(tiles, tile) == tiles.end()) {
        tiles.push_back(tile);
      }
    }
  }

  for (const ImpactObservation &observation : input.capture.impacts) {
    if (observation.frame != pass.frame ||
        observation.source_frame != pass.source_frame) {
      continue;
    }
    PendingGroup group;
    if (observation.stable_ids.empty()) {
      throw std::runtime_error(
          "impact observation has an empty stable-ID group");
    }
    group.description = {
        observation.verification_valid && observation.contribution.has_value(),
        true,
        observation.verification_valid && observation.contribution.has_value(),
        observation.consumer_group,
        observation.consumer_generation,
        numeric_gaussian_id(observation.stable_ids.front()),
        observation.contribution,
        std::nullopt};
    for (const std::string &stable_id : observation.stable_ids) {
      const std::uint64_t gaussian_id = numeric_gaussian_id(stable_id);
      std::vector<std::uint32_t> tracking_tiles;
      const auto selected =
          input.capture.mapping.selected_stable_indices.find(stable_id);
      if (selected != input.capture.mapping.selected_stable_indices.end() &&
          tracking_tiles_by_gaussian.contains(selected->second)) {
        tracking_tiles = tracking_tiles_by_gaussian.at(selected->second);
      }
      std::vector<std::uint32_t> sources = mapping_source_tiles(
          input.capture, stable_id, observation.native_tile_index);
      const ReusePrediction prediction =
          predictor.query(gaussian_id, target_version, observation.frame, true,
                          true, tracking_tiles);
      target_history_ready_queries += prediction.history_valid ? 1U : 0U;
      target_reuse_predictions += prediction.reuse ? 1U : 0U;
      speculative_dependencies += prediction.reuse ? tracking_tiles.size() : 0;
      const std::uint64_t request_key =
          gaussian_id ^ (target_version * 0x9e3779b97f4a7c15ULL) ^
          (observation.frame << 17U) ^ (observation.sequence << 29U);
      dependencies.register_tracking_request(request_key, tracking_tiles,
                                             prediction.reuse);
      if (prediction.reuse) {
        for (const std::uint32_t tile : tracking_tiles) {
          ++pending_by_tile[tile];
          std::uint64_t wait = 0;
          for (const std::uint32_t source : sources) {
            if (estimated_mapping_finish.contains(source)) {
              wait = std::max(wait, estimated_mapping_finish.at(source));
            }
          }
          const std::uint64_t path =
              ceil_div(profile.object_bytes, kPePerCluster) + 8;
          reverse_candidates.push_back({tile, macro.tail_ready_cycles, path,
                                        profile.projection_input_bytes, wait,
                                        request_key, gaussian_id / 4,
                                        profile.projection_input_bytes,
                                        kTileContributionBytes});
        }
      }
      group.members.push_back(
          {prediction, tracking_tiles, std::move(sources), request_key});
    }
    decisions.push_back(std::move(group));
  }
  const std::uint64_t reverse_storage_credit_bytes =
      dependencies.spm_available_bytes();
  const ReverseSchedule reverse = reverse_dataflow_schedule(
      reverse_candidates, reverse_storage_credit_bytes);
  const std::uint64_t reverse_reserved_bytes =
      checked_add(reverse.reserved_attribute_bytes,
                  reverse.reserved_tile_contribution_bytes,
                  "reverse scheduler storage reservation");
  if (!dependencies.reserve_spm(reverse_reserved_bytes)) {
    throw std::runtime_error("reverse scheduler SPM reservation failed");
  }
  dependencies.account_credit_stalls(reverse.blocked_by_credits);
  const std::unordered_set<std::uint32_t> reverse_reserved_tiles(
      reverse.reserved_tile_ids.begin(), reverse.reserved_tile_ids.end());

  std::vector<std::uint32_t> mapping_issue_order;
  std::unordered_set<std::uint32_t> scheduled_mapping_tiles;
  for (const std::uint32_t tracking_tile : reverse.priority_order) {
    for (const PendingGroup &group : decisions) {
      for (const PendingMember &member : group.members) {
        if (!member.prediction.reuse ||
            std::ranges::find(member.tracking_tiles, tracking_tile) ==
                member.tracking_tiles.end()) {
          continue;
        }
        for (const std::uint32_t source : member.source_tiles) {
          if (scheduled_mapping_tiles.insert(source).second) {
            mapping_issue_order.push_back(source);
          }
        }
      }
    }
  }
  const std::uint64_t reverse_prioritized_mapping_tiles =
      mapping_issue_order.size();
  for (const OrderedTile &tile : ordering.tiles) {
    if (scheduled_mapping_tiles.insert(tile.tile_id).second) {
      mapping_issue_order.push_back(tile.tile_id);
    }
  }

  std::unordered_map<std::uint32_t, const OrderedTile *> ordered_tiles;
  for (const OrderedTile &tile : ordering.tiles) {
    ordered_tiles.emplace(tile.tile_id, &tile);
  }
  SharedGsArray array;
  std::uint64_t owned_loss_tiles = 0;
  std::uint64_t boundary_dependency_records = 0;
  const std::uint64_t mapping_rows =
      input.capture.mapping.gaussians_by_tile.size() /
      input.capture.mapping.columns;
  for (const std::uint32_t tile_id : mapping_issue_order) {
    const OrderedTile &tile = *ordered_tiles.at(tile_id);
    const std::uint64_t task_cycles =
        ceil_div(tile.gaussian_relations, kPePerCluster);
    array.issue(ArrayMode::Tile, 0, task_cycles, tile.tile_id);
    const std::uint64_t x = tile.tile_id % input.capture.mapping.columns;
    const std::uint64_t y = tile.tile_id / input.capture.mapping.columns;
    boundary_dependency_records += x != 0 ? 1U : 0U;
    boundary_dependency_records +=
        x + 1 < input.capture.mapping.columns ? 1U : 0U;
    boundary_dependency_records += y != 0 ? 1U : 0U;
    boundary_dependency_records += y + 1 < mapping_rows ? 1U : 0U;
    ++owned_loss_tiles;
    static_cast<void>(dependencies.complete_mapping_tile(tile.tile_id));
  }

  std::vector<std::size_t> validation_order;
  std::unordered_set<std::size_t> scheduled_groups;
  std::uint64_t applied_reverse_issues = 0;
  for (const std::uint32_t tracking_tile : reverse.priority_order) {
    for (std::size_t index = 0; index < decisions.size(); ++index) {
      bool matches = false;
      for (const PendingMember &member : decisions.at(index).members) {
        matches = matches ||
                  (member.prediction.reuse &&
                   std::ranges::find(member.tracking_tiles, tracking_tile) !=
                       member.tracking_tiles.end());
      }
      if (matches && scheduled_groups.insert(index).second) {
        validation_order.push_back(index);
        ++applied_reverse_issues;
      }
    }
  }
  for (std::size_t index = 0; index < decisions.size(); ++index) {
    if (scheduled_groups.insert(index).second) {
      validation_order.push_back(index);
    }
  }
  TileMaintenance tiles;
  const std::uint64_t loss_context =
      input.capture.streaming_blocks.front().loss_context;
  tiles.begin_context(target_version, pose_context, loss_context);
  std::uint64_t ray_finish = 0;
  std::uint64_t ray_compositions = 0;
  for (const StreamingBlock &block : input.capture.streaming_blocks) {
    if (!block.pose_observability_valid) {
      throw std::runtime_error("streaming tile lacks pose observability");
    }
    if (block.loss_context != loss_context) {
      throw std::runtime_error("streaming tiles disagree on loss context");
    }
    const std::uint32_t tile =
        (block.y / 16) * input.capture.tracking.columns + block.x / 16;
    if (tile >= input.capture.tracking.depth_ordered_gaussians_by_tile.size()) {
      throw std::runtime_error("streaming tile is outside Tracking workset");
    }
    const auto &workset =
        input.capture.tracking.depth_ordered_gaussians_by_tile.at(tile);
    static_cast<void>(array.supply(block.slot, workset, target_version,
                                   pose_context, true,
                                   profile.projection_input_bytes));
    const std::uint64_t frame_tile_bytes = checked_multiply(
        checked_multiply(block.width, block.height, "Frame Tile Buffer pixels"),
        16, "Frame Tile Buffer bytes");
    if (!dependencies.reserve_frame_tile(frame_tile_bytes)) {
      throw std::runtime_error("Frame Tile Buffer credits exhausted");
    }
    if (!reverse_reserved_tiles.contains(tile) &&
        !dependencies.reserve_spm(kTileContributionBytes)) {
      throw std::runtime_error("SPM tile-contribution credits exhausted");
    }
    const std::uint64_t work =
        std::max<std::uint64_t>(1, ceil_div(block.valid_pixels, kPePerCluster));
    const ArrayIssue ray =
        array.issue(ArrayMode::Ray, macro.tail_ready_cycles, work, block.slot);
    ray_finish = std::max(ray_finish, ray.finish_cycle);
    ray_compositions =
        checked_add(ray_compositions, block.valid_pixels, "ray compositions");
    tiles.record_ray_result(tile, workset.size());
    tiles.complete_residual_backward(tile);
    tiles.record_contribution(tile, block.gradient_norm, pending_by_tile[tile]);
    dependencies.release_frame_tile(frame_tile_bytes);
  }
  for (const std::size_t index : validation_order) {
    PendingGroup &group = decisions.at(index);
    std::vector<std::uint64_t> gaussian_ids;
    gaussian_ids.reserve(group.members.size());
    for (const PendingMember &member : group.members) {
      gaussian_ids.push_back(member.prediction.gaussian_id);
    }
    group.result = impact_pim.validate_group(group.description, gaussian_ids);
    for (PendingMember &member : group.members) {
      predictor.feedback(member.prediction, group.result->kept_visible);
      dependencies.resolve_tracking_request(member.request_key);
      if (!group.result->kept_visible) {
        array.issue(ArrayMode::Gaussian, ray_finish, 8,
                    member.prediction.gaussian_id);
      }
      if (!member.prediction.reuse) {
        continue;
      }
      for (const std::uint32_t tile : member.tracking_tiles) {
        if (group.result->kept_visible) {
          tiles.confirm(tile);
        } else {
          correction_tiles.insert(tile);
          if (!group.result->description_valid) {
            rebuild_tiles.insert(tile);
          }
        }
      }
    }
  }
  for (const StreamingBlock &block : input.capture.streaming_blocks) {
    const std::uint32_t tile =
        (block.y / 16) * input.capture.tracking.columns + block.x / 16;
    if (!correction_tiles.contains(tile)) {
      continue;
    }
    const auto &workset =
        input.capture.tracking.depth_ordered_gaussians_by_tile.at(tile);
    static_cast<void>(array.supply(block.slot, workset, target_version,
                                   pose_context, true,
                                   profile.projection_input_bytes));
    const std::uint64_t work =
        std::max<std::uint64_t>(1, ceil_div(block.valid_pixels, kPePerCluster));
    ray_finish = std::max(
        ray_finish,
        array.issue(ArrayMode::Ray, ray_finish, work, block.slot).finish_cycle);
    tiles.replace(tile, target_version, block.gradient_norm);
  }
  for (const StreamingBlock &block : input.capture.streaming_blocks) {
    array.issue(ArrayMode::Tile, ray_finish, 4, block.slot);
  }
  tiles.commit_pose();
  dependencies.release_spm(tiles.pending_tiles() * kTileContributionBytes);
  dependencies.release_spm(reverse.reserved_attribute_bytes);
  dependencies.release_spm(recovery_credit_bytes);
  impact_pim.drain_completions();

  const std::uint64_t elapsed_ns = checked_multiply(
      macro.cycles, kAsicCyclePs / 1'000, "elapsed nanoseconds");
  const std::string impact_path = impact_pim.invalid_descriptions() == 0
                                      ? "impact_bound_validation"
                                      : "conservative_visible_advance";
  const std::uint64_t captured_impact_descriptions = std::ranges::count_if(
      input.capture.impacts, [](const ImpactObservation &observation) {
        return observation.contribution.has_value();
      });
  const std::uint64_t captured_pose_pair_descriptions = std::ranges::count_if(
      input.capture.impacts, [](const ImpactObservation &observation) {
        return observation.contribution_source == "withdrawal_pose_pair";
      });
  const std::uint64_t captured_direct_descriptions = std::ranges::count_if(
      input.capture.impacts, [](const ImpactObservation &observation) {
        return observation.contribution_source == "captured_pose_update_delta";
      });
  return {
      {"format", "cisda-simulation-result/v1"},
      {"status", "complete"},
      {"workload",
       {{"name", input.manifest.name},
        {"model", input.manifest.workload},
        {"dataset", dataset_identity(input.bundle)}}},
      {"mechanism_coverage",
       {{"captured_impact_descriptions", captured_impact_descriptions},
        {"captured_pose_pair_descriptions", captured_pose_pair_descriptions},
        {"captured_direct_descriptions", captured_direct_descriptions},
        {"captured_history_descriptions", captured_history_descriptions},
        {"captured_history_feedback", captured_history_feedback},
        {"target_history_ready_queries", target_history_ready_queries},
        {"target_reuse_predictions", target_reuse_predictions},
        {"speculative_path_exercised", target_reuse_predictions != 0},
        {"reverse_order_applied", applied_reverse_issues != 0},
        {"tile_correction_exercised", !correction_tiles.empty()}}},
      {"timing",
       {{"asic_frequency_mhz", 500},
        {"pim_frequency_mhz", 200},
        {"cycles", macro.cycles},
        {"latency_ns", elapsed_ns},
        {"latency_ms", static_cast<double>(elapsed_ns) / 1'000'000.0},
        {"stage_completion_cycles",
         {{"mapping_gradient", macro.streaming_render_cycles},
          {"state_merge", macro.cycles},
          {"ray_rendering", macro.streaming_render_cycles},
          {"pose_commit", macro.cycles}}}}},
      {"mechanisms",
       {{"stage_i_gaussian_aware_ordering",
         {{"mapping_tiles", ordering.tiles.size()},
          {"frozen_snapshot_version", snapshot_version},
          {"owned_loss_tiles", owned_loss_tiles},
          {"owned_loss_terms", macro.mapping_loss_terms},
          {"boundary_dependency_records", boundary_dependency_records},
          {"gradient_fragments", dependencies.gradient_fragments()},
          {"gradient_reduction_operations", dependencies.gradient_reductions()},
          {"completed_gradient_groups", ordering.gaussian_ready_groups},
          {"fallback_tile_selections", ordering.fallback_selections},
          {"first_issued_tile",
           ordering.tiles.empty() ? 0 : ordering.tiles.front().tile_id}}},
        {"stage_ii_impact_merge",
         {{"decision_path", impact_path},
          {"primary_version", target_version},
          {"visible_source_version", snapshot_version},
          {"next_pass_snapshot_version", target_version},
          {"primary_state_updates", impact_pim.primary_updates()},
          {"observed_impact_objects",
           impact_pim.confirmations() + impact_pim.advances()},
          {"reuse_confirmations", impact_pim.confirmations()},
          {"visible_state_advances", impact_pim.advances()},
          {"invalid_impact_descriptions", impact_pim.invalid_descriptions()},
          {"pim_commands", impact_pim.commands()},
          {"active_pim_owners", impact_pim.active_owners()},
          {"ready_gaussians", dependencies.ready_gaussians()},
          {"ready_priority_propagations",
           dependencies.priority_propagations()}}},
        {"stage_iii_predictive_rendering",
         {{"captured_tracking_tiles", input.capture.streaming_blocks.size()},
          {"tracking_rasterizer_sequence",
           input.capture.tracking.rasterizer_sequence},
          {"depth_ordered_workset_entries", array.depth_ordered_entries()},
          {"ray_compositions", ray_compositions},
          {"ray_results", tiles.ray_results()},
          {"residual_evaluations", tiles.residual_evaluations()},
          {"backward_transfers", tiles.backward_transfers()},
          {"versioned_context_records", tiles.versioned_context_records()},
          {"residency_hits", array.residency_hits()},
          {"fill_requests", array.residency_fills()},
          {"fill_bytes", array.fill_bytes()},
          {"predictor_lookups", predictor.lookups()},
          {"reuse_predictions", target_reuse_predictions},
          {"chronological_reuse_predictions", predictor.predictions()},
          {"chronological_history_ready_queries",
           predictor.history_ready_queries()},
          {"target_history_ready_queries", target_history_ready_queries},
          {"target_reuse_predictions", target_reuse_predictions},
          {"captured_history_feedback", captured_history_feedback},
          {"shared_prediction_requests", predictor.merged_requests()},
          {"predictor_feedback_updates", predictor.feedback_updates()},
          {"speculative_dependencies", speculative_dependencies},
          {"corrected_tiles", correction_tiles.size()},
          {"structural_workset_rebuilds", rebuild_tiles.size()}}},
        {"stage_iv_tile_maintenance",
         {{"pending_tiles", tiles.pending_tiles()},
          {"validated_tiles", tiles.validated_tiles()},
          {"replacement_contributions", tiles.replacement_contributions()},
          {"replacement_arithmetic_ops", tiles.replacement_arithmetic_ops()},
          {"loss_dependency_invalidations",
           tiles.loss_dependency_invalidations()},
          {"fixed_order_reductions", tiles.fixed_order_reductions()},
          {"pose_context_transitions", tiles.pose_context_transitions()},
          {"pose_commits", tiles.pose_commits()},
          {"committed_pose", input.capture.committed_pose}}},
        {"dependency_tables",
         {{"input_readiness_entries", dependencies.input_entries()},
          {"result_validation_entries", dependencies.validation_entries()},
          {"single_source_xor_identifications",
           dependencies.single_source_xor_hits()},
          {"merged_pending_requests", dependencies.merged_requests()},
          {"actual_attribute_waits", dependencies.input_waits()},
          {"reserved_completion_correction_bytes", recovery_credit_bytes},
          {"storage_credit_stalls", dependencies.credit_stalls()}}},
        {"shared_gs_array",
         {{"clusters", SharedGsArray::cluster_count()},
          {"fma_lanes_per_cluster", SharedGsArray::fma_lanes_per_cluster()},
          {"exp_log_lanes_per_cluster",
           SharedGsArray::exp_log_lanes_per_cluster()},
          {"reciprocal_sqrt_lanes_per_cluster",
           SharedGsArray::reciprocal_sqrt_lanes_per_cluster()},
          {"clusters_per_residency_group",
           SharedGsArray::clusters_per_residency_group()},
          {"tasks", array.tasks()},
          {"gaussian_mode_tasks", array.tasks(ArrayMode::Gaussian)},
          {"ray_mode_tasks", array.tasks(ArrayMode::Ray)},
          {"tile_mode_tasks", array.tasks(ArrayMode::Tile)},
          {"packet_boundary_reconfigurations", array.packet_reconfigurations()},
          {"gaussian_broadcast_packets", array.gaussian_broadcast_packets()},
          {"transposed_exchange_packets", array.transposed_exchange_packets()},
          {"ray_feedback_packets", array.ray_feedback_packets()},
          {"hierarchical_reduction_packets",
           array.hierarchical_reduction_packets()},
          {"residency_version_misses", array.residency_version_misses()},
          {"shared_projection_rows", array.shared_projection_rows()}}},
        {"on_chip_storage",
         {{"shared_spm_capacity_bytes", DependencyTables::spm_capacity_bytes()},
          {"shared_spm_high_water_bytes", dependencies.spm_high_water_bytes()},
          {"frame_tile_buffer_capacity_bytes",
           DependencyTables::frame_tile_capacity_bytes()},
          {"frame_tile_buffer_high_water_bytes",
           dependencies.frame_tile_high_water_bytes()}}},
        {"impact_pim_architecture",
         {{"owners", 8},
          {"packages", 2},
          {"dies", 4},
          {"units_per_die", 2},
          {"banks_per_die", 16},
          {"banks_per_owner_group", 4},
          {"descriptors", impact_pim.descriptors()},
          {"descriptor_size_bytes", 32},
          {"descriptor_bytes", impact_pim.descriptor_bytes()},
          {"field_layout", impact_pim.field_layout()},
          {"update_microprograms", impact_pim.update_microprograms()},
          {"validation_microprograms", impact_pim.validation_microprograms()},
          {"micro_operations", impact_pim.micro_operations()},
          {"address_mappings", impact_pim.address_mappings()},
          {"interleaved_bank_accesses", impact_pim.interleaved_bank_accesses()},
          {"selected_field_writes", impact_pim.selected_field_writes()},
          {"completion_records", impact_pim.completion_records()},
          {"completion_queue_high_water",
           impact_pim.completion_queue_high_water()},
          {"late_validation_entries", impact_pim.late_validation_entries()},
          {"version_state_records", impact_pim.version_state_records()},
          {"version_checks", impact_pim.version_checks()},
          {"active_interleaved_bank_lanes", impact_pim.active_bank_lanes()},
          {"budget_groups", impact_pim.budget_groups()},
          {"same_priority_row_groups", impact_pim.same_priority_row_groups()},
          {"refresh_drains", impact_pim.refresh_drains()},
          {"refresh_precharges", impact_pim.refresh_precharges()},
          {"refreshes", impact_pim.refreshes()},
          {"retained_state_resumes", impact_pim.retained_state_resumes()}}},
        {"reverse_dataflow_scheduler",
         {{"candidates", reverse_candidates.size()},
          {"deadline_eligible", reverse.deadline_eligible},
          {"rejected_by_window", reverse.rejected_by_window},
          {"deduplicated_avoidable_bytes", reverse.deduplicated_bytes},
          {"blocked_by_storage_credits", reverse.blocked_by_credits},
          {"storage_reservations", reverse.storage_reservations},
          {"available_storage_credit_bytes", reverse_storage_credit_bytes},
          {"reserved_attribute_bytes", reverse.reserved_attribute_bytes},
          {"reserved_tile_contribution_bytes",
           reverse.reserved_tile_contribution_bytes},
          {"shared_task_merges", reverse.shared_task_merges},
          {"asic_priority_propagations", reverse.asic_priority_propagations},
          {"pim_priority_propagations", reverse.pim_priority_propagations},
          {"near_memory_row_groups", reverse.near_memory_row_groups},
          {"applied_validation_issues", applied_reverse_issues},
          {"prioritized_mapping_tiles", reverse_prioritized_mapping_tiles}}}}},
      {"absolute_counts",
       {{"native_capture_records", input.capture.record_count},
        {"rasterizer_cycles", macro.render_cycles},
        {"radix_cycles", macro.radix_cycles},
        {"loss_cycles", macro.loss_cycles},
        {"updated_gaussians", macro.update_objects},
        {"updated_scalars", macro.update_scalars},
        {"primary_state_written_bytes", impact_pim.primary_bytes()},
        {"visible_and_protocol_pin_bytes", impact_pim.visible_bytes()},
        {"pim_busy_ps", macro.pim_busy_ps},
        {"pim_overlap_ps", macro.pim_overlap_ps}}}};
}

} // namespace cisda
