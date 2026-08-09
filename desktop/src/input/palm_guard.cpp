#include "pdb/input/palm_guard.h"

namespace pdb::input {
PalmGuard::PalmGuard(std::chrono::milliseconds releaseDelay) : releaseDelay_(releaseDelay) {}

void PalmGuard::ObservePen(bool inRange, std::chrono::steady_clock::time_point now) noexcept {
  if (penInRange_ && !inRange) lastPenExit_ = now;
  penInRange_ = inRange;
  seenPen_ = true;
}

bool PalmGuard::AllowsTouch(std::chrono::steady_clock::time_point now) const noexcept {
  return !penInRange_ && (!seenPen_ || now - lastPenExit_ >= releaseDelay_);
}

void PalmGuard::Reset() noexcept {
  penInRange_ = false;
  seenPen_ = false;
  lastPenExit_ = {};
}
}  // namespace pdb::input
