// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/ProceduralProps.hpp>

#include <nlohmann/json.hpp>

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
    "props":[
      {"name":"team_left_landmark","path":"world/fountain.m2","u":0.113,"v":0.86,
       "scale":1.2,"yaw_degrees":45,"height_offset":0},
      {"name":"team_left_glow","path":"world/noggit/lights/noggit_light_deepskyblue01.m2",
       "u":0.113,"v":0.86,"scale":1.0,"yaw_degrees":0,"height_offset":8}
    ]
  })");
  auto const parsed = Noggit::Ai::parseProceduralProps(specification);
  require(parsed.props.has_value(), "valid props rejected");
  require(parsed.props->props.size() == 2, "prop count changed");
  require(parsed.props->props[0].name == "team_left_landmark"
            && parsed.props->props[0].scale == 1.2f
            && parsed.props->props[1].height_offset == 8.0f,
          "prop fields were not parsed faithfully");

  auto duplicate = specification;
  duplicate["props"][1]["name"] = "team_left_landmark";
  require(!Noggit::Ai::parseProceduralProps(duplicate).props,
          "duplicate prop names accepted");

  auto out_of_bounds = specification;
  out_of_bounds["props"][0]["u"] = 1.5;
  require(!Noggit::Ai::parseProceduralProps(out_of_bounds).props,
          "out-of-bounds coordinates accepted");

  auto bad_yaw = specification;
  bad_yaw["props"][0]["yaw_degrees"] = 360.0;
  require(!Noggit::Ai::parseProceduralProps(bad_yaw).props,
          "yaw of 360 degrees accepted");

  auto extra = specification;
  extra["props"][0]["extra"] = true;
  require(!Noggit::Ai::parseProceduralProps(extra).props,
          "strict nested validation accepted an extra field");

  auto extra_root = specification;
  extra_root["seed"] = "x";
  require(!Noggit::Ai::parseProceduralProps(extra_root).props,
          "extra root field accepted");

  auto too_many = nlohmann::json{{"props", nlohmann::json::array()}};
  for (int index = 0; index < 257; ++index)
    too_many["props"].push_back({
      {"name", "prop_" + std::to_string(index)}, {"path", "world/p.m2"},
      {"u", .5}, {"v", .5}, {"scale", 1.0},
      {"yaw_degrees", 0.0}, {"height_offset", 0.0}});
  require(!Noggit::Ai::parseProceduralProps(too_many).props,
          "more than 256 props accepted");
}
