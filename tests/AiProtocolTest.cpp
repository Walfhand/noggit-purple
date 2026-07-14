// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AiProtocol.hpp>

#include <cmath>
#include <stdexcept>
#include <set>
#include <string>

namespace
{
  void require(bool condition, char const* message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  void requireStrictSchema(nlohmann::json const& schema)
  {
    if (schema.value("type", nlohmann::json{}) == "object")
    {
      require(schema.value("additionalProperties", true) == false,
              "strict object must reject extra properties");
      std::set<std::string> properties;
      for (auto const& [name, value] : schema.at("properties").items())
      {
        properties.insert(name);
        requireStrictSchema(value);
      }
      std::set<std::string> required;
      for (auto const& name : schema.at("required"))
      {
        required.insert(name.get<std::string>());
      }
      require(properties == required, "strict object requires every property");
    }
    if (schema.contains("items"))
    {
      requireStrictSchema(schema.at("items"));
    }
  }
}

int main()
{
  auto const tools = Noggit::Ai::toolDefinitions();
  require(tools.is_array(), "tools must be an array");
  require(tools.size() == 20, "unexpected tool count");

  std::set<std::string> tool_names;
  nlohmann::json const* terrain_layout = nullptr;
  nlohmann::json const* liquid_layout = nullptr;
  nlohmann::json const* texture_search = nullptr;
  nlohmann::json const* texture_preview = nullptr;
  nlohmann::json const* asset_scatter = nullptr;

  for (auto const& tool : tools)
  {
    tool_names.insert(tool.at("name").get<std::string>());
    if (tool.at("name") == "apply_terrain_layout_on_map")
    {
      terrain_layout = &tool;
    }
    if (tool.at("name") == "apply_liquid_layout_on_map")
    {
      liquid_layout = &tool;
    }
    if (tool.at("name") == "search_textures")
    {
      texture_search = &tool;
    }
    if (tool.at("name") == "preview_textures")
    {
      texture_preview = &tool;
    }
    if (tool.at("name") == "scatter_assets_on_map") asset_scatter = &tool;
    require(tool.at("type") == "function", "tool type must be function");
    require(tool.at("strict") == true, "tool must use strict mode");
    auto const& parameters = tool.at("parameters");
    require(parameters.at("type") == "object", "parameters must be an object");
    require(parameters.at("additionalProperties") == false, "extra properties must be rejected");
    requireStrictSchema(parameters);

    std::set<std::string> properties;
    for (auto const& [name, value] : parameters.at("properties").items())
    {
      static_cast<void>(value);
      properties.insert(name);
    }
    std::set<std::string> required;
    for (auto const& name : parameters.at("required"))
    {
      required.insert(name.get<std::string>());
    }
    require(properties == required, "strict mode requires every property");
  }
  require(tool_names.count("paint_texture") == 1, "paint_texture tool is missing");
  require(tool_names.count("set_base_texture_on_loaded_tiles") == 1,
          "set_base_texture_on_loaded_tiles tool is missing");
  require(tool_names.count("submit_map_plan") == 1, "submit_map_plan tool is missing");
  require(tool_names.count("inspect_map") == 1, "inspect_map tool is missing");
  require(tool_names.count("generate_terrain_on_map") == 1,
          "generate_terrain_on_map tool is missing");
  require(tool_names.count("apply_terrain_layout_on_map") == 1,
          "apply_terrain_layout_on_map tool is missing");
  require(tool_names.count("apply_liquid_layout_on_map") == 1,
          "apply_liquid_layout_on_map tool is missing");
  require(tool_names.count("set_base_texture_on_map") == 1,
          "set_base_texture_on_map tool is missing");
  require(tool_names.count("blend_terrain_textures_on_map") == 1,
          "blend_terrain_textures_on_map tool is missing");
  require(tool_names.count("validate_map") == 1, "validate_map tool is missing");
  require(tool_names.count("inspect_map_view") == 1, "inspect_map_view tool is missing");
  require(tool_names.count("search_textures") == 1, "search_textures tool is missing");
  require(tool_names.count("preview_textures") == 1, "preview_textures tool is missing");
  require(tool_names.count("search_assets") == 1, "search_assets tool is missing");
  require(tool_names.count("scatter_assets_on_map") == 1,
          "scatter_assets_on_map tool is missing");
  require(tool_names.count("create_moba_arena_blueprint") == 1,
          "create_moba_arena_blueprint tool is missing");
  require(tool_names.count("apply_ground_effect_on_map") == 1,
          "apply_ground_effect_on_map tool is missing");
  require(tool_names.count("search_ground_effects") == 1,
          "search_ground_effects tool is missing");

  require(asset_scatter != nullptr, "asset scatter schema is missing");
  auto const& scatter_properties = asset_scatter->at("parameters").at("properties");
  require(scatter_properties.at("assets").at("minItems") == 1
            && scatter_properties.at("assets").at("maxItems") == 16
            && scatter_properties.at("regions").at("maxItems") == 16
            && scatter_properties.at("exclusions").at("maxItems") == 32,
          "asset scatter bounds changed");

  require(terrain_layout != nullptr, "terrain layout schema is missing");
  auto const& layout_properties = terrain_layout->at("parameters").at("properties");
  require(layout_properties.at("texture_paths").at("minItems") == 2
            && layout_properties.at("texture_paths").at("maxItems") == 16,
          "terrain layout texture count changed");
  require(layout_properties.at("features").at("minItems") == 1
            && layout_properties.at("features").at("maxItems") == 32,
          "terrain layout feature count changed");
  require(layout_properties.at("steep_texture_layer").at("type")
            == nlohmann::json::array({"integer", "null"})
            && layout_properties.at("slope_start_degrees").at("type")
            == nlohmann::json::array({"number", "null"})
            && layout_properties.at("slope_full_degrees").at("type")
            == nlohmann::json::array({"number", "null"}),
          "terrain layout steep parameters must stay nullable");
  require(layout_properties.at("edge_noise_ratio").at("minimum") == 0.0
            && layout_properties.at("edge_noise_ratio").at("maximum") == 0.05
            && layout_properties.at("max_slope_degrees").at("type")
              == nlohmann::json::array({"number", "null"})
            && layout_properties.at("max_slope_degrees").at("minimum") == 5.0
            && layout_properties.at("max_slope_degrees").at("maximum") == 60.0
            && layout_properties.at("smoothing_strength").at("minimum") == 0.0
            && layout_properties.at("smoothing_strength").at("maximum") == 1.0,
          "terrain layout naturalization bounds changed");
  auto const& feature_properties = layout_properties.at("features")
    .at("items").at("properties");
  require(feature_properties.at("shape").at("enum")
            == nlohmann::json::array({"corridor", "area"})
            && feature_properties.at("height_mode").at("enum")
              == nlohmann::json::array({"absolute", "offset"})
            && feature_properties.at("texture_layer").at("maximum") == 15
            && feature_properties.at("roughness_amplitude").at("minimum") == 0.0
            && feature_properties.at("roughness_amplitude").at("maximum") == 100.0
            && feature_properties.at("texture_strength").at("minimum") == 0.05
            && feature_properties.at("texture_strength").at("maximum") == 1.0
            && feature_properties.at("width_variation_ratio").at("minimum") == 0.0
            && feature_properties.at("width_variation_ratio").at("maximum") == 0.75,
          "terrain layout feature modes changed");

  require(texture_search != nullptr, "texture search schema is missing");
  auto const& search_properties = texture_search->at("parameters").at("properties");
  require(search_properties.at("offset").at("minimum") == 0
            && search_properties.at("offset").at("maximum") == 1000000,
          "texture search pagination changed");

  require(texture_preview != nullptr, "texture preview schema is missing");
  auto const& preview_paths = texture_preview->at("parameters")
    .at("properties").at("texture_paths");
  require(preview_paths.at("minItems") == 1 && preview_paths.at("maxItems") == 12,
          "texture preview count changed");

  require(liquid_layout != nullptr, "liquid layout schema is missing");
  auto const& liquid_properties = liquid_layout->at("parameters").at("properties");
  require(liquid_properties.at("replace_existing").at("type") == "boolean"
            && liquid_properties.at("edge_noise_ratio").at("minimum") == 0.0
            && liquid_properties.at("edge_noise_ratio").at("maximum") == 0.05,
          "liquid layout root contract changed");
  require(liquid_properties.at("features").at("minItems") == 1
            && liquid_properties.at("features").at("maxItems") == 32,
          "liquid layout feature count changed");
  auto const& liquid_feature_properties = liquid_properties.at("features")
    .at("items").at("properties");
  require(liquid_feature_properties.at("shape").at("enum")
            == nlohmann::json::array({"corridor", "area"})
            && liquid_feature_properties.at("liquid_type_id").at("minimum") == 1
            && liquid_feature_properties.at("liquid_type_id").at("maximum") == 65535
            && liquid_feature_properties.at("depth").at("minimum") == 0.01
            && liquid_feature_properties.at("depth").at("maximum") == 1.0,
          "liquid layout feature contract changed");

  require(std::abs(Noggit::Ai::terrainRatio("plains", -1.0f, 1.0f) - 0.4f) < 0.0001f,
          "plains terrain ratio changed");
  require(std::abs(Noggit::Ai::terrainRatio("island", -1.0f, 0.0f) - 0.05f) < 0.0001f
            && std::abs(Noggit::Ai::terrainRatio("island", 1.0f, 0.0f) - 0.05f) < 0.0001f,
          "island coast must stay flat");
  require(std::abs(Noggit::Ai::terrainRatio("island", -1.0f, 1.0f) - 0.62f) < 0.0001f
            && std::abs(Noggit::Ai::terrainRatio("island", 0.0f, 1.0f) - 0.70f) < 0.0001f
            && std::abs(Noggit::Ai::terrainRatio("island", 1.0f, 1.0f) - 0.78f) < 0.0001f,
          "island hills must stay moderate");

  auto const low = Noggit::Ai::textureBlendAlphas(
    -20.0f, 0.0f, 0.0f, 0.0f, 80.0f, true, 10.0f, 25.0f, 50.0f, 0.5f);
  auto const steep = Noggit::Ai::textureBlendAlphas(
    40.0f, 60.0f, 0.0f, 0.0f, 80.0f, true, 10.0f, 25.0f, 50.0f, 0.5f);
  auto const high = Noggit::Ai::textureBlendAlphas(
    100.0f, 0.0f, 0.0f, 0.0f, 80.0f, true, 10.0f, 25.0f, 50.0f, 0.5f);
  auto const mixed = Noggit::Ai::textureBlendAlphas(
    4.0f, 37.0f, 0.35f, 0.0f, 80.0f, true, 10.0f, 25.0f, 50.0f, 0.8f);
  auto sum = [](auto const& weights)
  {
    return weights[0] + weights[1] + weights[2] + weights[3];
  };
  require(low[1] == 255 && steep[2] == 255 && high[3] == 255,
          "procedural texture roles changed");
  require(sum(low) == 255 && sum(steep) == 255
            && sum(high) == 255 && sum(mixed) == 255,
          "procedural texture alpha must sum to 255");
  require(mixed[0] > 0 && mixed[1] > 0 && mixed[2] > 0,
          "procedural transition must really mix visible layers");

  auto const response = nlohmann::json{
    {"output", {
      {{"type", "reasoning"}, {"encrypted_content", "fixture"}},
      {
        {"type", "function_call"},
        {"id", "fc_ignored"},
        {"call_id", "call_123"},
        {"name", "change_terrain_height"},
        {"arguments", R"({"x":100,"z":200,"delta":2,"radius":20,"falloff":"smooth","inner_radius":0.25})"}
      },
      {
        {"type", "message"},
        {"content", {
          {{"type", "output_text"}, {"text", "Le terrain a été modifié."}}
        }}
      }
    }}
  };

  auto const calls = Noggit::Ai::functionCalls(response);
  require(calls.size() == 1, "function call was not extracted");
  require(calls.front().call_id == "call_123", "call_id must be used for correlation");
  require(calls.front().name == "change_terrain_height", "wrong function name");
  require(Noggit::Ai::outputText(response) == "Le terrain a été modifié.", "output text was not extracted");
}
