// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AiProtocol.hpp>

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
}

int main()
{
  auto const tools = Noggit::Ai::toolDefinitions();
  require(tools.is_array(), "tools must be an array");
  require(tools.size() == 5, "unexpected tool count");

  std::set<std::string> tool_names;

  for (auto const& tool : tools)
  {
    tool_names.insert(tool.at("name").get<std::string>());
    require(tool.at("type") == "function", "tool type must be function");
    require(tool.at("strict") == true, "tool must use strict mode");
    auto const& parameters = tool.at("parameters");
    require(parameters.at("type") == "object", "parameters must be an object");
    require(parameters.at("additionalProperties") == false, "extra properties must be rejected");

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
  require(tool_names.count("search_textures") == 1, "search_textures tool is missing");

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
