// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }
}

int main()
{
  auto specification = nlohmann::json::parse(R"({
    "seed":"jungle-v1",
    "assets":[
      {"path":"world/tree.m2","role":"canopy","weight":3,"min_scale":0.8,"max_scale":1.2,"spacing_multiplier":1.2},
      {"path":"world/tree2.m2","role":"canopy","weight":2,"min_scale":0.7,"max_scale":1.1,"spacing_multiplier":1.0},
      {"path":"world/rock.wmo","role":"rock","weight":1,"min_scale":0.9,"max_scale":1.1,"spacing_multiplier":1.0}
    ],
    "regions":[{
      "name":"jungle","role":"canopy","points":[{"u":0.1,"v":0.1},{"u":0.9,"v":0.1},{"u":0.9,"v":0.9},{"u":0.1,"v":0.9}],
      "density_per_tile":64,"min_spacing_ratio":0.02,
      "min_height":-100,"max_height":100,"min_slope_degrees":0,"max_slope_degrees":35,
      "cluster_scale":3,"cluster_strength":0.75
    }],
    "exclusions":[{
      "shape":"corridor","points":[{"u":0.5,"v":0.0},{"u":0.5,"v":1.0}],"half_width_ratio":0.05
    }]
  })");
  auto parsed = Noggit::Ai::parseProceduralScatter(specification);
  require(parsed.scatter.has_value(), "valid scatter rejected");
  auto const first = Noggit::Ai::proceduralScatterCandidate(
    *parsed.scatter, 0, 26, 26, 4, 0.0f, 0.5f, 0.0f, 1.0f);
  auto const repeated = Noggit::Ai::proceduralScatterCandidate(
    *parsed.scatter, 0, 26, 26, 4, 0.0f, 0.5f, 0.0f, 1.0f);
  require(first.u == repeated.u && first.v == repeated.v
          && first.asset_index == repeated.asset_index
          && first.scale == repeated.scale && first.yaw_degrees == repeated.yaw_degrees
          && first.active == repeated.active,
          "scatter must be deterministic");
  require(first.u >= 0.0f && first.u <= 0.5f && first.v >= 0.0f && first.v <= 1.0f,
          "candidate escaped its tile");
  bool first_species = false;
  bool second_species = false;
  bool clearing = false;
  for (std::size_t index = 0; index < 512; ++index)
  {
    auto const candidate = Noggit::Ai::proceduralScatterCandidate(
      *parsed.scatter, 0, 26, 26, index, 0.0f, 1.0f, 0.0f, 1.0f);
    first_species |= candidate.asset_index == 0;
    second_species |= candidate.asset_index == 1;
    clearing |= !candidate.active;
    require(candidate.asset_index != 2, "region selected an asset from another role");
  }
  require(first_species && second_species && clearing,
          "species patches or low-frequency clearings disappeared");
  require(Noggit::Ai::proceduralScatterContains(parsed.scatter->regions[0].points, 0.2f, 0.2f)
          && !Noggit::Ai::proceduralScatterContains(parsed.scatter->regions[0].points, 0.05f, 0.2f),
          "polygon containment changed");
  require(Noggit::Ai::proceduralScatterRegionIntersects(
            parsed.scatter->regions[0], 0.0f, 0.5f, 0.0f, 0.5f)
          && !Noggit::Ai::proceduralScatterRegionIntersects(
            parsed.scatter->regions[0], 0.91f, 1.0f, 0.0f, 0.09f),
          "region/tile preflight intersection changed");
  require(Noggit::Ai::proceduralScatterExcluded(*parsed.scatter, 0.5f, 0.5f, 1000.0f, 500.0f)
          && !Noggit::Ai::proceduralScatterExcluded(*parsed.scatter, 0.1f, 0.5f, 1000.0f, 500.0f),
          "corridor exclusion changed");

  specification["regions"][0]["extra"] = true;
  require(!Noggit::Ai::parseProceduralScatter(specification).scatter,
          "strict nested validation accepted an extra field");
  specification["regions"][0].erase("extra");
  specification["assets"][0]["weight"] = std::numeric_limits<double>::infinity();
  require(!Noggit::Ai::parseProceduralScatter(specification).scatter,
          "non-finite weight accepted");
}
