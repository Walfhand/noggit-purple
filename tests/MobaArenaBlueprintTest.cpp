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
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <numbers>
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

  void requireAudit(
    Noggit::Ai::MobaArenaAuditReport const& audit,
    std::filesystem::path const& artifacts = NOGGIT_TEST_ARTIFACT_DIR)
  {
    if (audit.ok()) return;
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

  void writeMountainPreview(
    Noggit::Ai::ProceduralLayout const& layout,
    std::filesystem::path const& path,
    float base_height,
    float world_size)
  {
    constexpr unsigned size = 384;
    constexpr unsigned width = size * 3;
    auto const spacing = world_size / 512.0f;
    std::vector<float> heights(size * size);
    for (unsigned y = 0; y < size; ++y)
      for (unsigned x = 0; x < size; ++x)
        heights[y * size + x]
          = Noggit::Ai::sampleSmoothedProceduralLayoutHeight(
              layout,
              (static_cast<float>(x) + .5f) / static_cast<float>(size),
              (static_cast<float>(y) + .5f) / static_cast<float>(size),
              base_height, world_size, world_size, spacing);

    constexpr std::array<std::array<float, 3>, 4> colors{{
      {{48.0f, 75.0f, 39.0f}},
      {{93.0f, 88.0f, 70.0f}},
      {{83.0f, 60.0f, 43.0f}},
      {{111.0f, 113.0f, 110.0f}}
    }};
    constexpr std::array<float, 3> light{{-.48f, .76f, -.44f}};
    auto const [minimum_height, maximum_height] = std::minmax_element(
      heights.begin(), heights.end());
    auto const height_range = std::max(1.0f, *maximum_height - *minimum_height);
    std::vector<unsigned char> pixels(width * size * 4, 255);
    std::vector<std::array<unsigned char, 3>> terrain_colors(size * size);
    for (unsigned y = 0; y < size; ++y)
      for (unsigned x = 0; x < size; ++x)
      {
        auto const pixel = (static_cast<std::size_t>(y) * width + x) * 4;
        pixels[pixel] = static_cast<unsigned char>(10 + y * 12 / size);
        pixels[pixel + 1] = static_cast<unsigned char>(17 + y * 16 / size);
        pixels[pixel + 2] = static_cast<unsigned char>(22 + y * 20 / size);
      }
    auto heightAt = [&](int x, int y)
    {
      x = std::clamp(x, 0, static_cast<int>(size) - 1);
      y = std::clamp(y, 0, static_cast<int>(size) - 1);
      return heights[static_cast<std::size_t>(y) * size
        + static_cast<std::size_t>(x)];
    };
    for (unsigned y = 0; y < size; ++y)
      for (unsigned x = 0; x < size; ++x)
      {
        auto const gradient_x = (heightAt(x + 1, y) - heightAt(x - 1, y))
          / (2.0f * spacing);
        auto const gradient_z = (heightAt(x, y + 1) - heightAt(x, y - 1))
          / (2.0f * spacing);
        auto const normal_length
          = std::sqrt(gradient_x * gradient_x + gradient_z * gradient_z + 1.0f);
        auto const shade = std::clamp(
          (-gradient_x * light[0] + light[1] - gradient_z * light[2])
            / normal_length,
          .08f, 1.0f);
        auto const slope = std::atan(std::hypot(gradient_x, gradient_z))
          * 180.0f / static_cast<float>(std::numbers::pi);
        auto const sample = Noggit::Ai::sampleProceduralLayout(
          layout,
          (static_cast<float>(x) + .5f) / static_cast<float>(size),
          (static_cast<float>(y) + .5f) / static_cast<float>(size),
          base_height, slope, world_size, world_size);
        std::array<float, 3> color{};
        for (std::size_t layer = 0; layer < colors.size(); ++layer)
          for (std::size_t channel = 0; channel < color.size(); ++channel)
            color[channel] += colors[layer][channel]
              * static_cast<float>(sample.quantized_weights[layer]) / 255.0f;
        auto const pixel
          = (static_cast<std::size_t>(y) * width + x + size) * 4;
        auto& terrain_color
          = terrain_colors[static_cast<std::size_t>(y) * size + x];
        for (std::size_t channel = 0; channel < color.size(); ++channel)
        {
          terrain_color[channel] = static_cast<unsigned char>(std::clamp(
            color[channel] * (.38f + .62f * shade), 0.0f, 255.0f));
          pixels[pixel + channel] = terrain_color[channel];
        }

        auto const elevation = std::clamp(
          (heights[static_cast<std::size_t>(y) * size + x] - *minimum_height)
            / height_range,
          0.0f, 1.0f);
        auto const contour = std::fmod(
          std::max(0.0f,
            heights[static_cast<std::size_t>(y) * size + x] - base_height),
          20.0f) < 1.5f ? .58f : 1.0f;
        std::array<float, 3> elevation_color{
          37.0f + 122.0f * elevation,
          70.0f + 86.0f * elevation,
          35.0f + 121.0f * elevation
        };
        auto const elevation_pixel
          = (static_cast<std::size_t>(y) * width + x + size * 2) * 4;
        for (std::size_t channel = 0;
             channel < elevation_color.size(); ++channel)
          pixels[elevation_pixel + channel] = static_cast<unsigned char>(
            elevation_color[channel] * contour);
      }

    std::vector<float> depth_buffer(
      static_cast<std::size_t>(size) * size,
      std::numeric_limits<float>::lowest());
    for (unsigned y = 0; y < size; ++y)
      for (unsigned x = 0; x < size; ++x)
      {
        auto const height = heights[static_cast<std::size_t>(y) * size + x];
        auto const height_delta = height - base_height;
        auto const screen_x = static_cast<int>(size / 2)
          + static_cast<int>((static_cast<float>(x) - static_cast<float>(y))
                             * .38f);
        auto const screen_y = static_cast<int>(150.0f
          + (static_cast<float>(x) + static_cast<float>(y)) * .22f
          - height_delta * .55f);
        auto const depth = static_cast<float>(x + y) + height_delta * .6f;
        auto const& color
          = terrain_colors[static_cast<std::size_t>(y) * size + x];
        for (auto offset_y = -3; offset_y <= 3; ++offset_y)
          for (auto offset_x = -3; offset_x <= 3; ++offset_x)
          {
            auto const projected_x = screen_x + offset_x;
            auto const projected_y = screen_y + offset_y;
            if (projected_x < 0 || projected_y < 0
                || projected_x >= static_cast<int>(size)
                || projected_y >= static_cast<int>(size))
              continue;
            auto const projected = static_cast<std::size_t>(projected_y) * size
              + static_cast<std::size_t>(projected_x);
            if (depth <= depth_buffer[projected]) continue;
            depth_buffer[projected] = depth;
            auto const pixel = (static_cast<std::size_t>(projected_y) * width
                                + static_cast<std::size_t>(projected_x)) * 4;
            for (std::size_t channel = 0; channel < color.size(); ++channel)
              pixels[pixel + channel] = color[channel];
          }
      }

    std::filesystem::create_directories(path.parent_path());
    auto const error = lodepng::encode(path.string(), pixels, width, size);
    if (error)
      throw std::runtime_error("cannot write MOBA mountain preview: "
        + std::string{lodepng_error_text(error)});
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
  auto const dump_audit = (argc == 2 || argc == 3)
    && std::string_view{argv[1]} == "--dump-audit";
  if (argc > 3 || (argc >= 2 && !update_golden && !dump_audit))
    throw std::runtime_error(
      "usage: noggit-moba-arena-blueprint-test "
      "[--update-golden|--dump-audit [output-directory]]");
  auto const dump_artifacts = dump_audit && argc == 3
    ? std::filesystem::path{argv[2]}
    : std::filesystem::temp_directory_path() / "noggit-moba-audit";
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
  auto const& texture_paths = spec.at("texture_paths");
  require(std::all_of(texture_paths.begin(), texture_paths.begin() + 3,
          [](nlohmann::json const& path)
          {
            return path.get_ref<std::string const&>()
              .starts_with("tileset/expansion07/");
          })
            && texture_paths.at(3)
              == "tileset/expansion02/dragonblight/ig_rockjaggeda.blp",
          "the default MOBA rock texture must resolve in WotLK");
  require(spec.at("skybox_path")
              == "environments/stars/legionnexus_netherskybox01.m2"
            && spec.at("skybox_flags") == 0,
          "the map's selected skybox is not the MOBA default");
  require(std::abs(spec.at("arena_scale_ratio").get<double>() - .32) < .0001,
          "the default 2x2 MOBA arena must target Summoner's Rift traversal time");
  auto reference_spec = spec;
  reference_spec["arena_scale_ratio"] = 1.0;
  auto first = Noggit::Ai::compileMobaArenaBlueprint(reference_spec, 4);
  if (dump_audit)
  {
    auto const audit = Noggit::Ai::auditMobaArenaBlueprint(first, 4);
    requireAudit(audit, dump_artifacts);
    std::filesystem::create_directories(dump_artifacts);
    auto const write_error = lodepng::encode(
      (dump_artifacts / "moba-arena-semantic.actual.png").string(),
      audit.preview_rgba, audit.preview_width, audit.preview_height);
    if (write_error)
      throw std::runtime_error("cannot write MOBA audit preview");
    auto compact = Noggit::Ai::compileMobaArenaBlueprint(spec, 2);
    compact.erase("arena_fit");
    auto const compact_audit
      = Noggit::Ai::auditMobaArenaBlueprint(compact, 2);
    auto const compact_write_error = lodepng::encode(
      (dump_artifacts / "moba-arena-compact.actual.png").string(),
      compact_audit.preview_rgba,
      compact_audit.preview_width, compact_audit.preview_height);
    if (compact_write_error)
      throw std::runtime_error("cannot write compact MOBA audit preview");
  }
  auto const second = Noggit::Ai::compileMobaArenaBlueprint(reference_spec, 4);
  require(first == second, "blueprint must be deterministic");
  auto mutation_baseline = Noggit::Ai::compileMobaArenaBlueprint(reference_spec, 2);
  auto const three_tile_blueprint = Noggit::Ai::compileMobaArenaBlueprint(reference_spec, 3);
  auto const translated_blueprint = Noggit::Ai::compileMobaArenaBlueprint(
    reference_spec, 4, 20, 20);
  require(mutation_baseline.at("audit").at("ok") == true
            && three_tile_blueprint.at("audit").at("ok") == true
            && translated_blueprint.at("audit").at("ok") == true,
          "every supported footprint size and translated tile origin must pass the production audit");
  require(translated_blueprint.at("audit").at("metrics")
              .at("scatter").at("tile_origin").at("min_tile_x") == 20
            && translated_blueprint.at("audit").at("metrics")
              .at("scatter").at("tile_origin").at("min_tile_z") == 20,
          "the production audit ignored the map's absolute tile origin");

  auto fitted = Noggit::Ai::compileMobaArenaBlueprint(spec, 2);
  require(std::abs(fitted.at("arena_fit").at("playable_side_world_units")
                       .get<double>() - 341.333333) < .001
            && fitted.at("arena_fit").at("estimated_base_to_base_run_seconds")
                 .get<double>() > 50.0
            && fitted.at("arena_fit").at("estimated_base_to_base_run_seconds")
                 .get<double>() < 53.0,
          "the compact 2x2 arena no longer matches the target traversal time");
  auto const& fitted_ground = terrainFeature(fitted, "arena_ground");
  auto fitted_min_u = 1.0;
  auto fitted_max_u = 0.0;
  for (auto const& point : fitted_ground.at("points"))
  {
    fitted_min_u = std::min(fitted_min_u, point.at("u").get<double>());
    fitted_max_u = std::max(fitted_max_u, point.at("u").get<double>());
  }
  auto const& perimeter = terrainFeature(fitted, "arena_perimeter_relief");
  auto const& mountain_mass = terrainFeature(fitted, "arena_mountain_mass");
  auto const base_height = fitted_ground.at("points").front().at("height").get<double>();
  auto const& reference_relief
    = terrainFeature(mutation_baseline, "jungle_1_wall_mass");
  auto const& fitted_relief = terrainFeature(fitted, "jungle_1_wall_mass");
  auto const reference_relief_height
    = reference_relief.at("points").front().at("height").get<double>();
  auto const fitted_relief_height
    = fitted_relief.at("points").front().at("height").get<double>();
  auto perimeter_min_height = std::numeric_limits<double>::max();
  auto perimeter_max_height = std::numeric_limits<double>::lowest();
  for (auto const& point : perimeter.at("points"))
  {
    perimeter_min_height = std::min(
      perimeter_min_height, point.at("height").get<double>());
    perimeter_max_height = std::max(
      perimeter_max_height, point.at("height").get<double>());
  }
  auto const fitted_scale
    = fitted.at("arena_fit").at("scale_ratio").get<double>();
  auto const fitted_margin = (1.0 - fitted_scale) * .5;
  require(std::abs(fitted_min_u) < .0001
            && std::abs(fitted_max_u - 1.0) < .0001
            && fitted_ground.at("transition_width_ratio").get<double>() >= .10
            && fitted_ground.at("roughness_amplitude") == 0.0
            && fitted_ground.at("priority") == 5
            && mountain_mass.at("shape") == "corridor"
            && mountain_mass.at("texture_layer") == 3
            && mountain_mass.at("priority") == 6
            && mountain_mass.at("points").size() == 17
            && mountain_mass.at("points").front()
              != mountain_mass.at("points").back()
            && mountain_mass.at("half_width_ratio").get<double>() >= .022
            && mountain_mass.at("half_width_ratio").get<double>() <= .032
            && mountain_mass.at("transition_width_ratio").get<double>() >= .08
            && mountain_mass.at("transition_width_ratio").get<double>() <= .12
            && mountain_mass.at("roughness_amplitude").get<double>() >= 98.0
            && mountain_mass.at("roughness_amplitude").get<double>() <= 100.0
            && mountain_mass.at("texture_strength").get<double>() <= .6
            && std::abs(
                 mountain_mass.at("width_variation_ratio").get<double>() - .55)
              < .0001
            && perimeter.at("shape") == "corridor"
            && perimeter.at("texture_layer") == 3
            && perimeter.at("priority") == 7
            && perimeter.at("points").size() == 17
            && perimeter.at("points").front()
              != perimeter.at("points").back()
            && perimeter.at("half_width_ratio").get<double>() >= .022
            && perimeter.at("transition_width_ratio").get<double>() >= .08
            && perimeter_max_height - perimeter_min_height < .0001
            && perimeter.at("roughness_amplitude").get<double>() >= 98.0
            && perimeter.at("roughness_amplitude").get<double>() <= 100.0
            && perimeter.at("texture_strength").get<double>() <= .6
            && std::abs(perimeter.at("width_variation_ratio").get<double>() - .55)
              < .0001
            && std::abs(fitted_relief_height - base_height
                  - (reference_relief_height - base_height)) < .0001
            && std::abs(fitted_relief.at("roughness_amplitude").get<double>()
                  - reference_relief.at("roughness_amplitude").get<double>())
              < .0001
            && callArguments(fitted, "apply_terrain_layout_on_map")
                 .at("features").size()
              == callArguments(first, "apply_terrain_layout_on_map")
                   .at("features").size() + 2
            && fitted.at("moba_semantics").at("camps").size()
              == first.at("moba_semantics").at("camps").size()
            && callArguments(fitted, "place_props_on_map").at("props").size()
              == callArguments(first, "place_props_on_map").at("props").size(),
          "fitting the arena must preserve its generated content inside a raised perimeter");
  for (std::size_t index = 0; index < perimeter.at("points").size(); ++index)
    require(std::abs(
              mountain_mass.at("points")[index].at("u").get<double>()
                + perimeter.at("points")[index].at("u").get<double>() - 1.0)
              < .000001
            && std::abs(
              mountain_mass.at("points")[index].at("v").get<double>()
                + perimeter.at("points")[index].at("v").get<double>() - 1.0)
              < .000001
            && mountain_mass.at("points")[index].at("height")
              == perimeter.at("points")[index].at("height"),
            "the two open mountain arcs lost their opposite overlap");
  auto const fitted_liquid = Noggit::Ai::parseProceduralLiquidLayout(
    callArguments(fitted, "apply_liquid_layout_on_map"));
  auto const fitted_terrain = Noggit::Ai::parseProceduralLayout(
    callArguments(fitted, "apply_terrain_layout_on_map"));
  require(fitted_liquid.layout.has_value() && fitted_terrain.layout.has_value(),
          "the fitted terrain and river must remain valid layouts");
  for (auto const& corner : std::array<std::array<float, 2>, 4>{{
         {{0.0f, 0.0f}}, {{1.0f, 0.0f}}, {{1.0f, 1.0f}}, {{0.0f, 1.0f}}}})
  {
    auto const reset = Noggit::Ai::sampleProceduralLayout(
      *fitted_terrain.layout, corner[0], corner[1], 300.0f, 0.0f,
      3200.0f / 3.0f, 3200.0f / 3.0f);
    require(std::abs(reset.height - base_height) < .001f
              && reset.quantized_weights[0] == 255,
            "regenerating the compact arena retained stale outer relief");
  }
  auto const& reference_terrain
    = callArguments(mutation_baseline, "apply_terrain_layout_on_map");
  auto const& compact_terrain
    = callArguments(fitted, "apply_terrain_layout_on_map");
  constexpr auto degrees_to_radians = 0.017453292519943295;
  constexpr auto radians_to_degrees = 57.29577951308232;
  auto const reference_slope
    = reference_terrain.at("max_slope_degrees").get<double>();
  auto const expected_compact_slope = std::atan(
    std::tan(reference_slope * degrees_to_radians) / .32)
    * radians_to_degrees;
  auto canonical_fitted = Noggit::Ai::canonicalMobaArenaBlueprint(fitted);
  auto const& canonical_fitted_terrain
    = callArguments(canonical_fitted, "apply_terrain_layout_on_map");
  require(std::abs(compact_terrain.at("max_slope_degrees").get<double>()
                - expected_compact_slope) < .000001
            && std::abs(canonical_fitted_terrain
                 .at("max_slope_degrees").get<double>() - reference_slope)
              < .000001
            && terrainFeature(canonical_fitted, "arena_ground")
                 .at("transition_width_ratio") == .02,
          "compact slope widening must preserve the horizontal relief mask");
  require(terrainFeature(mutation_baseline, "objective_north").at("priority") == 70
            && terrainFeature(fitted, "objective_north").at("priority") == 91
            && terrainFeature(fitted, "objective_south").at("priority") == 91
            && terrainFeature(fitted, "river_bed").at("priority") == 92,
          "only compact objective pits may override the retained wall relief");
  for (auto const& reference_feature : reference_terrain.at("features"))
  {
    auto const& name = reference_feature.at("name").get_ref<std::string const&>();
    auto const& compact_feature = terrainFeature(fitted, name);
    auto const expected_transition = name == "arena_ground"
      ? std::clamp(fitted_margin * .30, .004, .11)
      : reference_feature.at("transition_width_ratio").get<double>() * .32;
    auto const expected_half_width
      = reference_feature.at("half_width_ratio").get<double>() * .32
        * ((name == "top_lane" || name == "bottom_lane") ? 1.25 : 1.0);
    require(std::abs(compact_feature.at("half_width_ratio").get<double>()
                  - expected_half_width)
                < .000001
              && std::abs(
                compact_feature.at("transition_width_ratio").get<double>()
                - expected_transition)
                < .000001
              && compact_feature.at("points").size()
                == reference_feature.at("points").size(),
            "compact terrain widths must preserve the reference mask");
    for (std::size_t index = 0;
         index < reference_feature.at("points").size(); ++index)
    {
      auto const& reference_point = reference_feature.at("points")[index];
      auto const& compact_point = compact_feature.at("points")[index];
      auto const expected_u = name == "arena_ground"
        ? reference_point.at("u").get<double>()
        : .5 + (reference_point.at("u").get<double>() - .5) * .32;
      auto const expected_v = name == "arena_ground"
        ? reference_point.at("v").get<double>()
        : .5 + (reference_point.at("v").get<double>() - .5) * .32;
      require(std::abs(compact_point.at("u").get<double>() - expected_u)
                  < .000001
                && std::abs(compact_point.at("v").get<double>() - expected_v)
                  < .000001
                && std::abs(compact_point.at("height").get<double>()
                    - reference_point.at("height").get<double>()) < .000001,
              "compact terrain points must be an exact horizontal homothety");
    }
  }
  auto const& reference_vegetation
    = callArguments(mutation_baseline, "scatter_assets_on_map", 1);
  auto const& fitted_vegetation
    = callArguments(fitted, "scatter_assets_on_map", 1);
  require(reference_vegetation.at("regions").size()
              == fitted_vegetation.at("regions").size()
            && reference_vegetation.at("exclusions").size()
              == fitted_vegetation.at("exclusions").size(),
          "the compact arena lost vegetation regions or exclusions");
  for (std::size_t index = 0;
       index < reference_vegetation.at("regions").size(); ++index)
    require(std::abs(
      fitted_vegetation.at("regions")[index].at("min_spacing_ratio").get<double>()
      - reference_vegetation.at("regions")[index]
          .at("min_spacing_ratio").get<double>() * .32) < .000001,
      "compact vegetation spacing must preserve the arena scale");
  for (std::size_t index = 0;
       index < reference_vegetation.at("exclusions").size(); ++index)
    require(std::abs(
      fitted_vegetation.at("exclusions")[index]
          .at("half_width_ratio").get<double>()
      - reference_vegetation.at("exclusions")[index]
          .at("half_width_ratio").get<double>() * .32) < .000001,
      "compact vegetation exclusions must preserve the arena scale");
  auto requireEffectiveCore = [&](std::string_view name)
  {
    auto const& layout = *fitted_terrain.layout;
    auto const feature = std::find_if(
      layout.features.begin(), layout.features.end(),
      [&](auto const& value) { return value.name == name; });
    require(feature != layout.features.end(),
            "the compact terrain feature is missing");
    auto const index = static_cast<std::size_t>(
      std::distance(layout.features.begin(), feature));
    auto min_u = 1.0f;
    auto max_u = 0.0f;
    auto min_v = 1.0f;
    auto max_v = 0.0f;
    for (auto const& point : feature->points)
    {
      min_u = std::min(min_u, point.u);
      max_u = std::max(max_u, point.u);
      min_v = std::min(min_v, point.v);
      max_v = std::max(max_v, point.v);
    }
    auto has_core = false;
    for (auto y = 0; y < 128 && !has_core; ++y)
      for (auto x = 0; x < 128 && !has_core; ++x)
      {
        auto const u = min_u - .01f
          + (max_u - min_u + .02f) * (x + .5f) / 128.0f;
        auto const v = min_v - .01f
          + (max_v - min_v + .02f) * (y + .5f) / 128.0f;
        has_core = Noggit::Ai::sampleProceduralLayout(
          layout, u, v, 20.0f, 0.0f,
          3200.0f / 3.0f, 3200.0f / 3.0f).feature_masks[index] >= .999f;
      }
    if (!has_core)
      throw std::runtime_error(
        "compact terrain feature lost its effective core: "
        + std::string{name});
  };
  requireEffectiveCore("jungle_1_clear_path");
  requireEffectiveCore("jungle_1_branch_path");
  requireEffectiveCore("jungle_2_clear_path");
  requireEffectiveCore("jungle_2_branch_path");
  requireEffectiveCore("jungle_3_clear_path");
  requireEffectiveCore("jungle_3_branch_path");
  requireEffectiveCore("jungle_4_clear_path");
  requireEffectiveCore("jungle_4_branch_path");
  requireEffectiveCore("objective_north");
  requireEffectiveCore("objective_south");
  requireEffectiveCore("jungle_36_wall_mass");
  requireEffectiveCore("jungle_45_wall_mass");
  auto const& compact_river = fitted_liquid.layout->features.front();
  auto const reference_liquid = Noggit::Ai::parseProceduralLiquidLayout(
    callArguments(mutation_baseline, "apply_liquid_layout_on_map"));
  require(reference_liquid.layout.has_value()
            && std::abs(compact_river.points.front().height
                  - reference_liquid.layout->features.front().points.front().height)
              < .0001f
            && std::abs(compact_river.half_width_ratio
                  - reference_liquid.layout->features.front().half_width_ratio * .32f)
              < .000001f
            && std::abs(compact_river.transition_width_ratio
                  - reference_liquid.layout->features.front()
                      .transition_width_ratio * .32f) < .000001f,
          "the compact river must preserve the reference mask and height");
  auto compact_min_u = 1.0f;
  auto compact_max_u = 0.0f;
  auto compact_min_v = 1.0f;
  auto compact_max_v = 0.0f;
  for (auto const& point : compact_river.points)
  {
    compact_min_u = std::min(compact_min_u, point.u);
    compact_max_u = std::max(compact_max_u, point.u);
    compact_min_v = std::min(compact_min_v, point.v);
    compact_max_v = std::max(compact_max_v, point.v);
  }
  require(compact_max_u - compact_min_u
              + 2.0f * compact_river.half_width_ratio
              >= Noggit::Ai::moba_arena_minimum_liquid_span_u
            && compact_max_v - compact_min_v
              + 2.0f * compact_river.half_width_ratio
              >= Noggit::Ai::moba_arena_minimum_liquid_span_v,
          "the compact river no longer satisfies its runtime span contract");
  constexpr auto compact_world_size = 3200.0f / 3.0f;
  auto mountain_scene_layout = *fitted_terrain.layout;
  mountain_scene_layout.features.erase(std::remove_if(
    mountain_scene_layout.features.begin(), mountain_scene_layout.features.end(),
    [](Noggit::Ai::ProceduralLayoutFeature const& value)
    {
      return value.name != "arena_ground"
        && value.name != "arena_mountain_mass"
        && value.name != "arena_perimeter_relief";
    }),
    mountain_scene_layout.features.end());
  require(mountain_scene_layout.features.size() == 3,
          "the fitted terrain lost its mountain scene");
  auto mountain_layout = mountain_scene_layout;
  mountain_layout.features.erase(std::remove_if(
    mountain_layout.features.begin(), mountain_layout.features.end(),
    [](Noggit::Ai::ProceduralLayoutFeature const& value)
    {
      return value.name != "arena_mountain_mass"
        && value.name != "arena_perimeter_relief";
    }),
    mountain_layout.features.end());
  require(mountain_layout.features.size() == 2,
          "the fitted terrain lost one of its mountain arcs");
  auto const sampleHeight = [&](Noggit::Ai::ProceduralLayout const& layout,
                                float u, float v)
  {
    return Noggit::Ai::sampleSmoothedProceduralLayoutHeight(
      layout, u, v, 20.0f,
      compact_world_size, compact_world_size,
      compact_world_size / 512.0f) - 20.0f;
  };
  auto const sampleSceneDelta = [&](float u, float v)
  { return sampleHeight(mountain_scene_layout, u, v); };
  auto const sampleRidgeDelta = [&](float u, float v)
  { return sampleHeight(mountain_layout, u, v); };
  auto lane_mountain_intrusion_max = 0.0f;
  for (auto const name : {"top_lane", "middle_lane", "bottom_lane"})
  {
    auto const lane = std::find_if(
      fitted_terrain.layout->features.begin(),
      fitted_terrain.layout->features.end(),
      [&](auto const& feature) { return feature.name == name; });
    require(lane != fitted_terrain.layout->features.end(),
            "the fitted terrain lost a lane");
    auto const clearance
      = lane->half_width_ratio + lane->transition_width_ratio + .006f;
    for (std::size_t segment = 0; segment + 1 < lane->points.size(); ++segment)
    {
      auto const& first = lane->points[segment];
      auto const& second = lane->points[segment + 1];
      auto const du = second.u - first.u;
      auto const dv = second.v - first.v;
      auto const length = std::hypot(du, dv);
      auto const steps = std::max(
        1, static_cast<int>(std::ceil(length / .002f)));
      for (auto step = 0; step <= steps; ++step)
      {
        auto const along = static_cast<float>(step) / steps;
        auto const u = first.u + du * along;
        auto const v = first.v + dv * along;
        for (auto const side : {-1.0f, 0.0f, 1.0f})
          lane_mountain_intrusion_max = std::max(
            lane_mountain_intrusion_max,
            sampleRidgeDelta(
              u - dv / length * clearance * side,
              v + du / length * clearance * side));
      }
    }
  }

  std::vector<float> ridge_heights;
  std::vector<float> contact_distances;
  std::vector<float> ridge_distances;
  std::vector<float> crest_clearances;
  std::vector<float> face_slopes;
  std::vector<float> rocky_face_flags;
  auto rocky_faces = 0;
  auto mixed_shoulders = 0;
  auto collectCrestClearances = [&](nlohmann::json const& arc)
  {
    for (auto const& arc_point : arc.at("points"))
    {
      auto const u = arc_point.at("u").get<float>();
      auto const v = arc_point.at("v").get<float>();
      auto const outside_u = std::max({
        static_cast<float>(fitted_margin) - u,
        u - static_cast<float>(1.0 - fitted_margin), 0.0f});
      auto const outside_v = std::max({
        static_cast<float>(fitted_margin) - v,
        v - static_cast<float>(1.0 - fitted_margin), 0.0f});
      crest_clearances.push_back(
        std::hypot(outside_u, outside_v) * compact_world_size);
    }
  };
  collectCrestClearances(mountain_mass);
  collectCrestClearances(perimeter);
  for (auto side = 0; side < 4; ++side)
  {
    for (auto sample = 0; sample < 64; ++sample)
    {
      auto const along = static_cast<float>(
        fitted_margin + fitted_scale * (sample + .5) / 64.0);
      auto best_height = std::numeric_limits<float>::lowest();
      auto best_distance = 0.0f;
      auto contact = std::numeric_limits<float>::infinity();
      std::array<float, 97> profile_heights{};
      std::array<float, 97> profile_u{};
      std::array<float, 97> profile_v{};
      for (auto depth = 0; depth <= 96; ++depth)
      {
        auto const distance
          = static_cast<float>(fitted_margin) * depth / 96.0f;
        auto u = along;
        auto v = along;
        if (side == 0) { u = static_cast<float>(fitted_margin) - distance; }
        if (side == 1) { v = static_cast<float>(fitted_margin) - distance; }
        if (side == 2) { u = 1.0f - static_cast<float>(fitted_margin) + distance; }
        if (side == 3) { v = 1.0f - static_cast<float>(fitted_margin) + distance; }
        auto const scene_height = sampleSceneDelta(u, v);
        auto const ridge_height = sampleRidgeDelta(u, v);
        profile_heights[depth] = scene_height;
        profile_u[depth] = u;
        profile_v[depth] = v;
        if (!std::isfinite(contact) && scene_height >= 1.0f)
          contact = distance * compact_world_size;
        if (ridge_height > best_height)
        {
          best_height = ridge_height;
          best_distance = distance * compact_world_size;
        }
      }
      contact_distances.push_back(contact);
      ridge_distances.push_back(best_distance);
      ridge_heights.push_back(best_height);
      auto ray_has_rocky_face = false;
      auto ray_has_mixed_face = false;
      auto ray_max_slope = 0.0f;
      constexpr auto slope_span = 4;
      auto const slope_run = 2.0f * slope_span
        * static_cast<float>(fitted_margin) / 96.0f
        * compact_world_size;
      for (auto depth = slope_span; depth <= 96 - slope_span; ++depth)
      {
        auto const rise = profile_heights[depth + slope_span]
          - profile_heights[depth - slope_span];
        auto const middle = profile_heights[depth];
        if (rise <= 0.0f || middle <= best_height * .08f
            || middle >= best_height * .92f)
          continue;
        auto const slope = std::atan(rise / slope_run)
          * 180.0f / static_cast<float>(std::numbers::pi);
        ray_max_slope = std::max(ray_max_slope, slope);
        auto const face = Noggit::Ai::sampleProceduralLayout(
          mountain_scene_layout, profile_u[depth], profile_v[depth],
          20.0f, slope, compact_world_size, compact_world_size);
        ray_has_rocky_face = ray_has_rocky_face
          || (slope >= 52.0f && face.quantized_weights[3] >= 160);
        ray_has_mixed_face = ray_has_mixed_face
          || (face.quantized_weights[3] > 0
            && (face.quantized_weights[0] > 0
              || face.quantized_weights[2] > 0));
      }
      rocky_faces += ray_has_rocky_face;
      rocky_face_flags.push_back(ray_has_rocky_face ? 1.0f : 0.0f);
      mixed_shoulders += ray_has_mixed_face;
      if (ray_max_slope > 0.0f) face_slopes.push_back(ray_max_slope);
    }
  }
  auto circularOrder = [](std::vector<float> const& side_order)
  {
    auto result = std::vector<float>{};
    result.reserve(side_order.size());
    for (auto index = 0; index < 64; ++index)
      result.push_back(side_order[64 + index]);
    for (auto index = 0; index < 64; ++index)
      result.push_back(side_order[128 + index]);
    for (auto index = 0; index < 64; ++index)
      result.push_back(side_order[192 + 63 - index]);
    for (auto index = 0; index < 64; ++index)
      result.push_back(side_order[63 - index]);
    return result;
  };
  auto const ordered_ridge_heights = circularOrder(ridge_heights);
  auto const ordered_rocky_face_flags = circularOrder(rocky_face_flags);
  auto sorted_ridge_distances = ridge_distances;
  std::sort(sorted_ridge_distances.begin(), sorted_ridge_distances.end());
  std::sort(ridge_heights.begin(), ridge_heights.end());
  std::sort(face_slopes.begin(), face_slopes.end());
  auto const ridge_p10 = ridge_heights[ridge_heights.size() / 10];
  auto const ridge_p90 = ridge_heights[ridge_heights.size() * 9 / 10];
  auto low_saddle_faces = 0;
  auto low_saddle_rocky_faces = 0;
  for (std::size_t index = 0; index < ordered_ridge_heights.size(); ++index)
  {
    if (ordered_ridge_heights[index] > ridge_p10)
      continue;
    ++low_saddle_faces;
    low_saddle_rocky_faces += ordered_rocky_face_flags[index] > 0.0f;
  }
  auto const ridge_distance_p90
    = sorted_ridge_distances[sorted_ridge_distances.size() * 9 / 10];
  auto prominent_peak_count = 0;
  auto peaks_by_side = std::array<int, 4>{};
  constexpr auto peak_window = std::size_t{6};
  for (std::size_t index = 0; index < ordered_ridge_heights.size(); ++index)
  {
    auto left_min = ordered_ridge_heights[index];
    auto right_min = ordered_ridge_heights[index];
    auto local_maximum = true;
    for (std::size_t offset = 1; offset <= peak_window; ++offset)
    {
      auto const left = ordered_ridge_heights[
        (index + ordered_ridge_heights.size() - offset)
          % ordered_ridge_heights.size()];
      auto const right = ordered_ridge_heights[
        (index + offset) % ordered_ridge_heights.size()];
      left_min = std::min(left_min, left);
      right_min = std::min(right_min, right);
      local_maximum = local_maximum
        && ordered_ridge_heights[index] > left
        && ordered_ridge_heights[index] >= right;
    }
    if (!local_maximum
        || ordered_ridge_heights[index] - std::max(left_min, right_min) < 12.0f)
      continue;
    ++prominent_peak_count;
    ++peaks_by_side[index / 64];
  }
  auto const high_crest_threshold
    = ridge_p10 + .65f * (ridge_p90 - ridge_p10);
  auto longest_high_crest_run = std::size_t{0};
  auto high_crest_run = std::size_t{0};
  for (std::size_t index = 0;
       index < ordered_ridge_heights.size() * 2; ++index)
  {
    if (ordered_ridge_heights[index % ordered_ridge_heights.size()]
        >= high_crest_threshold)
      high_crest_run = std::min(
        ordered_ridge_heights.size(), high_crest_run + 1);
    else
      high_crest_run = 0;
    longest_high_crest_run
      = std::max(longest_high_crest_run, high_crest_run);
  }
  require(!face_slopes.empty(), "the mountain slope probe found no rising face");
  auto const slope_p50 = face_slopes[face_slopes.size() / 2];
  auto const slope_p90 = face_slopes[face_slopes.size() * 9 / 10];
  auto outer_min = std::numeric_limits<float>::max();
  auto outer_max = std::numeric_limits<float>::lowest();
  for (auto y = 0; y <= 48; ++y)
    for (auto x = 0; x <= 48; ++x)
    {
      auto const u = static_cast<float>(x) / 48.0f;
      auto const v = static_cast<float>(y) / 48.0f;
      if (u > fitted_margin && u < 1.0 - fitted_margin
          && v > fitted_margin && v < 1.0 - fitted_margin)
        continue;
      auto const height = Noggit::Ai::sampleProceduralLayout(
        mountain_scene_layout, u, v, 20.0f, 0.0f,
        compact_world_size, compact_world_size).height;
      outer_min = std::min(outer_min, height);
      outer_max = std::max(outer_max, height);
    }
  auto const contact_max
    = *std::max_element(contact_distances.begin(), contact_distances.end());
  auto const ridge_distance_max
    = *std::max_element(ridge_distances.begin(), ridge_distances.end());
  auto const crest_clearance_max
    = *std::max_element(crest_clearances.begin(), crest_clearances.end());
  auto corner_relief_min = std::numeric_limits<float>::max();
  for (auto const& corner : std::array<std::array<float, 2>, 4>{{
         {{.02f, .02f}}, {{.98f, .02f}}, {{.98f, .98f}}, {{.02f, .98f}}}})
    corner_relief_min = std::min(
      corner_relief_min, sampleSceneDelta(corner[0], corner[1]));
  auto const mountain_ok
    = std::all_of(contact_distances.begin(), contact_distances.end(),
        [](float distance)
        { return std::isfinite(distance) && distance <= 55.0f; })
      && std::all_of(ridge_distances.begin(), ridge_distances.end(),
           [](float distance) { return distance <= 180.0f; })
      && contact_max >= 4.0f
      && ridge_distance_p90 <= 165.0f
      && crest_clearance_max <= 110.0f
      && ridge_heights.front() >= 45.0f
      && ridge_p90 - ridge_p10 >= 70.0f
      && slope_p50 >= 45.0f && slope_p90 <= 82.0f
      && outer_max - outer_min >= 70.0f
      && prominent_peak_count >= 6
      && std::all_of(peaks_by_side.begin(), peaks_by_side.end(),
           [](int count) { return count >= 1; })
      && longest_high_crest_run <= 68
      && mixed_shoulders >= 120
      && rocky_faces >= 160
      && low_saddle_rocky_faces * 5 <= low_saddle_faces * 4
      && lane_mountain_intrusion_max <= 1.0f;
  if (dump_audit)
  {
    auto const preview
      = dump_artifacts / "moba-arena-mountains.actual.png";
    writeMountainPreview(
      *fitted_terrain.layout, preview, 20.0f, compact_world_size);
    std::cout << nlohmann::json{
      {"format", "noggit.moba-mountain-feedback/v1"},
      {"ok", mountain_ok},
      {"terrain_features",
        callArguments(fitted, "apply_terrain_layout_on_map")
          .at("features").size()},
      {"preview", preview.string()},
      {"rock_texture", texture_paths.at(3)},
      {"contact_distance_max", contact_max},
      {"ridge_distance_max", ridge_distance_max},
      {"ridge_distance_p90", ridge_distance_p90},
      {"crest_clearance_max", crest_clearance_max},
      {"ridge_height", {
        {"min", ridge_heights.front()},
        {"p10", ridge_p10},
        {"p90", ridge_p90},
        {"max", ridge_heights.back()}
      }},
      {"macro_slope_degrees", {
        {"p50", slope_p50},
        {"p90", slope_p90}
      }},
      {"outer_relief_range", outer_max - outer_min},
      {"corner_relief_min", corner_relief_min},
      {"prominent_peak_count", prominent_peak_count},
      {"peaks_by_side", peaks_by_side},
      {"longest_high_crest_run", longest_high_crest_run},
      {"mixed_shoulders", mixed_shoulders},
      {"rocky_faces", rocky_faces},
      {"low_saddle_rocky_faces", low_saddle_rocky_faces},
      {"low_saddle_faces", low_saddle_faces},
      {"lane_mountain_intrusion_max", lane_mountain_intrusion_max},
      {"sampled_faces", 256}
    }.dump(2) << std::endl;
    if (mountain_ok)
    {
      requireAudit(
        Noggit::Ai::auditMobaArenaBlueprint(fitted, 2), dump_artifacts);
      return 0;
    }
  }
  require(mountain_ok,
          "the sampled mountain chain is not close, broad, ridged and rocky");

  for (auto const scale : std::array{.7, .9, .95})
  {
    auto scaled_spec = spec;
    scaled_spec["arena_scale_ratio"] = scale;
    auto scaled = Noggit::Ai::compileMobaArenaBlueprint(scaled_spec, 2);
    require(Noggit::Ai::parseProceduralLayout(
              callArguments(scaled, "apply_terrain_layout_on_map"))
                .layout.has_value(),
            "a supported fitted mountain layout became invalid");
  }
  auto narrow_arena_spec = spec;
  narrow_arena_spec["arena_scale_ratio"] = .99;
  auto narrow_arena
    = Noggit::Ai::compileMobaArenaBlueprint(narrow_arena_spec, 2);
  require(std::none_of(
            callArguments(narrow_arena, "apply_terrain_layout_on_map")
              .at("features").begin(),
            callArguments(narrow_arena, "apply_terrain_layout_on_map")
              .at("features").end(),
            [](nlohmann::json const& value)
            { return value.at("name") == "arena_perimeter_relief"; }),
          "a nearly full-map arena kept a sub-vertex mountain ridge");
  auto near_full_spec = spec;
  near_full_spec["arena_scale_ratio"] = .999999;
  auto near_full
    = Noggit::Ai::compileMobaArenaBlueprint(near_full_spec, 2);
  require(std::none_of(
            callArguments(near_full, "apply_terrain_layout_on_map")
              .at("features").begin(),
            callArguments(near_full, "apply_terrain_layout_on_map")
              .at("features").end(),
            [](nlohmann::json const& value)
            { return value.at("name") == "arena_perimeter_relief"; }),
          "a full-map arena kept a mountain ridge");
  auto high_arena_spec = spec;
  high_arena_spec["base_height"] = 4950;
  auto high_arena
    = Noggit::Ai::compileMobaArenaBlueprint(high_arena_spec, 2);
  auto const& high_perimeter
    = terrainFeature(high_arena, "arena_perimeter_relief");
  auto high_layout = Noggit::Ai::parseProceduralLayout(
    callArguments(high_arena, "apply_terrain_layout_on_map"));
  require(high_layout.layout.has_value(),
          "the high fitted arena produced an invalid layout");
  auto high_min = std::numeric_limits<float>::max();
  auto high_max = std::numeric_limits<float>::lowest();
  for (std::size_t index = 0;
       index + 1 < high_perimeter.at("points").size(); ++index)
  {
    auto const& first = high_perimeter.at("points").at(index);
    auto const& second = high_perimeter.at("points").at(index + 1);
    for (auto step = 0; step < 8; ++step)
    {
      auto const ratio = static_cast<float>(step) / 8.0f;
      auto const sample = Noggit::Ai::sampleProceduralLayout(
        *high_layout.layout,
        first.at("u").get<float>() * (1.0f - ratio)
          + second.at("u").get<float>() * ratio,
        first.at("v").get<float>() * (1.0f - ratio)
          + second.at("v").get<float>() * ratio,
        4950.0f, 0.0f, compact_world_size, compact_world_size);
      high_min = std::min(high_min, sample.height);
      high_max = std::max(high_max, sample.height);
    }
  }
  require(high_min > 4950.0f && high_max <= 5000.0f
            && high_max - high_min >= 3.0f,
          "a high fitted arena flattened or inverted its mountain peaks");
  requireAudit(Noggit::Ai::auditMobaArenaBlueprint(fitted, 2));
  auto fitted_runtime = fitted;
  fitted_runtime.erase("arena_fit");
  auto const runtime_audit
    = Noggit::Ai::auditMobaArenaBlueprint(fitted_runtime, 2);
  auto const runtime_core_samples
    = runtime_audit.metrics.at("feature_cores")
        .at("arena_perimeter_relief")
        .at("effective_core_samples").get<std::size_t>();
  if (runtime_audit.hasIssue("props.unwalkable")
      || runtime_core_samples < 4)
  {
    auto message = std::string{
      "the compact runtime must retain a sampled mountain crest and walkable "
      "props: unwalkable="} + std::to_string(
        runtime_audit.hasIssue("props.unwalkable"))
      + ", core_samples=" + std::to_string(runtime_core_samples);
    for (auto const& issue : runtime_audit.issues)
      if (issue.code == "props.unwalkable")
        message += ", prop=" + issue.subject;
    throw std::runtime_error(message);
  }
  auto const runtime_origin_audit = Noggit::Ai::auditMobaArenaBlueprint(
    fitted_runtime, 2, 64, 26, 25);
  auto const& runtime_vegetation
    = runtime_origin_audit.metrics.at("scatter").at("vegetation");
  require(runtime_vegetation.at("accepted_after_spacing").get<std::size_t>() >= 1
            && runtime_vegetation.at("regions").contains("jungle_20_canopy"),
          "the compact runtime scatter lost its trees");

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
