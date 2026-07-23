// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/MobaArenaAudit.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>

#include <nlohmann/json.hpp>
#include <lodepng.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }

  void requireAudit(Noggit::Ai::MobaArenaAuditReport const& audit)
  {
    if (audit.ok()) return;
    auto const artifacts = std::filesystem::path{NOGGIT_TEST_ARTIFACT_DIR};
    std::filesystem::create_directories(artifacts);
    lodepng::encode((artifacts / "moba-arena-structural.actual.png").string(),
                    audit.preview_rgba, audit.preview_width, audit.preview_height);
    auto message = std::string{"MOBA structural audit failed:"};
    for (auto const& issue : audit.issues)
    {
      message += "\n- " + issue.code;
      if (!issue.subject.empty()) message += " [" + issue.subject + "]";
      message += ": " + issue.message;
    }
    throw std::runtime_error(message);
  }

  void verifyGolden(Noggit::Ai::MobaArenaAuditReport const& audit, bool update)
  {
    namespace fs = std::filesystem;
    auto const golden = fs::path{NOGGIT_MOBA_GOLDEN_PATH};
    if (update)
    {
      fs::create_directories(golden.parent_path());
      auto const error = lodepng::encode(
        golden.string(), audit.preview_rgba, audit.preview_width, audit.preview_height);
      if (error) throw std::runtime_error("cannot write MOBA golden: "
        + std::string{lodepng_error_text(error)});
      return;
    }

    std::vector<unsigned char> expected;
    unsigned width = 0;
    unsigned height = 0;
    auto const decode_error = lodepng::decode(expected, width, height, golden.string());
    if (decode_error)
      throw std::runtime_error("missing or unreadable MOBA golden; run "
        "noggit-moba-arena-blueprint-test --update-golden");
    if (width != audit.preview_width || height != audit.preview_height
        || expected.size() != audit.preview_rgba.size())
      throw std::runtime_error("MOBA golden dimensions changed");
    if (expected == audit.preview_rgba) return;

    std::uint64_t absolute_error = 0;
    std::size_t changed_pixels = 0;
    std::vector<unsigned char> diff(expected.size(), 255);
    for (std::size_t pixel = 0; pixel < width * height; ++pixel)
    {
      auto maximum_delta = 0;
      for (std::size_t channel = 0; channel < 3; ++channel)
      {
        auto const index = pixel * 4 + channel;
        auto const delta = std::abs(static_cast<int>(expected[index])
                                  - static_cast<int>(audit.preview_rgba[index]));
        absolute_error += static_cast<std::uint64_t>(delta);
        maximum_delta = std::max(maximum_delta, delta);
        diff[index] = static_cast<unsigned char>(std::min(255, delta * 8));
      }
      changed_pixels += maximum_delta > 4;
    }
    auto const score = 1.0 - static_cast<double>(absolute_error)
      / static_cast<double>(255ULL * 3ULL * width * height);

    auto const artifacts = fs::path{NOGGIT_TEST_ARTIFACT_DIR};
    fs::create_directories(artifacts);
    lodepng::encode((artifacts / "moba-arena-semantic.actual.png").string(),
                    audit.preview_rgba, width, height);
    lodepng::encode((artifacts / "moba-arena-semantic.diff.png").string(),
                    diff, width, height);
    throw std::runtime_error("MOBA semantic golden changed: score="
      + std::to_string(score) + ", changed_pixels="
      + std::to_string(changed_pixels));
  }

  void verifyReferenceSimilarity(Noggit::Ai::MobaArenaAuditReport const& audit)
  {
    namespace fs = std::filesystem;
    constexpr auto minimum_iou = .80;
    constexpr auto minimum_tolerant_overlap = .95;
    constexpr auto boundary_tolerance_pixels = 4;
    constexpr auto minimum_reference_component_area = std::size_t{80};

    std::vector<unsigned char> pixels;
    unsigned width = 0;
    unsigned height = 0;
    auto const decode_error = lodepng::decode(
      pixels, width, height, NOGGIT_MOBA_REFERENCE_MASK_PATH);
    if (decode_error)
      throw std::runtime_error("missing or unreadable MOBA reference mask: "
        + std::string{lodepng_error_text(decode_error)});
    if (width == 0 || height == 0 || pixels.size() != width * height * 4)
      throw std::runtime_error("MOBA reference mask dimensions are invalid");

    // The checked-in 1-bit mask is the target screenshot thresholded at
    // arithmetic RGB mean < 35. Keep only sizeable, internal 4-connected
    // regions so the bases and screenshot background are not scored as walls.
    std::vector<std::uint8_t> raw(width * height);
    std::vector<std::uint8_t> reference(width * height);
    std::vector<std::uint8_t> visited(width * height);
    for (std::size_t pixel = 0; pixel < raw.size(); ++pixel)
      raw[pixel] = pixels[pixel * 4] >= 128;
    std::vector<std::size_t> component;
    for (std::size_t root = 0; root < raw.size(); ++root)
    {
      if (!raw[root] || visited[root]) continue;
      component.clear();
      component.push_back(root);
      visited[root] = true;
      auto touches_boundary = false;
      for (std::size_t cursor = 0; cursor < component.size(); ++cursor)
      {
        auto const pixel = component[cursor];
        auto const x = pixel % width;
        auto const y = pixel / width;
        touches_boundary |= x == 0 || y == 0 || x + 1 == width || y + 1 == height;
        auto visit = [&](std::size_t neighbor)
        {
          if (!raw[neighbor] || visited[neighbor]) return;
          visited[neighbor] = true;
          component.push_back(neighbor);
        };
        if (x > 0) visit(pixel - 1);
        if (x + 1 < width) visit(pixel + 1);
        if (y > 0) visit(pixel - width);
        if (y + 1 < height) visit(pixel + width);
      }
      if (!touches_boundary && component.size() >= minimum_reference_component_area)
        for (auto const pixel : component) reference[pixel] = true;
    }

    auto const panel_width = audit.preview_width / 2;
    auto const panel_height = audit.preview_height;
    if (panel_width == 0 || panel_height == 0
        || audit.preview_rgba.size() != audit.preview_width * panel_height * 4)
      throw std::runtime_error("MOBA semantic wall panel dimensions are invalid");

    std::uint64_t intersection = 0;
    std::uint64_t union_count = 0;
    std::uint64_t predicted_count = 0;
    std::uint64_t reference_count = 0;
    std::vector<std::uint8_t> predicted_mask(width * height);
    std::vector<unsigned char> overlay(width * height * 4, 255);
    for (std::size_t y = 0; y < height; ++y)
      for (std::size_t x = 0; x < width; ++x)
      {
        auto const reference_pixel = y * width + x;
        auto const panel_x = std::min<std::size_t>(
          panel_width - 1, (2 * x + 1) * panel_width / (2 * width));
        auto const panel_y = std::min<std::size_t>(
          panel_height - 1, (2 * y + 1) * panel_height / (2 * height));
        auto const panel_pixel = (panel_y * audit.preview_width + panel_x) * 4;
        auto const predicted = audit.preview_rgba[panel_pixel] < 30
          && audit.preview_rgba[panel_pixel + 1] < 45
          && audit.preview_rgba[panel_pixel + 2] < 35;
        auto const expected = reference[reference_pixel] != 0;
        predicted_mask[reference_pixel] = predicted;
        intersection += predicted && expected;
        union_count += predicted || expected;
        predicted_count += predicted;
        reference_count += expected;

        auto const overlay_pixel = reference_pixel * 4;
        if (predicted && expected)
          overlay[overlay_pixel] = overlay[overlay_pixel + 1]
            = overlay[overlay_pixel + 2] = 0;
        else if (expected)
        {
          overlay[overlay_pixel] = 220;
          overlay[overlay_pixel + 1] = 50;
          overlay[overlay_pixel + 2] = 47;
        }
        else if (predicted)
        {
          overlay[overlay_pixel] = 52;
          overlay[overlay_pixel + 1] = 120;
          overlay[overlay_pixel + 2] = 246;
        }
      }

    auto const iou = union_count
      ? static_cast<double>(intersection) / static_cast<double>(union_count) : 1.0;
    auto const precision = predicted_count
      ? static_cast<double>(intersection) / static_cast<double>(predicted_count) : 0.0;
    auto const recall = reference_count
      ? static_cast<double>(intersection) / static_cast<double>(reference_count) : 0.0;
    auto const nearby = [&](std::vector<std::uint8_t> const& mask,
                            std::size_t x, std::size_t y)
    {
      for (auto dy = -boundary_tolerance_pixels; dy <= boundary_tolerance_pixels; ++dy)
        for (auto dx = -boundary_tolerance_pixels; dx <= boundary_tolerance_pixels; ++dx)
        {
          if (dx * dx + dy * dy > boundary_tolerance_pixels * boundary_tolerance_pixels)
            continue;
          auto const sample_x = static_cast<int>(x) + dx;
          auto const sample_y = static_cast<int>(y) + dy;
          if (sample_x >= 0 && sample_y >= 0
              && sample_x < static_cast<int>(width)
              && sample_y < static_cast<int>(height)
              && mask[static_cast<std::size_t>(sample_y) * width
                    + static_cast<std::size_t>(sample_x)])
            return true;
        }
      return false;
    };
    auto tolerant_precision_count = std::uint64_t{0};
    auto tolerant_recall_count = std::uint64_t{0};
    for (std::size_t y = 0; y < height; ++y)
      for (std::size_t x = 0; x < width; ++x)
      {
        auto const pixel = y * width + x;
        tolerant_precision_count += predicted_mask[pixel] && nearby(reference, x, y);
        tolerant_recall_count += reference[pixel] && nearby(predicted_mask, x, y);
      }
    auto const tolerant_precision = predicted_count
      ? static_cast<double>(tolerant_precision_count)
          / static_cast<double>(predicted_count) : 0.0;
    auto const tolerant_recall = reference_count
      ? static_cast<double>(tolerant_recall_count)
          / static_cast<double>(reference_count) : 0.0;
    if (iou >= minimum_iou
        && tolerant_precision >= minimum_tolerant_overlap
        && tolerant_recall >= minimum_tolerant_overlap)
      return;

    auto const artifacts = fs::path{NOGGIT_TEST_ARTIFACT_DIR};
    fs::create_directories(artifacts);
    lodepng::encode((artifacts / "moba-arena-reference.overlay.png").string(),
                    overlay, width, height);
    throw std::runtime_error("MOBA wall reference similarity regressed: IoU="
      + std::to_string(iou) + " (minimum=" + std::to_string(minimum_iou)
      + "), precision="
      + std::to_string(precision) + ", recall=" + std::to_string(recall)
      + ", tolerant_precision=" + std::to_string(tolerant_precision)
      + ", tolerant_recall=" + std::to_string(tolerant_recall)
      + "; overlay: black=match, red=missing, blue=extra");
  }

  nlohmann::json& callArguments(nlohmann::json& blueprint,
                                std::string_view name,
                                std::size_t occurrence = 0)
  {
    for (auto& call : blueprint.at("next_calls"))
      if (call.at("name").get_ref<std::string const&>() == name
          && occurrence-- == 0)
        return call.at("arguments");
    throw std::runtime_error("missing blueprint call: " + std::string{name});
  }

  nlohmann::json& terrainFeature(nlohmann::json& blueprint,
                                 std::string_view name)
  {
    auto& features = callArguments(
      blueprint, "apply_terrain_layout_on_map").at("features");
    auto const found = std::find_if(features.begin(), features.end(),
      [&](nlohmann::json const& feature)
      {
        return feature.at("name").get_ref<std::string const&>() == name;
      });
    if (found == features.end())
      throw std::runtime_error("missing terrain feature: " + std::string{name});
    return *found;
  }

  nlohmann::json& placedProp(nlohmann::json& blueprint,
                             std::string_view name)
  {
    auto& props = callArguments(blueprint, "place_props_on_map").at("props");
    auto const found = std::find_if(props.begin(), props.end(),
      [&](nlohmann::json const& prop)
      {
        return prop.at("name").get_ref<std::string const&>() == name;
      });
    if (found == props.end())
      throw std::runtime_error("missing placed prop: " + std::string{name});
    return *found;
  }

  void replaceWallMass(nlohmann::json& blueprint, std::size_t ordinal,
                       nlohmann::json replacement)
  {
    terrainFeature(blueprint,
      "jungle_" + std::to_string(ordinal) + "_wall_mass")
      = std::move(replacement);
  }

  template<typename Mutation>
  void requireMutationIssues(nlohmann::json const& blueprint,
                             std::initializer_list<std::string_view> codes,
                             Mutation mutation)
  {
    auto corrupted = blueprint;
    mutation(corrupted);
    auto const audit = Noggit::Ai::auditMobaArenaBlueprint(corrupted, 2, 64);
    for (auto const code : codes)
    {
      if (audit.hasIssue(code)) continue;
      auto message = "corruption did not report " + std::string{code} + "; got:";
      for (auto const& issue : audit.issues)
        message += " " + issue.code + "(" + issue.message + ")";
      if (audit.metrics.contains("boundary_openings"))
        message += "; boundary_openings="
          + audit.metrics.at("boundary_openings").dump();
      if (audit.metrics.contains("corridor_width_ratio"))
        message += "; corridor_width_ratio="
          + audit.metrics.at("corridor_width_ratio").dump();
      if (audit.metrics.contains("corridor_width_by_route"))
        message += "; corridor_width_by_route="
          + audit.metrics.at("corridor_width_by_route").dump();
      throw std::runtime_error(message);
    }
  }

  nlohmann::json specification()
  {
    return Noggit::Ai::defaultMobaArenaSpecification();
  }
}

int main(int argc, char** argv)
{
  auto const update_golden = argc == 2 && std::string_view{argv[1]} == "--update-golden";
  auto const dump_audit = argc == 2 && std::string_view{argv[1]} == "--dump-audit";
  if (argc > 2 || (argc == 2 && !update_golden && !dump_audit))
    throw std::runtime_error(
      "usage: noggit-moba-arena-blueprint-test [--update-golden|--dump-audit]");
  auto footprint = [](std::size_t width, std::size_t height)
  {
    std::vector<std::pair<std::size_t, std::size_t>> tiles;
    for (std::size_t z = 0; z < height; ++z)
      for (std::size_t x = 0; x < width; ++x) tiles.emplace_back(x + 20, z + 20);
    return tiles;
  };
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(2, 2)),
          "a full 2x2 footprint must be accepted");
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(3, 3)),
          "a full 3x3 footprint must be accepted");
  require(!Noggit::Ai::validateMobaArenaFootprint(footprint(4, 4)),
          "a full square footprint must be accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(5, 5)).has_value(),
          "a footprint larger than the bounded MOBA scatter budget was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(1, 1)).has_value(),
          "a footprint smaller than 2x2 was accepted");
  require(Noggit::Ai::validateMobaArenaFootprint(footprint(4, 3)).has_value(),
          "a stretched footprint was accepted");
  auto incomplete = footprint(3, 3);
  incomplete.pop_back();
  require(Noggit::Ai::validateMobaArenaFootprint(incomplete).has_value(),
          "a footprint with a missing tile was accepted");

  auto const spec = specification();
  require(spec.at("skybox_path")
              == "environments/stars/legionnexus_netherskybox01.m2"
            && spec.at("skybox_flags") == 0,
          "the map's selected skybox is not the MOBA default");
  auto const first = Noggit::Ai::compileMobaArenaBlueprint(spec, 4);
  if (dump_audit)
  {
    auto const audit = Noggit::Ai::auditMobaArenaBlueprint(first, 4);
    requireAudit(audit);
    verifyReferenceSimilarity(audit);
    auto const artifacts = std::filesystem::path{NOGGIT_TEST_ARTIFACT_DIR};
    std::filesystem::create_directories(artifacts);
    auto const write_error = lodepng::encode(
      (artifacts / "moba-arena-semantic.actual.png").string(),
      audit.preview_rgba, audit.preview_width, audit.preview_height);
    if (write_error)
      throw std::runtime_error("cannot write MOBA audit preview");
    std::cout << audit.metrics.dump(2) << '\n';
    return 0;
  }
  auto const second = Noggit::Ai::compileMobaArenaBlueprint(spec, 4);
  require(first == second, "blueprint must be deterministic");
  auto const mutation_baseline = Noggit::Ai::compileMobaArenaBlueprint(spec, 2);
  auto const three_tile_blueprint = Noggit::Ai::compileMobaArenaBlueprint(spec, 3);
  auto const translated_blueprint = Noggit::Ai::compileMobaArenaBlueprint(
    spec, 4, 20, 20);
  require(mutation_baseline.at("audit").at("ok") == true
            && three_tile_blueprint.at("audit").at("ok") == true
            && translated_blueprint.at("audit").at("ok") == true,
          "every supported footprint size and translated tile origin must pass the production audit");
  require(translated_blueprint.at("audit").at("metrics")
              .at("scatter").at("tile_origin").at("min_tile_x") == 20
            && translated_blueprint.at("audit").at("metrics")
              .at("scatter").at("tile_origin").at("min_tile_z") == 20,
          "the production audit ignored the map's absolute tile origin");
  auto const audit = Noggit::Ai::auditMobaArenaBlueprint(first, 4);
  requireAudit(audit);
  verifyReferenceSimilarity(audit);
  require(audit.preview_width == audit.preview_resolution * 2
            && audit.preview_height == audit.preview_resolution
            && audit.preview_rgba.size()
              == audit.preview_width * audit.preview_height * 4,
          "structural audit must emit a deterministic two-panel preview");
  require(audit.metrics.at("preview_overlays").at("liquid_cells")
              == audit.metrics.at("liquid").at("active_mh2o_cells")
            && audit.metrics.at("preview_overlays").at("scatter_candidates")
              .get<std::size_t>() > 0
            && audit.metrics.at("preview_overlays").at("props")
              == audit.metrics.at("props").at("count"),
          "the golden preview must cover liquid, scatter candidates and props");
  require(audit.metrics.at("liquid").at("river_connected") == true
            && audit.metrics.at("liquid").at("connected_mh2o_cells")
              == audit.metrics.at("liquid").at("active_mh2o_cells")
            && audit.metrics.at("liquid").at("maximum_water_column")
                 .get<double>() <= 1.01
            && audit.metrics.at("props").at("anchored")
              == audit.metrics.at("props").at("count"),
          "liquid continuity, per-cell wading depth or prop anchors regressed");

  auto const compact_audit = Noggit::Ai::auditMobaArenaBlueprint(
    mutation_baseline, 2, 64);
  requireAudit(compact_audit);
  auto requirePreviewChange = [&](char const* subject, auto mutation)
  {
    auto corrupted = mutation_baseline;
    mutation(corrupted);
    auto const changed = Noggit::Ai::auditMobaArenaBlueprint(corrupted, 2, 64);
    if (changed.preview_rgba == compact_audit.preview_rgba)
      throw std::runtime_error(std::string{"preview ignored "} + subject);
  };
  requirePreviewChange("liquid mask", [](nlohmann::json& corrupted)
  {
    auto& width = callArguments(corrupted, "apply_liquid_layout_on_map")
      .at("features").front().at("half_width_ratio");
    width = width.get<double>() + .01;
  });
  requirePreviewChange("scatter candidates", [](nlohmann::json& corrupted)
  {
    auto& seed = callArguments(corrupted, "scatter_assets_on_map", 1).at("seed");
    seed = seed.get<std::string>() + "-preview-mutation";
  });
  requirePreviewChange("props", [](nlohmann::json& corrupted)
  {
    auto& prop = placedProp(corrupted, "team_left_glow");
    prop["u"] = prop.at("u").get<double>() + .01;
  });

  auto const& calls = first.at("next_calls");
  require(calls.size() == 9
          && calls[0].at("name") == "validate_moba_footprint"
          && calls[0].at("arguments").empty()
          && calls[1].at("name") == "apply_terrain_layout_on_map"
          && calls[2].at("name") == "apply_liquid_layout_on_map"
          && calls[3].at("name") == "apply_ground_effect_on_map"
          && calls[3].at("arguments").at("texture_path")
            == spec.at("texture_paths").at(0)
          && calls[3].at("arguments").at("effect_id")
            == spec.at("ground_effect_texture_id")
          && calls[4].at("name") == "scatter_assets_on_map"
          && calls[5].at("name") == "place_props_on_map"
          && calls[6].at("name") == "scatter_assets_on_map"
          && calls[7].at("name") == "apply_skybox_on_map"
          && calls[7].at("arguments").at("skybox_path")
            == spec.at("skybox_path")
          && calls[7].at("arguments").at("flags") == spec.at("skybox_flags")
          && calls[7].at("arguments").at("lighting_param_index") == 3
          && calls[8].at("name") == "validate_map"
          && calls[8].at("arguments").empty(),
          "generic execution pipeline changed");
  auto const terrain = Noggit::Ai::parseProceduralLayout(calls[1].at("arguments"));
  auto const liquid = Noggit::Ai::parseProceduralLiquidLayout(calls[2].at("arguments"));
  auto const walls = Noggit::Ai::parseProceduralScatter(calls[4].at("arguments"));
  auto const props = Noggit::Ai::parseProceduralProps(calls[5].at("arguments"));
  auto const vegetation = Noggit::Ai::parseProceduralScatter(calls[6].at("arguments"));
  if (!terrain.layout) throw std::runtime_error("terrain: " + terrain.error);
  if (!liquid.layout) throw std::runtime_error("liquid: " + liquid.error);
  if (!walls.scatter) throw std::runtime_error("walls: " + walls.error);
  if (!props.props) throw std::runtime_error("props: " + props.error);
  if (!vegetation.scatter) throw std::runtime_error("vegetation: " + vegetation.error);
  require(terrain.layout && liquid.layout && walls.scatter
            && props.props && vegetation.scatter,
          "blueprint must compile to valid generic tool arguments");
  auto wall_priority = -1;
  auto wall_feature_count = std::size_t{0};
  for (auto const& feature : terrain.layout->features)
    if (feature.name.ends_with("_wall_mass"))
    {
      wall_priority = std::max(wall_priority, feature.priority);
      ++wall_feature_count;
    }
  require(wall_feature_count > 0, "the canonical jungle wall mask is empty");
  require(std::all_of(terrain.layout->features.begin(), terrain.layout->features.end(),
            [&](auto const& feature)
            {
              auto const guaranteed_throat
                = feature.name.ends_with("_spur_path")
                  || feature.name.ends_with("_wall_cut")
                  || feature.name == "jungle_2_lane_door_path"
                  || feature.name == "jungle_4_lane_door_path";
              return !guaranteed_throat
                || feature.priority > wall_priority;
            }),
          "jungle cuts and guaranteed throats must carve the wall mask");
  // base_height 20 and river_depth 8 put the bed bottom at 12; the water
  // column must stay under WoW's swim threshold so the river is wadeable.
  require(liquid.layout->features.front().points.front().height <= 13.01f,
          "the river water column must stay wadeable on foot");
  auto const& liquid_feature = liquid.layout->features.front();
  auto minimum_u = 1.0f;
  auto maximum_u = 0.0f;
  auto minimum_v = 1.0f;
  auto maximum_v = 0.0f;
  for (auto const& point : liquid_feature.points)
  {
    minimum_u = std::min(minimum_u, point.u);
    maximum_u = std::max(maximum_u, point.u);
    minimum_v = std::min(minimum_v, point.v);
    maximum_v = std::max(maximum_v, point.v);
  }
  require(maximum_u - minimum_u + 2.0f * liquid_feature.half_width_ratio
              >= Noggit::Ai::moba_arena_minimum_liquid_span_u
            && maximum_v - minimum_v + 2.0f * liquid_feature.half_width_ratio
              >= Noggit::Ai::moba_arena_minimum_liquid_span_v,
          "the canonical river can never satisfy its runtime span contract");
  auto expected_wall_paths = std::set<std::string>{};
  auto expected_vegetation_paths = std::set<std::string>{};
  for (auto const& asset : spec.at("assets"))
    (asset.at("role") == "wall" ? expected_wall_paths : expected_vegetation_paths)
      .insert(asset.at("path").get<std::string>());
  auto actual_wall_paths = std::set<std::string>{};
  auto actual_vegetation_paths = std::set<std::string>{};
  for (auto const& asset : walls.scatter->assets)
    actual_wall_paths.insert(asset.path);
  for (auto const& asset : vegetation.scatter->assets)
    actual_vegetation_paths.insert(asset.path);
  require(expected_wall_paths == actual_wall_paths
          && expected_vegetation_paths == actual_vegetation_paths
          && std::all_of(walls.scatter->assets.begin(), walls.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "wall"; })
          && std::none_of(vegetation.scatter->assets.begin(), vegetation.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "wall"; })
          && std::any_of(vegetation.scatter->assets.begin(), vegetation.scatter->assets.end(),
            [](auto const& asset) { return asset.role == "rock"; }),
          "wall and vegetation assets must be isolated in separate batches");
  require(!walls.scatter->regions.empty()
            && std::all_of(walls.scatter->regions.begin(), walls.scatter->regions.end(),
              [](auto const& region)
              {
                return region.role == "wall" && region.density_per_tile > 0;
              }),
          "wall scatter must keep non-empty dedicated regions");
  require(std::any_of(vegetation.scatter->regions.begin(), vegetation.scatter->regions.end(),
            [](auto const& region)
            {
              return region.role == "canopy" || region.role == "rock";
            }),
          "vegetation scatter must retain canopy or decorative rock regions");
  auto const canopy_region_count = static_cast<std::size_t>(std::count_if(
    vegetation.scatter->regions.begin(), vegetation.scatter->regions.end(),
    [](auto const& region) { return region.role == "canopy"; }));
  require(canopy_region_count * 5 >= wall_feature_count * 4,
          "at least 80% of canonical jungle walls need canopy coverage");
  auto candidateCount = [](Noggit::Ai::ProceduralScatter const& scatter, int tiles)
  {
    auto count = std::size_t{0};
    for (int z = 0; z < tiles; ++z)
      for (int x = 0; x < tiles; ++x)
        for (auto const& region : scatter.regions)
          if (Noggit::Ai::proceduralScatterRegionIntersects(
                region, x / static_cast<float>(tiles), (x + 1) / static_cast<float>(tiles),
                z / static_cast<float>(tiles), (z + 1) / static_cast<float>(tiles)))
            count += region.density_per_tile;
    return count;
  };
  for (auto const* scatter : {&*walls.scatter, &*vegetation.scatter})
  {
    require(candidateCount(*scatter, 2) <= 4096,
            "a 2x2 MOBA batch must fit the compact scatter budget");
    require(candidateCount(*scatter, 3) <= 16384,
            "a 3x3 MOBA batch must fit the global scatter candidate limit");
    require(candidateCount(*scatter, 4) <= 16384,
            "a 4x4 MOBA batch must fit the global scatter candidate limit");
  }

  auto sparse_vegetation = specification();
  sparse_vegetation["vegetation_density_per_tile"] = 80;
  auto const sparse_blueprint = Noggit::Ai::compileMobaArenaBlueprint(sparse_vegetation, 4);
  auto const sparse_walls = Noggit::Ai::parseProceduralScatter(
    sparse_blueprint.at("next_calls").at(4).at("arguments"));
  require(sparse_walls.scatter
            && sparse_walls.scatter->regions.size() == walls.scatter->regions.size()
            && std::equal(sparse_walls.scatter->regions.begin(),
                          sparse_walls.scatter->regions.end(),
                          walls.scatter->regions.begin(),
                          [](auto const& sparse, auto const& normal)
                          { return sparse.density_per_tile == normal.density_per_tile; }),
          "vegetation density must not weaken gameplay wall chains");

  std::set<std::string> prop_names;
  std::size_t lamp_props = 0;
  std::size_t light_props = 0;
  for (auto const& prop : props.props->props)
  {
    require(prop_names.insert(prop.name).second, "prop names must be unique");
    require(prop.u > 0.0f && prop.u < 1.0f && prop.v > 0.0f && prop.v < 1.0f,
            "props must stay inside the map footprint");
    if (prop.name.starts_with("lamp_") && !prop.name.starts_with("lamp_glow_"))
      ++lamp_props;
    if (prop.path.find("noggit_light") != std::string::npos) ++light_props;
  }
  require(props.props->props.size() <= 256, "the prop budget was exceeded");
  require(prop_names.contains("team_left_landmark")
            && prop_names.contains("team_right_landmark")
            && prop_names.contains("objective_north_landmark")
            && prop_names.contains("objective_south_landmark"),
          "base and objective landmarks are missing");
  require(lamp_props >= 30,
          "lane lamp chains are incomplete");
  require(light_props >= 40,
          "every ambience light must come from the Patch-E light pack");

  requireMutationIssues(mutation_baseline, {"poi.unreachable"}, [](nlohmann::json& corrupted)
  {
    for (auto& point : terrainFeature(corrupted, "objective_north").at("points"))
      point["height"] = 100.0;
  });
  requireMutationIssues(mutation_baseline, {"navigation.boundary_continuity"},
    [](nlohmann::json& corrupted)
  {
    auto barrier = terrainFeature(corrupted, "objective_north");
    barrier["name"] = "top_lane_cut";
    barrier["priority"] = 100;
    barrier["half_width_ratio"] = .005;
    barrier["transition_width_ratio"] = .001;
    barrier["points"] = nlohmann::json::array({
      {{"u", .47}, {"v", .025}, {"height", 100.0}},
      {{"u", .53}, {"v", .025}, {"height", 100.0}},
      {{"u", .53}, {"v", .09}, {"height", 100.0}},
      {{"u", .47}, {"v", .09}, {"height", 100.0}}
    });
    replaceWallMass(corrupted, 46, std::move(barrier));
  });
  requireMutationIssues(mutation_baseline, {"boundary.opening_width"},
    [](nlohmann::json& corrupted)
  {
    auto& breach = terrainFeature(corrupted, "jungle_2_upper_wall_cut");
    breach["priority"] = 100;
    breach["half_width_ratio"] = .20;
    breach["transition_width_ratio"] = .001;
    breach["width_variation_ratio"] = 0.0;
  });
  requireMutationIssues(mutation_baseline, {"boundary.separator_width"},
    [](nlohmann::json& corrupted)
  {
    auto& breach = terrainFeature(corrupted, "jungle_2_upper_wall_cut");
    breach["priority"] = 99;
    breach["half_width_ratio"] = .20;
    breach["transition_width_ratio"] = .001;
    breach["width_variation_ratio"] = 0.0;
    auto separator = terrainFeature(corrupted, "objective_north");
    separator["name"] = "middle_lane_thin_separator";
    separator["priority"] = 100;
    separator["half_width_ratio"] = 0.0;
    separator["transition_width_ratio"] = .001;
    separator["points"] = nlohmann::json::array({
      {{"u", .6650}, {"v", .2843}, {"height", 26.5}},
      {{"u", .6680}, {"v", .2817}, {"height", 26.5}},
      {{"u", .7350}, {"v", .3557}, {"height", 26.5}},
      {{"u", .7320}, {"v", .3583}, {"height", 26.5}}
    });
    replaceWallMass(corrupted, 46, std::move(separator));
  });
  requireMutationIssues(mutation_baseline, {"quadrant.loop"},
    [](nlohmann::json& corrupted)
  {
    auto barrier = terrainFeature(corrupted, "objective_north");
    barrier["name"] = "north_top_door_block";
    barrier["priority"] = 100;
    barrier["half_width_ratio"] = .005;
    barrier["transition_width_ratio"] = .001;
    barrier["points"] = nlohmann::json::array({
      {{"u", .71}, {"v", .03}, {"height", 100.0}},
      {{"u", .79}, {"v", .03}, {"height", 100.0}},
      {{"u", .79}, {"v", .10}, {"height", 100.0}},
      {{"u", .71}, {"v", .10}, {"height", 100.0}}
    });
    replaceWallMass(corrupted, 46, std::move(barrier));
  });
  requireMutationIssues(mutation_baseline, {"corridor.declared_width"},
    [](nlohmann::json& corrupted)
  {
    for (auto& feature : callArguments(
           corrupted, "apply_terrain_layout_on_map").at("features"))
    {
      auto const& name = feature.at("name").get_ref<std::string const&>();
      if (!name.starts_with("jungle_") || !name.ends_with("_path")) continue;
      feature["half_width_ratio"] = .005;
      feature["transition_width_ratio"] = .001;
      feature["width_variation_ratio"] = 0.0;
    }
  });
  requireMutationIssues(mutation_baseline, {"corridor.physical_width"},
    [](nlohmann::json& corrupted)
  {
    auto first_wall = terrainFeature(corrupted, "objective_north");
    first_wall["name"] = "narrow_door_first_wall";
    first_wall["shape"] = "corridor";
    first_wall["priority"] = 100;
    first_wall["half_width_ratio"] = .005;
    first_wall["transition_width_ratio"] = .001;
    first_wall["points"] = nlohmann::json::array({
      {{"u", .9336}, {"v", .3809}, {"height", 26.5}},
      {{"u", .8925}, {"v", .5254}, {"height", 26.5}},
      {{"u", .9258}, {"v", .6367}, {"height", 26.5}},
      {{"u", .9336}, {"v", .6797}, {"height", 26.5}}
    });
    auto second_wall = first_wall;
    second_wall["name"] = "narrow_door_second_wall";
    second_wall["points"] = nlohmann::json::array({
      {{"u", .9176}, {"v", .3809}, {"height", 26.5}},
      {{"u", .8765}, {"v", .5254}, {"height", 26.5}},
      {{"u", .9098}, {"v", .6367}, {"height", 26.5}},
      {{"u", .9176}, {"v", .6797}, {"height", 26.5}}
    });
    replaceWallMass(corrupted, 45, std::move(first_wall));
    replaceWallMass(corrupted, 46, std::move(second_wall));
  });
  requireMutationIssues(mutation_baseline,
    {"manifest.kind", "camp.route_mouth_count"},
    [](nlohmann::json& corrupted)
  {
    auto& camps = corrupted.at("moba_semantics").at("camps");
    auto const hub = std::find_if(camps.begin(), camps.end(),
      [](nlohmann::json const& camp)
    {
      return camp.at("kind") == "hub";
    });
    auto const medium = std::find_if(camps.begin(), camps.end(),
      [](nlohmann::json const& camp)
    {
      return camp.at("kind") == "medium";
    });
    if (hub == camps.end() || medium == camps.end())
      throw std::runtime_error("missing camp kinds in mutation fixture");
    (*hub)["kind"] = "spur";
    (*medium)["kind"] = "invalid";
  });
  requireMutationIssues(mutation_baseline, {"camp.mouth_count"},
    [](nlohmann::json& corrupted)
  {
    auto ring = terrainFeature(corrupted, "objective_north");
    ring["name"] = "north_red_blocked_ring";
    ring["priority"] = 100;
    ring["half_width_ratio"] = 0.0;
    ring["transition_width_ratio"] = .001;
    ring["points"] = nlohmann::json::array({
      {{"u", .4055}, {"v", .2637}, {"height", 26.5}},
      {{"u", .4405}, {"v", .2031}, {"height", 26.5}},
      {{"u", .5105}, {"v", .2031}, {"height", 26.5}},
      {{"u", .5455}, {"v", .2637}, {"height", 26.5}},
      {{"u", .5105}, {"v", .3243}, {"height", 26.5}},
      {{"u", .4405}, {"v", .3243}, {"height", 26.5}}
    });
    replaceWallMass(corrupted, 46, std::move(ring));
  });
  requireMutationIssues(mutation_baseline, {"manifest.boundaries"},
    [](nlohmann::json& corrupted)
  {
    auto& boundaries = corrupted.at("moba_semantics").at("boundaries");
    boundaries.erase(boundaries.begin());
  });
  requireMutationIssues(mutation_baseline,
    {"route.boundary_contacts", "quadrant.door_count"},
    [](nlohmann::json& corrupted)
  {
    auto& points = terrainFeature(
      corrupted, "jungle_1_lane_door_path").at("points");
    points = nlohmann::json::array({points.at(1)});
  });
  requireMutationIssues(mutation_baseline, {"wall.too_thick"},
    [](nlohmann::json& corrupted)
  {
    for (auto& feature : callArguments(
           corrupted, "apply_terrain_layout_on_map").at("features"))
    {
      auto const& name = feature.at("name").get_ref<std::string const&>();
      if (!name.starts_with("jungle_") || !name.ends_with("_wall_mass")) continue;
      feature["half_width_ratio"] = .25;
      feature["transition_width_ratio"] = .001;
    }
  });
  requireMutationIssues(mutation_baseline, {"symmetry.geometry"},
    [](nlohmann::json& corrupted)
  {
    terrainFeature(corrupted, "objective_north")["half_width_ratio"] = .006;
  });
  requireMutationIssues(mutation_baseline, {"symmetry.rotation_mask"},
    [](nlohmann::json& corrupted)
  {
    for (auto& feature : callArguments(
           corrupted, "apply_terrain_layout_on_map").at("features"))
    {
      auto const& name = feature.at("name").get_ref<std::string const&>();
      if (!name.starts_with("jungle_") || !name.ends_with("_wall_mass"))
        continue;
      auto u = 0.0;
      auto v = 0.0;
      for (auto const& point : feature.at("points"))
      {
        u += point.at("u").get<double>();
        v += point.at("v").get<double>();
      }
      u /= static_cast<double>(feature.at("points").size());
      v /= static_cast<double>(feature.at("points").size());
      if (u + v >= 1.0) continue;
      for (auto& point : feature.at("points")) point["height"] = 20.0;
    }
  });
  requireMutationIssues(mutation_baseline, {"texture.jungle_identity"},
    [](nlohmann::json& corrupted)
  {
    auto& paths = callArguments(
      corrupted, "apply_terrain_layout_on_map").at("texture_paths");
    std::swap(paths.at(0), paths.at(2));
  });
  requireMutationIssues(mutation_baseline, {"liquid.river_alignment"},
    [](nlohmann::json& corrupted)
  {
    auto& points = callArguments(
      corrupted, "apply_liquid_layout_on_map").at("features").front().at("points");
    for (auto& point : points) point["v"] = point.at("v").get<double>() + .05;
  });
  requireMutationIssues(mutation_baseline, {"liquid.river_continuity"},
    [](nlohmann::json& corrupted)
  {
    auto& points = terrainFeature(corrupted, "river_bed").at("points");
    points.at(points.size() / 2)["height"] = 80.0;
  });
  requireMutationIssues(mutation_baseline, {"liquid.wadeable"},
    [](nlohmann::json& corrupted)
  {
    auto trench = terrainFeature(corrupted, "objective_north");
    trench["name"] = "liquid_depth_corruption";
    trench["priority"] = 100;
    trench["half_width_ratio"] = .005;
    trench["transition_width_ratio"] = .001;
    trench["points"] = nlohmann::json::array({
      {{"u", .485}, {"v", .49}, {"height", 0.0}},
      {{"u", .50}, {"v", .48}, {"height", 0.0}},
      {{"u", .515}, {"v", .49}, {"height", 0.0}},
      {{"u", .515}, {"v", .51}, {"height", 0.0}},
      {{"u", .50}, {"v", .52}, {"height", 0.0}},
      {{"u", .485}, {"v", .51}, {"height", 0.0}}
    });
    replaceWallMass(corrupted, 46, std::move(trench));
  });
  requireMutationIssues(mutation_baseline,
    {"props.missing", "props.name_contract"}, [](nlohmann::json& corrupted)
  {
    placedProp(corrupted, "team_left_landmark")["name"] = "orphan_landmark";
  });
  requireMutationIssues(mutation_baseline,
    {"props.anchor", "props.liquid"}, [](nlohmann::json& corrupted)
  {
    auto& prop = placedProp(corrupted, "north_red_brazier");
    prop["u"] = .5;
    prop["v"] = .5;
  });
  requireMutationIssues(mutation_baseline,
    {"props.anchor", "props.light_pair"}, [](nlohmann::json& corrupted)
  {
    auto& prop = placedProp(corrupted, "team_left_glow");
    prop["u"] = prop.at("u").get<double>() + .01;
  });
  requireMutationIssues(mutation_baseline, {"props.unwalkable"},
    [](nlohmann::json& corrupted)
  {
    auto const& wall = terrainFeature(corrupted, "jungle_5_wall_mass");
    auto u = 0.0;
    auto v = 0.0;
    for (auto const& point : wall.at("points"))
    {
      u += point.at("u").get<double>();
      v += point.at("v").get<double>();
    }
    u /= static_cast<double>(wall.at("points").size());
    v /= static_cast<double>(wall.at("points").size());
    auto& prop = placedProp(corrupted, "north_red_brazier");
    prop["u"] = u;
    prop["v"] = v;
  });
  requireMutationIssues(mutation_baseline, {"props.spatial_duplicate"},
    [](nlohmann::json& corrupted)
  {
    auto& duplicate = placedProp(corrupted, "north_raptors_brazier");
    auto const& existing = placedProp(corrupted, "north_red_brazier");
    duplicate["u"] = existing.at("u");
    duplicate["v"] = existing.at("v");
  });
  requireMutationIssues(mutation_baseline, {"symmetry.props"},
    [](nlohmann::json& corrupted)
  {
    for (auto const name : {"north_red_brazier", "north_red_flame"})
    {
      auto& prop = placedProp(corrupted, name);
      prop["u"] = prop.at("u").get<double>() + .005;
    }
  });
  requireMutationIssues(mutation_baseline, {"manifest.missing"},
    [](nlohmann::json& corrupted)
  {
    corrupted.erase("moba_semantics");
  });
  requireMutationIssues(mutation_baseline,
    {"manifest.objectives", "manifest.bases"}, [](nlohmann::json& corrupted)
  {
    auto& semantics = corrupted.at("moba_semantics");
    semantics.at("objectives").front()["name"] = "objective_unknown";
    auto& bases = semantics.at("bases");
    bases.erase(bases.end() - 1);
  });
  requireMutationIssues(mutation_baseline,
    {"manifest.unregistered", "boundary.undeclared_opening"},
    [](nlohmann::json& corrupted)
  {
    auto& routes = corrupted.at("moba_semantics").at("routes");
    routes.erase(std::find_if(routes.begin(), routes.end(), [](auto const& route)
    {
      return route.at("name") == "jungle_2_upper_wall_cut";
    }));
  });
  requireMutationIssues(mutation_baseline,
    {"scatter.route_exclusion", "scatter.camp_exclusion"},
    [](nlohmann::json& corrupted)
  {
    callArguments(corrupted, "scatter_assets_on_map", 1)["exclusions"]
      = nlohmann::json::array();
  });
  requireMutationIssues(mutation_baseline, {"feature.core_missing"},
    [](nlohmann::json& corrupted)
  {
    auto cover = terrainFeature(corrupted, "arena_ground");
    cover["name"] = "arena_ground_cover";
    cover["priority"] = 6;
    replaceWallMass(corrupted, 46, std::move(cover));
  });

  auto missing_walls = specification();
  for (auto& asset : missing_walls["assets"])
    if (asset.at("role") == "wall") asset["role"] = "rock";
  try
  {
    static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(missing_walls, 4));
    require(false, "a specification without wall assets was accepted");
  }
  catch (std::invalid_argument const&)
  {
  }

  auto invalid = specification();
  invalid["extra"] = true;
  try
  {
    static_cast<void>(Noggit::Ai::compileMobaArenaBlueprint(invalid, 4));
    require(false, "extra root field accepted");
  }
  catch (std::invalid_argument const&)
  {
  }

  verifyGolden(audit, update_golden);
}
