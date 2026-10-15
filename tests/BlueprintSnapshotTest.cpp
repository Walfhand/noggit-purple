#include <noggit/ai/BlueprintSnapshot.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main()
{
  namespace fs = std::filesystem;
  auto const root = fs::temp_directory_path()
    / ("noggit-blueprint-snapshot-" + std::to_string(
         std::chrono::steady_clock::now().time_since_epoch().count()));
  auto const map = root / "map";
  auto const snapshot = root / "snapshots" / "map";
  fs::create_directories(map / "nested");
  std::ofstream(map / "nested" / "tile.adt") << "baseline";

  if (Noggit::Ai::captureBlueprintSnapshot(map, snapshot)) return 1;
  std::ofstream(map / "nested" / "tile.adt") << "generated";
  std::ofstream(map / "extra.adt") << "extra";
  if (Noggit::Ai::restoreBlueprintSnapshot(snapshot, map)) return 2;

  std::ifstream restored(map / "nested" / "tile.adt");
  std::string value;
  restored >> value;
  if (value != "baseline") return 3;
  if (fs::exists(map / "extra.adt")) return 4;
  if (!Noggit::Ai::restoreBlueprintSnapshot(root / "missing", map)) return 5;
  fs::remove_all(root);
}
