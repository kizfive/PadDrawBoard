#include "pdb/input/mapper.h"
#include "pdb/input/palm_guard.h"

#include <cassert>
#include <chrono>

namespace pdb::input::tests {
void RunCoordinateMapperTests() {
  CoordinateMapper mapper;
  MapperConfig config;
  config.tabletActiveRect = {0.1F, 0.2F, 0.9F, 0.8F};
  config.monitor = {100, 200, 2000, 1000};
  config.contentAspectRatio = 1.0F;
  config.orientationEpoch = 7;
  assert(mapper.Configure(config));
  assert((mapper.ContentViewport() == PixelRect{600, 200, 1000, 1000}));
  assert((mapper.Map({0.1F, 0.2F}, 7) == MappedPoint{600, 200}));
  assert((mapper.Map({0.9F, 0.8F}, 7) == MappedPoint{1599, 1199}));
  assert(!mapper.Map({0.05F, 0.2F}, 7).has_value());
  assert(!mapper.Map({0.1F, 0.2F}, 8).has_value());

  config.rotation = Rotation::k90;
  assert(mapper.Configure(config));
  assert((mapper.Map({0.1F, 0.2F}, 7) == MappedPoint{1599, 200}));
}

void RunPalmGuardTests() {
  using namespace std::chrono_literals;
  PalmGuard guard{150ms};
  const auto start = std::chrono::steady_clock::time_point{};
  assert(guard.AllowsTouch(start));
  guard.ObservePen(true, start);
  assert(!guard.AllowsTouch(start));
  guard.ObservePen(false, start + 10ms);
  assert(!guard.AllowsTouch(start + 159ms));
  assert(guard.AllowsTouch(start + 160ms));
}

}  // namespace pdb::input::tests

#ifdef PDB_INPUT_TEST_MAIN
int main() {
  pdb::input::tests::RunCoordinateMapperTests();
  pdb::input::tests::RunPalmGuardTests();
}
#endif
