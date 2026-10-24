#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace cisda {

struct ReusePrediction {
  std::uint16_t index{};
  bool history_valid{};
  bool reuse{};
  std::uint64_t gaussian_id{};
  std::uint64_t target_version{};
  std::uint64_t context{};
  std::vector<std::uint32_t> tiles;
};

class ReusePredictor {
public:
  ReusePrediction query(std::uint64_t gaussian_id, std::uint64_t target_version,
                        std::uint64_t context, bool resident,
                        bool version_match, std::vector<std::uint32_t> tiles);
  void feedback(const ReusePrediction &prediction, bool kept_visible);

  std::uint64_t lookups() const;
  std::uint64_t predictions() const;
  std::uint64_t feedback_updates() const;
  std::uint64_t merged_requests() const;

private:
  static std::uint16_t fold(std::uint64_t gaussian_id,
                            std::uint8_t history_bits);

  std::array<std::uint8_t, 2048> counters_{};
  std::unordered_map<std::uint64_t, std::deque<bool>> histories_;
  std::uint64_t lookups_{};
  std::uint64_t predictions_{};
  std::uint64_t feedback_updates_{};
  std::uint64_t merged_requests_{};
};

std::uint64_t numeric_gaussian_id(const std::string &stable_id);

} // namespace cisda
