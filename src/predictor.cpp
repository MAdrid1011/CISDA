#include "cisda/predictor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cisda {

std::uint64_t numeric_gaussian_id(const std::string &stable_id) {
  const std::size_t separator = stable_id.rfind(':');
  if (separator == std::string::npos || separator + 1 == stable_id.size()) {
    throw std::runtime_error("invalid stable Gaussian ID " + stable_id);
  }
  std::uint64_t value = 0;
  for (std::size_t index = separator + 1; index < stable_id.size(); ++index) {
    const unsigned char character =
        static_cast<unsigned char>(stable_id.at(index));
    if (std::isdigit(character) == 0) {
      throw std::runtime_error("invalid stable Gaussian ID " + stable_id);
    }
    value = value * 10 + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

std::uint16_t ReusePredictor::fold(std::uint64_t gaussian_id,
                                   std::uint8_t history_bits) {
  std::uint64_t folded = 0;
  while (gaussian_id != 0) {
    folded ^= gaussian_id & 0x7ffU;
    gaussian_id >>= 11U;
  }
  folded = (folded & ~0xfULL) | ((folded ^ history_bits) & 0xfULL);
  return static_cast<std::uint16_t>(folded & 0x7ffU);
}

ReusePrediction ReusePredictor::query(std::uint64_t gaussian_id,
                                      std::uint64_t target_version,
                                      std::uint64_t context, bool resident,
                                      bool version_match,
                                      std::vector<std::uint32_t> tiles) {
  ++lookups_;
  auto &history = histories_[gaussian_id];
  std::uint8_t bits = 0;
  for (const bool value : history) {
    bits = static_cast<std::uint8_t>((bits << 1U) | (value ? 1U : 0U));
  }
  const std::uint16_t index = fold(gaussian_id, bits);
  const bool history_valid = history.size() == 4;
  const bool reuse =
      history_valid && counters_.at(index) >= 2 && resident && version_match;
  predictions_ += reuse ? 1U : 0U;
  if (tiles.size() > 1) {
    merged_requests_ += tiles.size() - 1;
  }
  return {index,          history_valid, reuse,           gaussian_id,
          target_version, context,       std::move(tiles)};
}

void ReusePredictor::feedback(const ReusePrediction &prediction,
                              bool kept_visible) {
  std::uint8_t &counter = counters_.at(prediction.index);
  if (kept_visible) {
    counter = std::min<std::uint8_t>(3, counter + 1);
  } else {
    counter = counter == 0 ? 0 : static_cast<std::uint8_t>(counter - 1);
  }
  auto &history = histories_[prediction.gaussian_id];
  if (history.size() == 4) {
    history.pop_front();
  }
  history.push_back(kept_visible);
  ++feedback_updates_;
}

std::uint64_t ReusePredictor::lookups() const { return lookups_; }
std::uint64_t ReusePredictor::predictions() const { return predictions_; }
std::uint64_t ReusePredictor::feedback_updates() const {
  return feedback_updates_;
}
std::uint64_t ReusePredictor::merged_requests() const {
  return merged_requests_;
}

} // namespace cisda
