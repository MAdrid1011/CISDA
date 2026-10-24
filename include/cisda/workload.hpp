#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace cisda {

struct WorkloadManifest {
  std::string name;
  std::string workload;
  std::filesystem::path bundle_path;
  std::filesystem::path capture_path;
  std::filesystem::path model_root;
  std::filesystem::path dataset_root;
};

struct StreamingBlock {
  std::uint32_t slot{};
  std::uint32_t completion_order{};
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint64_t valid_pixels{};
  double gradient_norm{};
  std::uint64_t rasterizer_sequence{};
  std::uint64_t loss_context{};
  bool pose_observability_valid{};
};

struct ImpactObservation {
  std::uint64_t sequence{};
  std::uint64_t frame{};
  std::uint64_t iteration{};
  std::uint64_t source_frame{};
  std::uint64_t source_iteration{};
  std::uint64_t consumer_group{};
  std::uint64_t consumer_generation{};
  std::uint64_t native_tile_index{};
  std::string label;
  bool verification_valid{};
  std::optional<std::array<double, 6>> contribution;
  std::string contribution_source;
  std::vector<std::string> stable_ids;
};

struct MappingTopology {
  std::uint32_t columns{};
  std::vector<std::uint64_t> relation_counts;
  std::vector<std::vector<std::uint32_t>> source_tiles_by_gaussian;
  std::vector<std::vector<std::uint32_t>> gaussians_by_tile;
  std::unordered_map<std::string, std::uint32_t> selected_stable_indices;
};

struct TrackingTopology {
  std::uint64_t rasterizer_sequence{};
  std::uint32_t columns{};
  std::vector<std::vector<std::uint32_t>> depth_ordered_gaussians_by_tile;
};

struct CaptureFacts {
  std::string workload;
  std::uint64_t record_count{};
  bool candidate_pose_accepted{};
  nlohmann::json committed_pose;
  std::vector<StreamingBlock> streaming_blocks;
  std::vector<ImpactObservation> impacts;
  MappingTopology mapping;
  TrackingTopology tracking;
};

struct WorkloadInput {
  WorkloadManifest manifest;
  nlohmann::json bundle;
  CaptureFacts capture;
};

WorkloadInput load_workload(const std::filesystem::path &manifest_path);

} // namespace cisda
