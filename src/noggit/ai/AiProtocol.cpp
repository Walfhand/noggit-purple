// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AiProtocol.hpp>

#include <utility>

namespace Noggit::Ai
{
  namespace
  {
    bool readString(nlohmann::json const& object, char const* key, std::string& value)
    {
      auto const field = object.find(key);
      if (field == object.end() || !field->is_string())
      {
        return false;
      }

      value = field->get<std::string>();
      return true;
    }
  }

  nlohmann::json toolDefinitions()
  {
    auto context_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", nlohmann::json::object()},
      {"required", nlohmann::json::array()},
      {"additionalProperties", false}
    };

    auto terrain_properties = nlohmann::json::object();
    terrain_properties["x"] = {
      {"type", "number"},
      {"description", "Coordonnée monde X du centre du pinceau."},
      {"minimum", 0.0},
      {"maximum", 34133.34}
    };
    terrain_properties["z"] = {
      {"type", "number"},
      {"description", "Coordonnée monde Z du centre du pinceau."},
      {"minimum", 0.0},
      {"maximum", 34133.34}
    };
    terrain_properties["delta"] = {
      {"type", "number"},
      {"description", "Variation de hauteur. Positive pour relever, négative pour abaisser."},
      {"minimum", -50.0},
      {"maximum", 50.0}
    };
    terrain_properties["radius"] = {
      {"type", "number"},
      {"description", "Rayon du pinceau en unités monde."},
      {"minimum", 5.0},
      {"maximum", 200.0}
    };
    terrain_properties["falloff"] = {
      {"type", "string"},
      {"description", "Profil d'atténuation du pinceau."},
      {"enum", {"flat", "linear", "smooth", "gaussian"}}
    };
    terrain_properties["inner_radius"] = {
      {"type", "number"},
      {"description", "Part du rayon qui reçoit la pleine intensité, entre 0 et 1."},
      {"minimum", 0.0},
      {"maximum", 1.0}
    };

    auto terrain_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", std::move(terrain_properties)},
      {"required", {"x", "z", "delta", "radius", "falloff", "inner_radius"}},
      {"additionalProperties", false}
    };

    auto texture_properties = nlohmann::json::object();
    texture_properties["x"] = terrain_parameters["properties"]["x"];
    texture_properties["z"] = terrain_parameters["properties"]["z"];
    texture_properties["texture_path"] = {
      {"type", "string"},
      {"description", "Chemin WoW de la texture BLP sous tileset/. Utilise selected_texture retourné par get_editor_context quand l'utilisateur parle de la texture sélectionnée."}
    };
    texture_properties["radius"] = terrain_parameters["properties"]["radius"];
    texture_properties["hardness"] = {
      {"type", "number"},
      {"description", "Part dure du pinceau, entre 0 et 1."},
      {"minimum", 0.0},
      {"maximum", 1.0}
    };
    texture_properties["opacity"] = {
      {"type", "number"},
      {"description", "Opacité cible de la texture, strictement positive et au plus égale à 1."},
      {"minimum", 0.01},
      {"maximum", 1.0}
    };

    auto texture_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", std::move(texture_properties)},
      {"required", {"x", "z", "texture_path", "radius", "hardness", "opacity"}},
      {"additionalProperties", false}
    };

    auto search_properties = nlohmann::json::object();
    search_properties["query"] = {
      {"type", "string"},
      {"description", "Texte à chercher dans le chemin WoW de la texture. Une chaîne vide liste les premières textures."},
      {"maxLength", 128}
    };
    search_properties["limit"] = {
      {"type", "integer"},
      {"description", "Nombre maximal de textures à retourner."},
      {"minimum", 1},
      {"maximum", 100}
    };

    auto search_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", std::move(search_properties)},
      {"required", {"query", "limit"}},
      {"additionalProperties", false}
    };

    auto loaded_tiles_texture_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"texture_path", texture_parameters["properties"]["texture_path"]}
      }},
      {"required", {"texture_path"}},
      {"additionalProperties", false}
    };

    return nlohmann::json::array({
      {
        {"type", "function"},
        {"name", "get_editor_context"},
        {"description", "Lit la carte ouverte, la caméra, le curseur, la sélection et l'état de chargement. À appeler avant une modification si la position est implicite."},
        {"parameters", std::move(context_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "change_terrain_height"},
        {"description", "Relève ou abaisse le terrain avec un pinceau. La modification est ajoutée à l'historique Annuler de Noggit. Seules les tuiles déjà chargées sont acceptées."},
        {"parameters", std::move(terrain_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "paint_texture"},
        {"description", "Peint une texture de terrain BLP avec un coup de pinceau. La modification est ajoutée à l'historique Annuler de Noggit. Seules les tuiles déjà chargées sont acceptées."},
        {"parameters", std::move(texture_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "set_base_texture_on_loaded_tiles"},
        {"description", "Remplace toutes les couches de texture de chacun des 256 chunks de chaque tuile chargée par une seule texture de base. À utiliser quand l'utilisateur demande toute la carte, toutes les tuiles chargées ou tous les chunks. L'opération est annulable en une fois."},
        {"parameters", std::move(loaded_tiles_texture_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_textures"},
        {"description", "Cherche les textures de terrain BLP connues dans la listfile du client. À utiliser avant paint_texture quand aucun chemin exact ou aucune texture sélectionnée n'est disponible."},
        {"parameters", std::move(search_parameters)},
        {"strict", true}
      }
    });
  }

  std::vector<FunctionCall> functionCalls(nlohmann::json const& response)
  {
    std::vector<FunctionCall> calls;
    auto const output = response.find("output");
    if (output == response.end() || !output->is_array())
    {
      return calls;
    }

    for (auto const& item : *output)
    {
      if (!item.is_object())
      {
        continue;
      }

      std::string type;
      if (!readString(item, "type", type) || type != "function_call")
      {
        continue;
      }

      FunctionCall call;
      if (readString(item, "call_id", call.call_id)
          && readString(item, "name", call.name)
          && readString(item, "arguments", call.arguments))
      {
        calls.emplace_back(std::move(call));
      }
    }

    return calls;
  }

  std::string outputText(nlohmann::json const& response)
  {
    std::string result;
    auto const output = response.find("output");
    if (output == response.end() || !output->is_array())
    {
      return result;
    }

    for (auto const& item : *output)
    {
      std::string item_type;
      if (!item.is_object() || !readString(item, "type", item_type) || item_type != "message")
      {
        continue;
      }

      auto const content = item.find("content");
      if (content == item.end() || !content->is_array())
      {
        continue;
      }

      for (auto const& part : *content)
      {
        std::string part_type;
        if (!part.is_object() || !readString(part, "type", part_type) || part_type != "output_text")
        {
          continue;
        }

        std::string text;
        if (!readString(part, "text", text) || text.empty())
        {
          continue;
        }

        if (!result.empty())
        {
          result += '\n';
        }
        result += text;
      }
    }

    return result;
  }
}
