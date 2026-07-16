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
    search_properties["offset"] = {
      {"type", "integer"},
      {"description", "Index du premier résultat ; utiliser next_offset pour explorer d'autres familles."},
      {"minimum", 0},
      {"maximum", 1000000}
    };

    auto search_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", std::move(search_properties)},
      {"required", {"query", "limit", "offset"}},
      {"additionalProperties", false}
    };

    auto texture_preview_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"texture_paths", {
          {"type", "array"},
          {"description", "Textures candidates retournées par search_textures à comparer visuellement."},
          {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
          {"minItems", 1},
          {"maxItems", 12}
        }}
      }},
      {"required", {"texture_paths"}},
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
          {"description", "Hauteur monde visée si height_mode=absolute ; delta ajouté au relief existant si height_mode=offset."},
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
          {"description", "Nom ASCII unique utilisé dans le rapport et comme graine déterministe des bordures."}
        }},
        {"shape", {
          {"type", "string"}, {"enum", {"corridor", "area"}},
          {"description", "corridor suit les points ; area ferme au moins trois points en polygone."}
        }},
        {"height_mode", {
          {"type", "string"}, {"enum", {"absolute", "offset"}},
          {"description", "absolute fixe la hauteur ; offset conserve le relief et ajoute la valeur height."}
        }},
        {"points", {
          {"type", "array"},
          {"description", "Pour corridor : un point crée une plateforme, plusieurs tracent le chemin. Pour area : 3 à 16 sommets simples avec la même hauteur."},
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
          {"type", "integer"}, {"minimum", 0}, {"maximum", 15},
          {"description", "Index dans texture_paths appliqué par cette forme."}
        }},
        {"roughness_amplitude", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 100.0},
          {"description", "Amplitude déterministe du macro/micro-relief intérieur. Utiliser 0 pour les voies et plateformes, environ 3 à 8 pour une jungle légèrement accidentée."}
        }},
        {"texture_strength", {
          {"type", "number"}, {"minimum", 0.05}, {"maximum", 1.0},
          {"description", "Force de la texture sémantique. Utiliser 1 pour une voie nette et environ 0.35 à 0.75 pour fondre naturellement un biome avec la base."}
        }},
        {"width_variation_ratio", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 0.75},
          {"description", "Variation continue de largeur d'un corridor. 0 garde une largeur fixe ; 0.1 à 0.3 évite les rubans uniformes. Ignoré pour area."}
        }},
        {"priority", {
          {"type", "integer"}, {"minimum", 0}, {"maximum", 100},
          {"description", "Les priorités élevées sont composées en dernier aux croisements."}
        }}
      }},
      {"required", {
        "name", "shape", "height_mode", "points", "half_width_ratio",
        "transition_width_ratio", "texture_layer", "roughness_amplitude",
        "texture_strength", "width_variation_ratio", "priority"
      }},
      {"additionalProperties", false}
    };

    auto terrain_layout_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"texture_paths", {
          {"type", "array"},
          {"description", "Palette de carte de deux à 16 textures tileset/*.blp uniques ; la couche 0 est la base hors des formes. Chaque chunk ne peut employer que quatre couches réellement actives."},
          {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
          {"minItems", 2},
          {"maxItems", 16}
        }},
        {"steep_texture_layer", {
          {"type", {"integer", "null"}}, {"minimum", 0}, {"maximum", 15},
          {"description", "Couche qui remplace progressivement les autres sur les pentes, ou null pour désactiver."}
        }},
        {"slope_start_degrees", {
          {"type", {"number", "null"}}, {"minimum", 0.0}, {"maximum", 89.0}
        }},
        {"slope_full_degrees", {
          {"type", {"number", "null"}}, {"minimum", 1.0}, {"maximum", 90.0}
        }},
        {"edge_noise_ratio", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 0.05},
          {"description", "Variation déterministe des bordures selon le plus petit côté ; 0 désactive, 0.003 à 0.012 donne un rendu naturel."}
        }},
        {"max_slope_degrees", {
          {"type", {"number", "null"}}, {"minimum", 5.0}, {"maximum", 60.0},
          {"description", "Élargit automatiquement les transitions trop abruptes ; 25 à 35 convient aux zones jouables, null désactive."}
        }},
        {"smoothing_strength", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
          {"description", "Lissage léger du déplacement généré ; 0 désactive, 0.5 à 0.8 convient à un relief naturel."}
        }},
        {"features", {
          {"type", "array"},
          {"description", "Formes aux noms ASCII uniques, avec au plus 256 segments au total."},
          {"items", std::move(layout_feature_parameters)},
          {"minItems", 1},
          {"maxItems", 32}
        }}
      }},
      {"required", {
        "texture_paths", "steep_texture_layer", "slope_start_degrees",
        "slope_full_degrees", "edge_noise_ratio", "max_slope_degrees",
        "smoothing_strength", "features"
      }},
      {"additionalProperties", false}
    };

    auto liquid_point_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"u", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
          {"description", "Position X normalisée dans les bornes des tuiles existantes."}
        }},
        {"v", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
          {"description", "Position Z normalisée dans les bornes des tuiles existantes."}
        }},
        {"height", {
          {"type", "number"}, {"minimum", -500.0}, {"maximum", 5000.0},
          {"description", "Hauteur absolue de la surface du liquide en unités monde."}
        }}
      }},
      {"required", {"u", "v", "height"}},
      {"additionalProperties", false}
    };

    auto liquid_feature_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"name", {
          {"type", "string"}, {"minLength", 1}, {"maxLength", 64},
          {"description", "Nom ASCII unique de la zone liquide."}
        }},
        {"shape", {
          {"type", "string"}, {"enum", {"corridor", "area"}},
          {"description", "corridor trace une rivière ou un bassin ; area remplit un polygone fermé."}
        }},
        {"points", {
          {"type", "array"},
          {"items", std::move(liquid_point_parameters)},
          {"minItems", 1},
          {"maxItems", 16}
        }},
        {"half_width_ratio", {
          {"type", "number"}, {"minimum", 0.005}, {"maximum", 0.25},
          {"description", "Demi-largeur du cœur liquide, relative au plus petit côté de la carte."}
        }},
        {"transition_width_ratio", {
          {"type", "number"}, {"minimum", 0.001}, {"maximum", 0.25},
          {"description", "Largeur où la profondeur visuelle décroît jusqu'à zéro au rivage."}
        }},
        {"liquid_type_id", {
          {"type", "integer"}, {"minimum", 1}, {"maximum", 65535},
          {"description", "ID terrain eau ou océan de LiquidType.dbc retourné par inspect_map ; au plus 14 IDs distincts par layout."}
        }},
        {"depth", {
          {"type", "number"}, {"minimum", 0.01}, {"maximum", 1.0},
          {"description", "Profondeur/opacité MH2O normalisée, pas une profondeur en unités monde."}
        }},
        {"priority", {
          {"type", "integer"}, {"minimum", 0}, {"maximum", 100},
          {"description", "Le liquide de priorité la plus élevée gagne aux croisements."}
        }}
      }},
      {"required", {
        "name", "shape", "points", "half_width_ratio",
        "transition_width_ratio", "liquid_type_id", "depth", "priority"
      }},
      {"additionalProperties", false}
    };

    auto liquid_layout_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"replace_existing", {
          {"type", "boolean"},
          {"description", "Efface toute eau existante avant l'application. Utiliser true seulement pour une refonte globale ; en mode fusion, l'union des IDs existants et nouveaux reste limitée à 14 par tuile."}
        }},
        {"edge_noise_ratio", {
          {"type", "number"}, {"minimum", 0.0}, {"maximum", 0.05},
          {"description", "Irrégularité déterministe des rives relative au plus petit côté de la carte."}
        }},
        {"features", {
          {"type", "array"},
          {"description", "Une à 64 zones d'eau ou d'océan, avec au plus 128 segments et 14 IDs distincts au total."},
          {"items", std::move(liquid_feature_parameters)},
          {"minItems", 1},
          {"maxItems", 32}
        }}
      }},
      {"required", {"replace_existing", "edge_noise_ratio", "features"}},
      {"additionalProperties", false}
    };

    auto scatter_point_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"u", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"v", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}}
      }},
      {"required", {"u", "v"}},
      {"additionalProperties", false}
    };
    auto scatter_asset_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"role", {{"type", "string"}, {"enum", {"canopy", "understory", "rock", "wall", "detail"}}}},
        {"weight", {{"type", "number"}, {"exclusiveMinimum", 0.0}, {"maximum", 100.0}}},
        {"min_scale", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 10.0}}},
        {"max_scale", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 10.0}}},
        {"spacing_multiplier", {{"type", "number"}, {"minimum", 0.25}, {"maximum", 4.0}}}
      }},
      {"required", {"path", "role", "weight", "min_scale", "max_scale", "spacing_multiplier"}},
      {"additionalProperties", false}
    };
    auto scatter_region_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
        {"role", {{"type", "string"}, {"enum", {"canopy", "understory", "rock", "wall", "detail"}}}},
        {"points", {
          {"type", "array"}, {"items", scatter_point_parameters},
          {"minItems", 3}, {"maxItems", 16}
        }},
        {"density_per_tile", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}},
        {"min_spacing_ratio", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 0.25}}},
        {"min_height", {{"type", "number"}, {"minimum", -500.0}, {"maximum", 5000.0}}},
        {"max_height", {{"type", "number"}, {"minimum", -500.0}, {"maximum", 5000.0}}},
        {"min_slope_degrees", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 90.0}}},
        {"max_slope_degrees", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 90.0}}},
        {"cluster_scale", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 16.0}}},
        {"cluster_strength", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}}
      }},
      {"required", {
        "name", "role", "points", "density_per_tile", "min_spacing_ratio",
        "min_height", "max_height", "min_slope_degrees", "max_slope_degrees",
        "cluster_scale", "cluster_strength"
      }},
      {"additionalProperties", false}
    };
    auto scatter_exclusion_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"shape", {{"type", "string"}, {"enum", {"corridor", "area"}}}},
        {"points", {
          {"type", "array"}, {"items", scatter_point_parameters},
          {"minItems", 1}, {"maxItems", 16}
        }},
        {"half_width_ratio", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 0.25}}}
      }},
      {"required", {"shape", "points", "half_width_ratio"}},
      {"additionalProperties", false}
    };
    auto scatter_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"seed", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
        {"assets", {
          {"type", "array"}, {"items", std::move(scatter_asset_parameters)},
          {"minItems", 1}, {"maxItems", 16}
        }},
        {"regions", {
          {"type", "array"}, {"items", std::move(scatter_region_parameters)},
          {"minItems", 1}, {"maxItems", 16}
        }},
        {"exclusions", {
          {"type", "array"}, {"items", std::move(scatter_exclusion_parameters)},
          {"minItems", 0}, {"maxItems", 96}
        }}
      }},
      {"required", {"seed", "assets", "regions", "exclusions"}},
      {"additionalProperties", false}
    };

    auto props_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"props", {
          {"type", "array"}, {"minItems", 1}, {"maxItems", 256},
          {"items", {
            {"type", "object"},
            {"properties", {
              {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
              {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
              {"u", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
              {"v", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
              {"scale", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 10.0}}},
              {"yaw_degrees", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 360.0}}},
              {"height_offset", {{"type", "number"}, {"minimum", -100.0}, {"maximum", 100.0}}}
            }},
            {"required", {"name", "path", "u", "v", "scale",
                          "yaw_degrees", "height_offset"}},
            {"additionalProperties", false}
          }}
        }}
      }},
      {"required", {"props"}},
      {"additionalProperties", false}
    };

    auto prop_paths_parameters = nlohmann::json{
      {"type", "object"},
      {"description", "Chemins M2 des props d'ambiance : landmarks de base et d'objectif, braziers, lampadaires et lumières dynamiques world/noggit/lights de Patch-E."},
      {"properties", {
        {"base_landmark", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"objective_landmark", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"camp_marker", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"lane_lamp", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"team_left_light", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"team_right_light", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"river_light", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"flame_light", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"lamp_light", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}}
      }},
      {"required", {"base_landmark", "objective_landmark", "camp_marker",
                    "lane_lamp", "team_left_light", "team_right_light",
                    "river_light", "flame_light", "lamp_light"}},
      {"additionalProperties", false}
    };

    auto moba_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"prop_paths", prop_paths_parameters},
        {"texture_paths", {
          {"type", "array"},
          {"description", "Exactement quatre textures choisies visuellement : herbe, voie, sol humide, roche."},
          {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
          {"minItems", 4}, {"maxItems", 4}
        }},
        {"liquid_type_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
        {"assets", scatter_parameters["properties"]["assets"]},
        {"seed", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
        {"base_height", {{"type", "number"}, {"minimum", -450.0}, {"maximum", 4950.0}}},
        {"river_depth", {{"type", "number"}, {"minimum", 2.0}, {"maximum", 30.0}}},
        {"lane_width_ratio", {{"type", "number"}, {"minimum", 0.012}, {"maximum", 0.055},
          {"description", "Demi-largeur normalisée des voies ; utiliser 0.032 par défaut."}}},
        {"river_width_ratio", {{"type", "number"}, {"minimum", 0.015}, {"maximum", 0.08}}},
        {"lane_curvature", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"river_curvature", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
        {"jungle_roughness", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 12.0}}},
        {"vegetation_density_per_tile", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}},
        {"ground_effect_texture_id", {
          {"type", "integer"}, {"minimum", 0},
          {"description", "ID GroundEffectTexture.dbc choisi avec search_ground_effects ; 0 installe et sélectionne l'herbe dense Battle for Azeroth si elle est disponible."}
        }},
        {"skybox_path", {
          {"type", "string"}, {"minLength", 1}, {"maxLength", 260},
          {"description", "Modèle M2 de ciel présent dans le client, généralement sous environments/stars."}
        }},
        {"skybox_flags", {
          {"type", "integer"}, {"minimum", 0}, {"maximum", 3},
          {"description", "Flags LightSkybox : 1 synchronise l'animation avec l'heure, 2 combine avec le ciel procédural."}
        }}
      }},
      {"required", {
        "texture_paths", "liquid_type_id", "assets", "seed", "base_height",
        "river_depth", "lane_width_ratio", "river_width_ratio", "lane_curvature",
        "river_curvature", "jungle_roughness", "vegetation_density_per_tile",
        "ground_effect_texture_id", "prop_paths", "skybox_path", "skybox_flags"
      }},
      {"additionalProperties", false}
    };

    auto skybox_apply_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"skybox_path", { {"type", "string"}, {"minLength", 1}, {"maxLength", 260} }},
        {"flags", { {"type", "integer"}, {"minimum", 0}, {"maximum", 3} }}
      }},
      {"required", {"skybox_path", "flags"}},
      {"additionalProperties", false}
    };

    auto ground_effect_apply_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"texture_path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"effect_id", {{"type", "integer"}, {"minimum", 0},
          {"description", "ID GroundEffectTexture.dbc ; 0 installe et sélectionne l'herbe dense Battle for Azeroth si elle est disponible."}}},
        {"overwrite", {{"type", "boolean"}}}
      }},
      {"required", {"texture_path", "effect_id", "overwrite"}},
      {"additionalProperties", false}
    };

    auto ground_effect_search_parameters = nlohmann::json{
      {"type", "object"},
      {"properties", {
        {"query", {{"type", "string"}, {"maxLength", 128}}},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}}
      }},
      {"required", {"query", "limit"}},
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
        {"description", "Applique sur toute la carte un layout naturel de terrain et de textures décrit en coordonnées normalisées : plateformes, corridors et zones polygonales, en hauteur absolue ou relative. Les paramètres de texture de pente doivent être tous null ou tous renseignés. Opération globale sauvegardée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", std::move(terrain_layout_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "apply_liquid_layout_on_map"},
        {"description", "Crée de vraies surfaces liquides MH2O sur les tuiles existantes à partir de corridors et zones normalisés. Utilise un ID terrain LiquidType.dbc retourné par inspect_map. Opération globale sauvegardée tuile par tuile, non annulable avec Ctrl+Z, réservée à un plan approuvé."},
        {"parameters", std::move(liquid_layout_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "scatter_assets_on_map"},
        {"description", "Répartit en un appel des arbres, rochers, buissons et décors M2/WMO dans des zones polygonales. Respecte les exclusions, la hauteur, la pente, la densité et l'espacement. Les régions du rôle wall (ou nommées *_wall) placent leurs assets en chaîne déterministe alignée sur le périmètre du polygone, pour bâtir des murs collidables. Opération globale déterministe, sauvegardée et réservée à un plan approuvé."},
        {"parameters", std::move(scatter_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "place_props_on_map"},
        {"description", "Place une liste explicite de props M2/WMO à des coordonnées normalisées précises : statues, fontaines, braziers, lampadaires et lumières dynamiques world/noggit/lights de Patch-E. Chaque prop est posé sur la hauteur du terrain, avec un height_offset optionnel pour les lumières flottantes. Opération globale déterministe, sauvegardée et réservée à un plan approuvé."},
        {"parameters", std::move(props_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "apply_ground_effect_on_map"},
        {"description", "Affecte un GroundEffectTexture.dbc à toutes les couches utilisant une texture de terrain donnée. Avec effect_id=0, crée dans le projet un set dense Battle for Azeroth requis par les assets Patch-N. Le rendu suit les alphamaps, est sauvegardé dans MCLY et prévisualisé dans Noggit."},
        {"parameters", std::move(ground_effect_apply_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "apply_skybox_on_map"},
        {"description", "Relie une skybox M2 à la lumière globale de la carte en clonant le LightParams clair et ses bandes, sans modifier les paramètres partagés des autres cartes. Écrit Light.dbc, LightParams.dbc, LightSkybox.dbc, LightIntBand.dbc et LightFloatBand.dbc dans le projet."},
        {"parameters", std::move(skybox_apply_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "create_moba_arena_blueprint"},
        {"description", "Compile les choix esthétiques de l'IA en une topologie MOBA fixe et cohérente sur une carte carrée de 2x2 à 4x4 tuiles, au sol plat : exactement trois voies, deux bases fortifiées ceinturées d'un mur d'enceinte à trois entrées, une rivière, quatre jungles ceinturées de chaînes de murs collidables, des murs sur les deux côtés des chemins de jungle et deux clairières d'objectif. Aucun relief ne sépare les zones : la séparation vient d'assets du rôle wall, des segments de mur M2 avec maillage de collision, de préférence des extensions récentes du Patch-N. Ajoute une couche d'ambiance : fontaines ou statues de base, statues d'objectif, braziers de camps et d'entrées, lampadaires couplés aux lumières dynamiques de Patch-E, puis une skybox moderne de Patch-N. Retourne huit appels génériques à exécuter sans modifier leurs arguments."},
        {"parameters", std::move(moba_parameters)},
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
        {"description", "Relit toutes les tuiles existantes et vérifie terrain, textures réellement visibles et cellules liquides MH2O. À appeler après les opérations globales avant d'annoncer leur réussite."},
        {"parameters", context_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "inspect_map_view"},
        {"description", "Capture la vue 3D actuelle et la montre visuellement au modèle. À appeler une fois après validate_map pour contrôler le rendu, sans relancer automatiquement une modification globale."},
        {"parameters", context_parameters},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_textures"},
        {"description", "Cherche les textures de terrain BLP connues dans la listfile du client. À utiliser avant toute texturation locale ou globale ; parcourir plusieurs termes et pages pour construire une palette variée."},
        {"parameters", std::move(search_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "preview_textures"},
        {"description", "Génère et montre un aperçu visuel de 1 à 12 textures BLP candidates. À appeler après search_textures et avant de choisir une palette dans un plan."},
        {"parameters", std::move(texture_preview_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_assets"},
        {"description", "Cherche les modèles M2 et bâtiments WMO disponibles dans la listfile du client afin de préparer les placements d'un plan."},
        {"parameters", std::move(asset_search_parameters)},
        {"strict", true}
      },
      {
        {"type", "function"},
        {"name", "search_ground_effects"},
        {"description", "Cherche dans GroundEffectTexture.dbc les ensembles d'herbes, fleurs et petits détails disponibles, avec leurs modèles et densités réels."},
        {"parameters", std::move(ground_effect_search_parameters)},
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
