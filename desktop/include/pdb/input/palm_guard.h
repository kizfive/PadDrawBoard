#pragma once

#include "pdb/input/domain.h"

#include <chrono>

namespace pdb::input {

// Suppresses fingers while the pen is in range, and for a short tail after it leaves.
class PalmGuard final {
 public:
  explicit PalmGuard(std::chrono::milliseconds releaseDelay = std::chrono::milliseconds{150});

  void ObservePen(bool inRange, std::chrono::steady_clock::time_point now) noexcept;
  [[nodiscard]] bool AllowsTouch(std::chrono::steady_clock::time_point now) const noexcept;
  void Reset() noexcept;

 private:
  std::chrono::milliseconds releaseDelay_;
  std::chrono::steady_clock::time_point lastPenExit_{};
  bool penInRange_{};
  bool seenPen_{};
};

}  // namespace pdb::input
