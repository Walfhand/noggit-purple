// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AiProtocol.hpp>

#include <algorithm>
#include <cmath>
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

    auto plan_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"title", {{"type", "string"}, {"maxLength", 120}}},
        {"summary", {{"type", "string"}, {"maxLength", 1500}}},
        {"steps", {
          {"type", "array"},
          {"items", {{"type", "string"}, {"maxLength", 300}}},
          {"minItems", 1},
          {"maxItems", 12}
        }}
      }},
      {"required", {"title", "summary", "steps"}},
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

    auto asset_search_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"query", {{"type", "string"}, {"maxLength", 128}}},
        {"kind", {{"type", "string"}, {"enum", {"any", "m2", "wmo"}}}},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}
      }},
      {"required", {"query", "kind", "limit"}},
      {"additionalProperties", false}
    };

    auto terrain_generation_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"preset", {
          {"type", "string"},
          {"description", "Forme globale du relief."},
          {"enum", {"plains", "rolling_hills", "mountains", "island"}}
        }},
        {"seed", {{"type", "string"}, {"maxLength", 64}}},
        {"base_height", {
          {"type", "number"},
          {"description", "Hauteur médiane en unités monde."},
          {"minimum", -500.0},
          {"maximum", 5000.0}
        }},
        {"height_scale", {
          {"type", "number"},
          {"description", "Amplitude autour de la hauteur médiane. Pour une île naturelle de quelques tuiles, 30 à 60 donne des collines ; au-delà de 100 le relief devient montagneux."},
          {"minimum", 1.0},
          {"maximum", 1500.0}
        }}
      }},
      {"required", {"preset", "seed", "base_height", "height_scale"}},
      {"additionalProperties", false}
    };

    auto texture_blend_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"base_texture_path", {
          {"type", "string"},
          {"description", "Texture principale des zones plates et intermédiaires, sous tileset/*.blp."},
          {"maxLength", 260}
        }},
        {"low_texture_path", {
          {"type", "string"},
          {"description", "Texture des zones basses, par exemple sable, terre humide ou boue."},
          {"maxLength", 260}
        }},
        {"steep_texture_path", {
          {"type", "string"},
          {"description", "Texture des pentes fortes, généralement roche ou terre."},
          {"maxLength", 260}
        }},
        {"high_texture_path", {
          {"type", {"string", "null"}},
          {"description", "Texture optionnelle des sommets. null pour rester à trois textures."},
          {"maxLength", 260}
        }},
        {"seed", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
        {"low_height", {
          {"type", "number"},
          {"description", "Hauteur monde autour de laquelle la texture basse disparaît."},
          {"minimum", -32768.0},
          {"maximum", 32767.0}
        }},
        {"high_height", {
          {"type", {"number", "null"}},
          {"description", "Hauteur monde autour de laquelle la texture de sommet apparaît ; null si high_texture_path est null."}
        }},
        {"blend_width", {
          {"type", "number"},
          {"description", "Demi-largeur des transitions de hauteur, en unités monde."},
          {"minimum", 0.5},
          {"maximum", 1000.0}
        }},
        {"slope_start_degrees", {
          {"type", "number"},
          {"description", "Angle en degrés où la texture de pente commence à apparaître."},
          {"minimum", 0.0},
          {"maximum", 89.0}
        }},
        {"slope_full_degrees", {
          {"type", "number"},
          {"description", "Angle en degrés où la texture de pente devient complète."},
          {"minimum", 1.0},
          {"maximum", 90.0}
        }},
        {"noise_strength", {
          {"type", "number"},
          {"description", "Irrégularité organique des limites de hauteur, entre 0 et 1."},
          {"minimum", 0.0},
          {"maximum", 1.0}
        }}
      }},
      {"required", {
        "base_texture_path", "low_texture_path", "steep_texture_path",
        "high_texture_path", "seed", "low_height", "high_height",
        "blend_width", "slope_start_degrees", "slope_full_degrees",
        "noise_strength"
      }},
      {"additionalProperties", false}
    };

    auto layout_point_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"u", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
          {"description", "Position X normalisée dans les bornes de la carte."}
        }},
        {"v", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
          {"description", "Position Z normalisée dans les bornes de la carte."}
        }},
        {"height", {
          {"type", "number"},
          {"description", "Hauteur monde absolue visée au cœur de la forme."},
          {"minimum", -500.0},
          {"maximum", 5000.0}
        }}
      }},
      {"required", {"u", "v", "height"}},
      {"additionalProperties", false}
    };

    auto layout_feature_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"name", {
          {"type", "string"}, {"minLength", 1}, {"maxLength", 64},
          {"description", "Nom ASCII unique utilisé dans le rapport d'exécution."}
        }},
        {"points", {
          {"type", "array"},
          {"description", "Un point crée une plateforme ; plusieurs points forment un corridor continu."},
          {"items", std::move(layout_point_parameters)},
          {"minItems", 1},
          {"maxItems", 16}
        }},
        {"half_width_ratio", {
          {"type", "number"}, {"minimum", 0.005}, {"maximum", 0.25},
          {"description", "Demi-largeur du cœur, en proportion du plus petit côté de la carte."}
        }},
        {"transition_width_ratio", {
          {"type", "number"}, {"minimum", 0.001}, {"maximum", 0.25},
          {"description", "Largeur de la transition douce jusqu'au terrain existant, selon le plus petit côté."}
        }},
        {"texture_layer", {
          {"type", "integer"}, {"minimum", 0}, {"maximum", 3},
          {"description", "Index dans texture_paths appliqué par cette forme."}
        }},
        {"priority", {
          {"type", "integer"}, {"minimum", 0}, {"maximum", 100},
          {"description", "Les priorités élevées sont composées en dernier aux croisements."}
        }}
      }},
      {"required", {
        "name", "points", "half_width_ratio", "transition_width_ratio",
        "texture_layer", "priority"
      }},
      {"additionalProperties", false}
    };

    auto terrain_layout_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"texture_paths", {
          {"type", "array"},
          {"description", "Palette de deux à quatre textures tileset/*.blp uniques ; la couche 0 est la base hors des formes."},
          {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
          {"minItems", 2},
          {"maxItems", 4}
        }},
        {"steep_texture_layer", {
          {"type", {"integer", "null"}}, {"minimum", 0}, {"maximum", 3},
          {"description", "Couche qui remplace progressivement les autres sur les pentes, ou null pour désactiver."}
        }},
        {"slope_start_degrees", {
          {"type", {"number", "null"}}, {"minimum", 0.0}, {"maximum", 89.0}
        }},
        {"slope_full_degrees", {
          {"type", {"number", "null"}}, {"minimum", 1.0}, {"maximum", 90.0}
        }},
        {"features", {
          {"type", "array"},
          {"description", "Formes aux noms ASCII uniques, avec au plus 128 segments au total."},
          {"items", std::move(layout_feature_parameters)},
          {"minItems", 1},
          {"maxItems", 32}
        }}
      }},
      {"required", {
        "texture_paths", "steep_texture_layer", "slope_start_degrees",
        "slope_full_degrees", "features"
      }},
      {"additionalProperties", false}
    };

    return nlohmann::json::array({
      {
        {"type", "function"},
        {"name", "get_editor_context"},
        {"description", "Lit la carte ouverte, la caméra, le curseur, la sélection et l'état de chargement. À appeler avant une modification si la position est implicite."},
        {"parameters", context_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "inspect_map"},
        {"description", "Inspecte la carte ouverte : masque compact des tuiles existantes, bornes, chargement et plage de hauteurs actuellement observable."},
        {"parameters", context_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "submit_map_plan"},
        {"description", "Présente le plan d'une création ou refonte globale de carte. Doit être appelé avant toute opération *_on_map ; Noggit attend ensuite l'approbation explicite de l'utilisateur."},
        {"parameters", std::move(plan_parameters)},
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
        {"parameters", loaded_tiles_texture_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "generate_terrain_on_map"},
        {"description", "Génère un relief continu et déterministe sur toutes les tuiles existantes. Opération globale enregistrée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", std::move(terrain_generation_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "apply_terrain_layout_on_map"},
        {"description", "Applique sur toute la carte un layout de terrain et de textures décrit en coordonnées normalisées. Une feature avec un point crée une plateforme ; avec plusieurs points, elle crée un corridor. Les paramètres de pente doivent être tous null ou tous renseignés. Opération globale sauvegardée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", std::move(terrain_layout_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "set_base_texture_on_map"},
        {"description", "Remplace les couches des 256 chunks de chaque tuile existante par une texture de base unique. Opération globale enregistrée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", loaded_tiles_texture_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "blend_terrain_textures_on_map"},
        {"description", "Crée sur toute la carte un mélange procédural continu de trois ou quatre textures selon la hauteur, la pente réelle et un bruit déterministe. Utilise cet outil pour un sol naturel après la génération du relief. Opération globale sauvegardée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", std::move(texture_blend_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "validate_map"},
        {"description", "Relit toutes les tuiles existantes et vérifie les 256 chunks de chacune : plage de hauteurs, chunks sans texture et pixels réellement mélangés. À appeler après les opérations globales avant d'annoncer leur réussite."},
        {"parameters", context_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_textures"},
        {"description", "Cherche les textures de terrain BLP connues dans la listfile du client. À utiliser avant paint_texture quand aucun chemin exact ou aucune texture sélectionnée n'est disponible."},
        {"parameters", std::move(search_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_assets"},
        {"description", "Cherche les modèles M2 et bâtiments WMO disponibles dans la listfile du client afin de préparer les placements d'un plan."},
        {"parameters", std::move(asset_search_parameters)},
        {"strict", true}
      }
    });
  }

  float terrainRatio(std::string_view preset, float noise, float edge_distance)
  {
    noise = std::clamp(noise, -1.0f, 1.0f);
    if (preset == "plains")
    {
      return 0.5f + noise * 0.1f;
    }
    if (preset == "rolling_hills")
    {
      return std::clamp(0.5f + noise * 0.3f, 0.0f, 1.0f);
    }
    if (preset == "mountains")
    {
      auto const ridge = 1.0f - std::abs(noise);
      return 0.15f + 0.85f * ridge * ridge;
    }
    if (preset == "island")
    {
      auto const edge = std::clamp(edge_distance, 0.0f, 1.0f);
      auto const land = edge * edge * (3.0f - 2.0f * edge);
      return 0.05f + land * (0.65f + noise * 0.08f);
    }
    return 0.5f;
  }

  std::array<std::uint8_t, 4> textureBlendAlphas(
    float height, float slope_degrees, float noise,
    float low_height, float high_height, bool has_high,
    float blend_width, float slope_start_degrees,
    float slope_full_degrees, float noise_strength)
  {
    auto smooth = [](float from, float to, float value)
    {
      auto const t = std::clamp((value - from) / (to - from), 0.0f, 1.0f);
      return t * t * (3.0f - 2.0f * t);
    };

    auto const noisy_height = height
      + std::clamp(noise, -1.0f, 1.0f) * blend_width
        * std::clamp(noise_strength, 0.0f, 1.0f);
    auto low = 1.0f - smooth(
      low_height - blend_width, low_height + blend_width, noisy_height);
    auto high = has_high
      ? smooth(high_height - blend_width, high_height + blend_width, noisy_height)
      : 0.0f;
    if (auto const total = low + high; total > 1.0f)
    {
      low /= total;
      high /= total;
    }

    auto const steep = smooth(slope_start_degrees, slope_full_degrees, slope_degrees);
    auto const flat = 1.0f - steep;
    auto const weights = std::array{
      flat * std::max(0.0f, 1.0f - low - high),
      flat * low,
      steep,
      flat * high
    };
    std::array<std::uint8_t, 4> alphas{};
    auto remaining = 255;
    for (std::size_t layer = 1; layer < alphas.size(); ++layer)
    {
      auto const alpha = std::clamp(
        static_cast<int>(std::lround(weights[layer] * 255.0f)), 0, remaining);
      alphas[layer] = static_cast<std::uint8_t>(alpha);
      remaining -= alpha;
    }
    alphas[0] = static_cast<std::uint8_t>(remaining);
    return alphas;
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
