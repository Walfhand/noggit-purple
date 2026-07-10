// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Noggit::Ai
{
  struct FunctionCall
  {
    std::string call_id;
    std::string name;
    std::string arguments;
  };

  nlohmann::json toolDefinitions();
  std::vector<FunctionCall> functionCalls(nlohmann::json const& response);
  std::string outputText(nlohmann::json const& response);
}
