#include "cisda/workload.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

namespace cisda {
namespace {

using json = nlohmann::json;

std::filesystem::path resolve_path(const std::filesystem::path &base,
                                   const json &manifest, const char *field) {
  if (!manifest.contains(field) || !manifest.at(field).is_string() ||
      manifest.at(field).get_ref<const std::string &>().empty()) {
    throw std::runtime_error(std::string("manifest requires ") + field);
  }
  std::filesystem::path value = manifest.at(field).get<std::string>();
  if (!value.is_absolute()) {
    value = base / value;
  }
  return std::filesystem::weakly_canonical(value);
}

json read_json(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("cannot open " + path.string());
  }
  try {
    return json::parse(stream);
  } catch (const json::exception &error) {
    throw std::runtime_error("cannot parse " + path.string() + ": " +
                             error.what());
  }
}

void require_file(const std::filesystem::path &path, const char *name) {
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error(std::string(name) +
                             " is not a file: " + path.string());
  }
}

void require_directory(const std::filesystem::path &path, const char *name) {
  if (!std::filesystem::is_directory(path)) {
    throw std::runtime_error(std::string(name) +
                             " is not a directory: " + path.string());
  }
  if (std::filesystem::directory_iterator(path) ==
      std::filesystem::directory_iterator()) {
    throw std::runtime_error(std::string(name) + " is empty: " + path.string());
  }
}

template <typename Callback>
void for_each_line(const std::filesystem::path &path, Callback callback) {
  if (path.extension() != ".gz") {
    std::ifstream stream(path);
    if (!stream) {
      throw std::runtime_error("cannot open " + path.string());
    }
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty()) {
        callback(line);
      }
    }
    if (!stream.eof()) {
      throw std::runtime_error("cannot read " + path.string());
    }
    return;
  }

  gzFile stream = gzopen(path.c_str(), "rb");
  if (stream == nullptr) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::array<char, 64 * 1024> chunk{};
  std::string pending;
  while (true) {
    const int count =
        gzread(stream, chunk.data(), static_cast<unsigned int>(chunk.size()));
    if (count < 0) {
      int code = Z_OK;
      const char *message = gzerror(stream, &code);
      gzclose(stream);
      throw std::runtime_error("cannot read " + path.string() + ": " +
                               (message == nullptr ? "zlib error" : message));
    }
    if (count == 0) {
      break;
    }
    pending.append(chunk.data(), static_cast<std::size_t>(count));
    std::size_t begin = 0;
    while (true) {
      const std::size_t end = pending.find('\n', begin);
      if (end == std::string::npos) {
        pending.erase(0, begin);
        break;
      }
      if (end != begin) {
        callback(std::string_view(pending).substr(begin, end - begin));
      }
      begin = end + 1;
    }
  }
  if (!pending.empty()) {
    callback(pending);
  }
  if (gzclose(stream) != Z_OK) {
    throw std::runtime_error("cannot close " + path.string());
  }
}

bool record_is(std::string_view line, std::string_view type) {
  const std::size_t key = line.find("\"record_type\"");
  return key != std::string_view::npos &&
         line.find(type, key) != std::string_view::npos;
}

std::uint64_t unsigned_value(const json &value, const char *name) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string(name) + " must be an integer");
  }
  const auto result = value.get<std::int64_t>();
  if (result < 0) {
    throw std::runtime_error(std::string(name) + " must be nonnegative");
  }
  return static_cast<std::uint64_t>(result);
}

std::vector<std::uint8_t> decode_base64(std::string_view text) {
  static constexpr std::array<std::int16_t, 256> table = [] {
    std::array<std::int16_t, 256> result{};
    result.fill(-1);
    for (std::uint16_t value = 0; value < 26; ++value) {
      result[static_cast<unsigned char>('A' + value)] = value;
      result[static_cast<unsigned char>('a' + value)] = value + 26;
    }
    for (std::uint16_t value = 0; value < 10; ++value) {
      result[static_cast<unsigned char>('0' + value)] = value + 52;
    }
    result[static_cast<unsigned char>('+')] = 62;
    result[static_cast<unsigned char>('/')] = 63;
    return result;
  }();

  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() * 3 / 4);
  std::uint32_t accumulator = 0;
  unsigned int bits = 0;
  for (const unsigned char character : text) {
    if (character == '=') {
      break;
    }
    if (std::isspace(character) != 0) {
      continue;
    }
    const std::int16_t value = table[character];
    if (value < 0) {
      throw std::runtime_error("capture contains invalid base64 data");
    }
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<std::uint8_t>(accumulator >> bits));
      accumulator &= (1U << bits) - 1U;
    }
  }
  return bytes;
}

std::vector<std::int32_t> decode_i32(const json &tensor, const char *name) {
  if (tensor.value("encoding", "") != "base64-little-endian-v1" ||
      tensor.value("dtype", "") != "<i4" || !tensor.contains("data") ||
      !tensor.at("data").is_string()) {
    throw std::runtime_error(std::string(name) +
                             " must be a little-endian int32 tensor");
  }
  const std::vector<std::uint8_t> bytes =
      decode_base64(tensor.at("data").get_ref<const std::string &>());
  if (bytes.size() % 4 != 0) {
    throw std::runtime_error(std::string(name) + " byte count is invalid");
  }
  std::vector<std::int32_t> values(bytes.size() / 4);
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::uint32_t value =
        static_cast<std::uint32_t>(bytes[index * 4]) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 3]) << 24U);
    values[index] = std::bit_cast<std::int32_t>(value);
  }
  return values;
}

std::vector<double> decode_f32(const json &tensor, const char *name) {
  if (tensor.value("encoding", "") != "base64-little-endian-v1" ||
      tensor.value("dtype", "") != "<f4" || !tensor.contains("data") ||
      !tensor.at("data").is_string()) {
    throw std::runtime_error(std::string(name) +
                             " must be a little-endian float32 tensor");
  }
  const std::vector<std::uint8_t> bytes =
      decode_base64(tensor.at("data").get_ref<const std::string &>());
  if (bytes.size() % 4 != 0) {
    throw std::runtime_error(std::string(name) + " byte count is invalid");
  }
  std::vector<double> values(bytes.size() / 4);
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::uint32_t bits =
        static_cast<std::uint32_t>(bytes[index * 4]) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[index * 4 + 3]) << 24U);
    const float value = std::bit_cast<float>(bits);
    if (!std::isfinite(value)) {
      throw std::runtime_error(std::string(name) +
                               " contains a non-finite value");
    }
    values.at(index) = value;
  }
  return values;
}

void append_numeric_values(const json &value, std::vector<double> &result,
                           const char *name) {
  if (value.is_number()) {
    const double number = value.get<double>();
    if (!std::isfinite(number)) {
      throw std::runtime_error(std::string(name) +
                               " contains a non-finite value");
    }
    result.push_back(number);
    return;
  }
  if (value.is_array()) {
    for (const json &entry : value) {
      append_numeric_values(entry, result, name);
    }
    return;
  }
  throw std::runtime_error(std::string(name) + " must contain numeric values");
}

std::vector<double> pose_values(const json &value, const char *name) {
  if (value.is_object() &&
      value.value("encoding", "") == "base64-little-endian-v1") {
    return decode_f32(value, name);
  }
  std::vector<double> result;
  append_numeric_values(value, result, name);
  return result;
}

std::array<double, 3> quaternion_log(const std::array<double, 4> &input) {
  double norm = 0.0;
  for (const double value : input) {
    norm += value * value;
  }
  if (!(norm > 0.0)) {
    throw std::runtime_error("impact pose contains a zero quaternion");
  }
  norm = std::sqrt(norm);
  std::array<double, 4> quaternion{};
  for (std::size_t index = 0; index < quaternion.size(); ++index) {
    quaternion.at(index) = input.at(index) / norm;
  }
  if (quaternion.at(0) < 0.0) {
    for (double &value : quaternion) {
      value = -value;
    }
  }
  const double vector_norm = std::sqrt(quaternion.at(1) * quaternion.at(1) +
                                       quaternion.at(2) * quaternion.at(2) +
                                       quaternion.at(3) * quaternion.at(3));
  if (vector_norm < 1.0e-15) {
    return {2.0 * quaternion.at(1), 2.0 * quaternion.at(2),
            2.0 * quaternion.at(3)};
  }
  const double angle = 2.0 * std::atan2(vector_norm, quaternion.at(0));
  return {angle * quaternion.at(1) / vector_norm,
          angle * quaternion.at(2) / vector_norm,
          angle * quaternion.at(3) / vector_norm};
}

std::optional<std::array<double, 6>> pose_impact(const json &payload) {
  if (payload.contains("impact_description")) {
    const json &description = payload.at("impact_description");
    if (description.contains("pose_update_delta")) {
      const std::vector<double> values = pose_values(
          description.at("pose_update_delta"), "impact pose_update_delta");
      if (values.size() != 6) {
        throw std::runtime_error(
            "impact pose_update_delta must contain six values");
      }
      std::array<double, 6> result{};
      std::copy(values.begin(), values.end(), result.begin());
      return result;
    }
  }
  if (!payload.contains("baseline_tracking") ||
      !payload.contains("withdrawn_tracking")) {
    return std::nullopt;
  }
  const json &baseline = payload.at("baseline_tracking");
  const json &withdrawn = payload.at("withdrawn_tracking");
  if (!baseline.contains("committed_pose") ||
      !withdrawn.contains("committed_pose")) {
    return std::nullopt;
  }
  const json &baseline_pose = baseline.at("committed_pose");
  const json &withdrawn_pose = withdrawn.at("committed_pose");
  std::array<double, 6> result{};
  if (baseline_pose.contains("quaternion") &&
      baseline_pose.contains("translation") &&
      withdrawn_pose.contains("quaternion") &&
      withdrawn_pose.contains("translation")) {
    const std::vector<double> baseline_quaternion =
        pose_values(baseline_pose.at("quaternion"), "baseline quaternion");
    const std::vector<double> withdrawn_quaternion =
        pose_values(withdrawn_pose.at("quaternion"), "withdrawn quaternion");
    const std::vector<double> baseline_translation =
        pose_values(baseline_pose.at("translation"), "baseline translation");
    const std::vector<double> withdrawn_translation =
        pose_values(withdrawn_pose.at("translation"), "withdrawn translation");
    if (baseline_quaternion.size() != 4 || withdrawn_quaternion.size() != 4 ||
        baseline_translation.size() != 3 || withdrawn_translation.size() != 3) {
      throw std::runtime_error("impact committed poses have invalid shapes");
    }
    for (std::size_t component = 0; component < 3; ++component) {
      result.at(component) = withdrawn_translation.at(component) -
                             baseline_translation.at(component);
    }
    const std::array<double, 4> baseline_q{
        baseline_quaternion.at(0), baseline_quaternion.at(1),
        baseline_quaternion.at(2), baseline_quaternion.at(3)};
    const std::array<double, 4> withdrawn_q{
        withdrawn_quaternion.at(0), withdrawn_quaternion.at(1),
        withdrawn_quaternion.at(2), withdrawn_quaternion.at(3)};
    const std::array<double, 4> relative{
        withdrawn_q.at(0) * baseline_q.at(0) +
            withdrawn_q.at(1) * baseline_q.at(1) +
            withdrawn_q.at(2) * baseline_q.at(2) +
            withdrawn_q.at(3) * baseline_q.at(3),
        -withdrawn_q.at(0) * baseline_q.at(1) +
            withdrawn_q.at(1) * baseline_q.at(0) -
            withdrawn_q.at(2) * baseline_q.at(3) +
            withdrawn_q.at(3) * baseline_q.at(2),
        -withdrawn_q.at(0) * baseline_q.at(2) +
            withdrawn_q.at(1) * baseline_q.at(3) +
            withdrawn_q.at(2) * baseline_q.at(0) -
            withdrawn_q.at(3) * baseline_q.at(1),
        -withdrawn_q.at(0) * baseline_q.at(3) -
            withdrawn_q.at(1) * baseline_q.at(2) +
            withdrawn_q.at(2) * baseline_q.at(1) +
            withdrawn_q.at(3) * baseline_q.at(0)};
    const std::array<double, 3> rotation = quaternion_log(relative);
    for (std::size_t component = 0; component < 3; ++component) {
      result.at(component + 3) = rotation.at(component);
    }
    return result;
  }
  return std::nullopt;
}

json decode_pose(const json &payload) {
  const json &pose = payload.at("committed_pose");
  const std::string parameterization =
      payload.value("parameterization", "unknown");
  if (pose.contains("quaternion") && pose.contains("translation")) {
    const std::vector<double> quaternion =
        decode_f32(pose.at("quaternion"), "pose quaternion");
    const std::vector<double> translation =
        decode_f32(pose.at("translation"), "pose translation");
    if (quaternion.size() != 4 || translation.size() != 3) {
      throw std::runtime_error("committed pose has an invalid vector shape");
    }
    return {{"parameterization", parameterization},
            {"quaternion", quaternion},
            {"translation", translation}};
  }
  if (pose.contains("camera_to_world")) {
    const std::vector<double> values =
        decode_f32(pose.at("camera_to_world"), "camera_to_world pose");
    if (values.size() != 16) {
      throw std::runtime_error("camera_to_world pose must contain 16 values");
    }
    json rows = json::array();
    for (std::size_t row = 0; row < 4; ++row) {
      rows.push_back({values.at(row * 4), values.at(row * 4 + 1),
                      values.at(row * 4 + 2), values.at(row * 4 + 3)});
    }
    return {{"parameterization", "camera_to_world_matrix_4x4"},
            {"matrix", std::move(rows)}};
  }
  throw std::runtime_error("committed pose has an unsupported representation");
}

std::uint64_t shape_element(const json &tensor, std::size_t index,
                            const char *name) {
  if (!tensor.contains("shape") || !tensor.at("shape").is_array() ||
      tensor.at("shape").size() <= index) {
    throw std::runtime_error(std::string(name) + " has no required shape");
  }
  return unsigned_value(tensor.at("shape").at(index), name);
}

CaptureFacts load_capture(const std::filesystem::path &path) {
  CaptureFacts facts;
  std::optional<std::uint64_t> final_pose_sequence;
  std::unordered_map<std::uint64_t, json> pose_records;
  for_each_line(path, [&](std::string_view line) {
    ++facts.record_count;
    if (!record_is(line, "header") && !record_is(line, "impact") &&
        !record_is(line, "streaming_schedule") && !record_is(line, "pose") &&
        !record_is(line, "accuracy")) {
      return;
    }
    const json record = json::parse(line);
    const std::string type = record.at("record_type").get<std::string>();
    if (type == "header") {
      facts.workload = record.at("workload").get<std::string>();
      return;
    }
    const json &payload = record.at("payload");
    if (type == "pose") {
      pose_records.emplace(
          unsigned_value(record.at("sequence"), "pose sequence"), payload);
      return;
    }
    if (type == "accuracy") {
      final_pose_sequence = unsigned_value(
          payload.at("streaming_commit").at("final_native_pose_sequence"),
          "final native pose sequence");
      return;
    }
    if (type == "streaming_schedule") {
      facts.candidate_pose_accepted =
          payload.at("candidate_pose").value("accepted", false);
      for (const json &block : payload.at("blocks")) {
        const json &coordinates = block.at("coordinates");
        const std::uint64_t rasterizer_sequence = unsigned_value(
            block.at("rasterizer_sequence"), "streaming rasterizer sequence");
        if (facts.tracking.rasterizer_sequence == 0) {
          facts.tracking.rasterizer_sequence = rasterizer_sequence;
        } else if (facts.tracking.rasterizer_sequence != rasterizer_sequence) {
          throw std::runtime_error(
              "streaming blocks refer to different rasterizer worksets");
        }
        facts.streaming_blocks.push_back(
            {static_cast<std::uint32_t>(
                 unsigned_value(block.at("block_slot"), "block_slot")),
             static_cast<std::uint32_t>(unsigned_value(
                 block.at("completion_order"), "completion_order")),
             static_cast<std::uint32_t>(
                 unsigned_value(coordinates.at("x"), "block x")),
             static_cast<std::uint32_t>(
                 unsigned_value(coordinates.at("y"), "block y")),
             static_cast<std::uint32_t>(
                 unsigned_value(coordinates.at("width"), "block width")),
             static_cast<std::uint32_t>(
                 unsigned_value(coordinates.at("height"), "block height")),
             unsigned_value(block.at("valid_pixels"), "valid_pixels"),
             block.at("gradient_norm").get<double>(), rasterizer_sequence,
             unsigned_value(block.at("loss_mask_sequence"),
                            "loss context sequence"),
             block.value("pose_observability_valid", false)});
      }
      return;
    }

    ImpactObservation observation;
    observation.sequence =
        unsigned_value(record.at("sequence"), "impact sequence");
    observation.frame = unsigned_value(payload.at("frame"), "impact frame");
    observation.iteration =
        unsigned_value(payload.at("iteration"), "impact iteration");
    observation.consumer_group =
        unsigned_value(payload.at("consumer_group"), "consumer_group");
    observation.consumer_generation = unsigned_value(
        payload.at("consumer_group_generation"), "consumer_group_generation");
    observation.label = payload.at("offline_label").get<std::string>();
    observation.verification_valid =
        payload.at("local_verification").value("outcome", false);
    observation.contribution = pose_impact(payload);
    if (observation.contribution.has_value()) {
      observation.contribution_source = payload.contains("impact_description")
                                            ? "captured_pose_update_delta"
                                            : "withdrawal_pose_pair";
    }
    const json &withdrawal = payload.at("withdrawal");
    observation.source_frame =
        unsigned_value(withdrawal.at("source_frame"), "source_frame");
    observation.source_iteration =
        unsigned_value(withdrawal.at("source_iteration"), "source_iteration");
    observation.native_tile_index = unsigned_value(
        payload.at("consumer_group_binding").at("native_tile_index"),
        "native_tile_index");
    observation.stable_ids =
        payload.at("stable_gaussian_ids").get<std::vector<std::string>>();
    facts.impacts.push_back(std::move(observation));
  });

  std::stable_sort(
      facts.impacts.begin(), facts.impacts.end(),
      [](const ImpactObservation &left, const ImpactObservation &right) {
        return std::tie(left.frame, left.iteration, left.sequence) <
               std::tie(right.frame, right.iteration, right.sequence);
      });

  if (!final_pose_sequence.has_value() ||
      !pose_records.contains(*final_pose_sequence)) {
    throw std::runtime_error("native capture lacks its final committed pose");
  }
  facts.committed_pose = decode_pose(pose_records.at(*final_pose_sequence));

  if (facts.workload.empty() || facts.streaming_blocks.empty() ||
      facts.impacts.empty() || !facts.candidate_pose_accepted) {
    throw std::runtime_error(
        "native capture lacks workload, accepted schedule, or impact evidence");
  }

  const ImpactObservation &target = facts.impacts.back();
  json selected;
  json tracking_selected;
  std::uint64_t selected_iteration = 0;
  std::uint64_t selected_sequence = 0;
  for_each_line(path, [&](std::string_view line) {
    if (!record_is(line, "rasterizer")) {
      return;
    }
    const json record = json::parse(line);
    const std::uint64_t sequence =
        unsigned_value(record.at("sequence"), "rasterizer sequence");
    if (sequence == facts.tracking.rasterizer_sequence) {
      tracking_selected = record;
    }
    const json &context = record.at("context");
    const std::string stage = context.value("stage", "");
    if (!stage.starts_with("mapping_render") ||
        !context.contains("stable_gaussian_ids") ||
        !context.at("stable_gaussian_ids").is_array() ||
        context.at("stable_gaussian_ids").empty() ||
        unsigned_value(context.at("frame"), "mapping frame") !=
            target.source_frame) {
      return;
    }
    const std::uint64_t iteration =
        unsigned_value(context.at("iteration"), "mapping iteration");
    if (iteration <= target.source_iteration &&
        (selected.empty() || iteration > selected_iteration ||
         (iteration == selected_iteration && sequence > selected_sequence))) {
      selected = record;
      selected_iteration = iteration;
      selected_sequence = sequence;
    }
  });
  if (selected.empty()) {
    throw std::runtime_error(
        "native capture has no stable-ID Mapping rasterizer for impact input");
  }
  if (tracking_selected.empty()) {
    throw std::runtime_error(
        "native capture lacks the scheduled Tracking rasterizer");
  }

  const json &ranges_tensor = selected.at("tile_ranges");
  const json &indices_tensor = selected.at("point_list_indices");
  const std::vector<std::int32_t> ranges =
      decode_i32(ranges_tensor, "tile_ranges");
  const std::vector<std::int32_t> point_indices =
      decode_i32(indices_tensor, "point_list_indices");
  const std::uint64_t tile_count =
      shape_element(ranges_tensor, 0, "tile_ranges");
  if (shape_element(ranges_tensor, 1, "tile_ranges") != 2 ||
      ranges.size() != tile_count * 2 ||
      point_indices.size() !=
          shape_element(indices_tensor, 0, "point_list_indices")) {
    throw std::runtime_error("native rasterizer tensor shape mismatch");
  }
  const std::uint64_t image_width =
      shape_element(selected.at("ray_operations"), 1, "ray_operations");
  facts.mapping.columns = static_cast<std::uint32_t>((image_width + 15) / 16);
  if (facts.mapping.columns == 0 || tile_count % facts.mapping.columns != 0) {
    throw std::runtime_error("native rasterizer tile grid is invalid");
  }

  const json &stable_ids = selected.at("context").at("stable_gaussian_ids");
  facts.mapping.relation_counts.resize(tile_count);
  facts.mapping.gaussians_by_tile.resize(tile_count);
  facts.mapping.source_tiles_by_gaussian.resize(stable_ids.size());
  for (std::uint64_t tile = 0; tile < tile_count; ++tile) {
    const std::int32_t begin = ranges.at(tile * 2);
    const std::int32_t end = ranges.at(tile * 2 + 1);
    if (begin < 0 || end < begin ||
        static_cast<std::size_t>(end) > point_indices.size()) {
      throw std::runtime_error("native rasterizer tile range is invalid");
    }
    facts.mapping.relation_counts.at(tile) =
        static_cast<std::uint64_t>(end - begin);
    auto &tile_gaussians = facts.mapping.gaussians_by_tile.at(tile);
    tile_gaussians.reserve(static_cast<std::size_t>(end - begin));
    std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();
    for (std::int32_t offset = begin; offset < end; ++offset) {
      const std::int32_t raw_index = point_indices.at(offset);
      if (raw_index < 0 ||
          static_cast<std::size_t>(raw_index) >= stable_ids.size()) {
        throw std::runtime_error("native rasterizer point index is invalid");
      }
      const auto gaussian = static_cast<std::uint32_t>(raw_index);
      if (gaussian == previous) {
        continue;
      }
      previous = gaussian;
      tile_gaussians.push_back(gaussian);
      auto &sources = facts.mapping.source_tiles_by_gaussian.at(gaussian);
      if (sources.empty() || sources.back() != tile) {
        sources.push_back(static_cast<std::uint32_t>(tile));
      }
    }
  }

  std::unordered_set<std::string> selected_ids;
  for (const ImpactObservation &impact : facts.impacts) {
    selected_ids.insert(impact.stable_ids.begin(), impact.stable_ids.end());
  }
  for (std::size_t index = 0; index < stable_ids.size(); ++index) {
    const std::string &stable_id =
        stable_ids.at(index).get_ref<const std::string &>();
    if (selected_ids.contains(stable_id)) {
      facts.mapping.selected_stable_indices.emplace(
          stable_id, static_cast<std::uint32_t>(index));
    }
  }

  const json &tracking_ranges_tensor = tracking_selected.at("tile_ranges");
  const json &tracking_indices_tensor =
      tracking_selected.at("point_list_indices");
  const std::vector<std::int32_t> tracking_ranges =
      decode_i32(tracking_ranges_tensor, "Tracking tile_ranges");
  const std::vector<std::int32_t> tracking_indices =
      decode_i32(tracking_indices_tensor, "Tracking point_list_indices");
  const std::uint64_t tracking_tile_count =
      shape_element(tracking_ranges_tensor, 0, "Tracking tile_ranges");
  if (shape_element(tracking_ranges_tensor, 1, "Tracking tile_ranges") != 2 ||
      tracking_ranges.size() != tracking_tile_count * 2 ||
      tracking_indices.size() != shape_element(tracking_indices_tensor, 0,
                                               "Tracking point_list_indices")) {
    throw std::runtime_error("Tracking rasterizer tensor shape mismatch");
  }
  const std::uint64_t tracking_width = shape_element(
      tracking_selected.at("ray_operations"), 1, "Tracking ray_operations");
  facts.tracking.columns =
      static_cast<std::uint32_t>((tracking_width + 15) / 16);
  if (facts.tracking.columns == 0 ||
      tracking_tile_count % facts.tracking.columns != 0) {
    throw std::runtime_error("Tracking rasterizer tile grid is invalid");
  }
  const std::uint64_t tracking_gaussians =
      shape_element(tracking_selected.at("radii"), 0, "Tracking radii");
  const std::string tracking_snapshot =
      tracking_selected.at("context").value("stable_gaussian_id_snapshot", "");
  const std::string mapping_snapshot =
      selected.at("context").value("stable_gaussian_id_snapshot", "");
  if (tracking_snapshot.empty() || mapping_snapshot.empty() ||
      tracking_snapshot != mapping_snapshot) {
    throw std::runtime_error(
        "Tracking rasterizer does not share the selected stable-ID snapshot");
  }
  facts.tracking.depth_ordered_gaussians_by_tile.resize(tracking_tile_count);
  for (std::uint64_t tile = 0; tile < tracking_tile_count; ++tile) {
    const std::int32_t begin = tracking_ranges.at(tile * 2);
    const std::int32_t end = tracking_ranges.at(tile * 2 + 1);
    if (begin < 0 || end < begin ||
        static_cast<std::size_t>(end) > tracking_indices.size()) {
      throw std::runtime_error("Tracking rasterizer tile range is invalid");
    }
    auto &workset = facts.tracking.depth_ordered_gaussians_by_tile.at(tile);
    workset.reserve(static_cast<std::size_t>(end - begin));
    for (std::int32_t offset = begin; offset < end; ++offset) {
      const std::int32_t gaussian = tracking_indices.at(offset);
      if (gaussian < 0 ||
          static_cast<std::uint64_t>(gaussian) >= tracking_gaussians) {
        throw std::runtime_error("Tracking point-list index is invalid");
      }
      workset.push_back(static_cast<std::uint32_t>(gaussian));
    }
  }
  return facts;
}

} // namespace

WorkloadInput load_workload(const std::filesystem::path &manifest_path) {
  require_file(manifest_path, "workload manifest");
  const json manifest_document = read_json(manifest_path);
  if (manifest_document.value("format", "") != "cisda-workload-manifest/v1") {
    throw std::runtime_error(
        "manifest format must be cisda-workload-manifest/v1");
  }
  const std::filesystem::path base = manifest_path.parent_path();
  WorkloadManifest manifest{
      manifest_document.at("name").get<std::string>(),
      manifest_document.at("workload").get<std::string>(),
      resolve_path(base, manifest_document, "compiled_workload"),
      resolve_path(base, manifest_document, "native_capture"),
      resolve_path(base, manifest_document, "model_root"),
      resolve_path(base, manifest_document, "dataset_root")};
  require_file(manifest.bundle_path, "compiled workload");
  require_file(manifest.capture_path, "native capture");
  require_directory(manifest.model_root, "model root");
  require_directory(manifest.dataset_root, "dataset root");

  json bundle = read_json(manifest.bundle_path);
  const std::string bundle_format = bundle.value("format", "");
  if (!bundle_format.ends_with("native-workload-bundle/v1")) {
    throw std::runtime_error("compiled workload has an unsupported format");
  }
  if (bundle.value("workload", "") != manifest.workload) {
    throw std::runtime_error("compiled workload identity mismatches manifest");
  }
  CaptureFacts capture = load_capture(manifest.capture_path);
  if (capture.workload != manifest.workload) {
    throw std::runtime_error("native capture identity mismatches manifest");
  }
  return {std::move(manifest), std::move(bundle), std::move(capture)};
}

} // namespace cisda
