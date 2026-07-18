// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AssistantDock.hpp>
#include <noggit/ai/BlueprintSnapshot.hpp>
#include <noggit/ai/MobaArenaBlueprint.hpp>
#include <noggit/ai/ProceduralLiquidLayout.hpp>
#include <noggit/ai/ProceduralLayout.hpp>
#include <noggit/ai/ProceduralProps.hpp>
#include <noggit/ai/ProceduralScatter.hpp>
#include <noggit/ai/ProceduralSkybox.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Brush.h>
#include <noggit/Camera.hpp>
#include <noggit/ChunkWater.hpp>
#include <noggit/DBC.h>
#include <noggit/MapHeaders.h>
#include <noggit/ModelHeaders.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/Sky.h>
#include <noggit/TileIndex.hpp>
#include <noggit/TileWater.hpp>
#include <noggit/TextureManager.h>
#include <noggit/World.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/rendering/GroundEffectPreview.hpp>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>

#include <ClientData.hpp>
#include <ClientFile.hpp>
#include <FastNoise/FastNoise.h>

#include <QtCore/QByteArray>
#include <QtCore/QBuffer>
#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace Noggit::Ai
{
  namespace
  {
    constexpr auto max_tool_rounds = std::size_t{32};
    constexpr auto max_response_output_tokens = 32768;
    constexpr auto max_scatter_candidates = std::size_t{16384};
    constexpr auto max_network_retries = 2;
    constexpr auto network_timeout_ms = 300000;
    constexpr auto preview_image_key = "_noggit_input_image_data_url";
    constexpr auto default_model = "gpt-5.6-terra";
    constexpr auto system_instructions = R"(Tu es l'agent de création intégré à Noggit. Réponds en français.
Pour une petite retouche locale explicite, utilise directement les outils locaux.
Pour une création, une refonte ou une opération portant sur toute la carte :
1. inspecte d'abord la carte et recherche les textures/assets nécessaires ;
2. choisis toi-même quand l'utilisateur te délègue les choix esthétiques ;
3. appelle submit_map_plan et arrête toute mutation jusqu'au message [APPROBATION HOTE] ;
4. après approbation, exécute toutes les étapes disponibles sans demander de confirmation intermédiaire ;
5. appelle validate_map après les opérations globales, puis inspect_map_view une fois avant d'annoncer leur réussite ; utilise la capture pour signaler honnêtement les défauts visibles mais ne relance jamais automatiquement une autre modification globale.
Après generate_terrain_on_map, utilise blend_terrain_textures_on_map pour répartir les textures selon la hauteur et la pente. apply_terrain_layout_on_map produit déjà sa texturation finale par zone : ne l'écrase ensuite ni avec blend_terrain_textures_on_map ni avec set_base_texture_on_map. Réserve paint_texture aux retouches locales.
Pour créer des routes, rivières, voies, plateformes ou autres formes continues, utilise apply_terrain_layout_on_map afin d'appliquer ensemble leur hauteur et leurs textures sémantiques.
Pour toute arène MOBA complète de 2x2 à 4x4 tuiles, utilise create_moba_arena_blueprint après avoir choisi quatre textures dans l'ordre herbe, voie, sol humide, roche, plusieurs assets de jungle, au moins deux segments de mur, un jeu de prop_paths, un GroundEffectTexture avec search_ground_effects et une skybox M2 sous environments/stars. Pour prop_paths, choisis des landmarks M2 des extensions récentes (fountain, statue, brazier, streetlight ou lamppost) et les lumières dynamiques world/noggit/lights/noggit_light_*.m2 de Patch-E : couleur d'équipe à gauche et à droite, flamme orange pour les braziers, violet ou felgreen pour la rivière. Pour une skybox animée complète, utilise skybox_flags=1 afin de la synchroniser avec l'heure sans doubler ses astres avec le ciel procédural. Après approbation du plan, exécute dans l'ordre et sans les modifier les huit next_calls retournés. Le blueprint utilise une topologie en X sur un sol plat : bases fortifiées au sud-ouest et au nord-est ceinturées d'un mur d'enceinte à trois entrées, voies latérales longeant les bords, voie médiane et rivière sur deux diagonales opposées, quatre jungles ceinturées de chaînes de murs et des murs sur les deux côtés de leurs chemins, douze camps et deux fosses d'objectif. Aucun relief ne sépare les zones : toute séparation vient des assets du rôle wall, des segments de mur M2 avec maillage de collision placés en chaîne alignée, le centre des voies et chemins restant ouvert. N'essaie pas de redessiner ces coordonnées toi-même. Les outils génériques restent destinés aux cartes qui ne sont pas des arènes MOBA.
Pour les choix par défaut d'une arène MOBA, préfère des textures et modèles d'extensions récentes réellement retournés par les recherches du client ouvert. Pour le rôle wall, choisis des segments épais et massifs, jamais de clôtures fines : la muraille de Suramar world/expansion06/doodads/suramar/7sr_boundarywall_short (34 unités de long) et 7sr_boundarywall_broken05 (45 unités) possèdent de vraies collisions et s'enchaînent presque parfaitement à l'espacement de 32 unités des chaînes. Utilise-les entre 0.95 et 1.05. La texture de voie doit être une vraie route (road/path/trail), pas une terre générique, et lane_width_ratio vaut 0.02 sauf demande contraire.
Avant une texturation globale, explore plusieurs termes anglais de noms de fichiers et plusieurs pages avec search_textures : grass, dirt, leaf, moss, mud, root et rock. Appelle ensuite preview_textures sur les candidates sérieuses et choisis la palette d'après l'image obtenue, pas seulement d'après leurs noms. Évite de reprendre systématiquement la même famille. Un layout peut référencer jusqu'à 16 textures sur la carte, mais jamais plus de quatre textures actives dans un même chunk ; réserve les variantes aux zones éloignées. Pour casser une grande surface uniforme, ajoute quelques petites areas de texture avec height_mode=offset, height=0 et roughness_amplitude=0 plutôt que de modifier de nouveau tout le relief.
Une texture de boue n'est pas de l'eau. Pour une rivière, un lac ou un océan visible en jeu, crée d'abord son lit avec apply_terrain_layout_on_map puis appelle apply_liquid_layout_on_map avec les mêmes points. Choisis uniquement un liquid_type_id eau ou océan retourné par inspect_map. Place la surface au-dessus du fond et sous les berges. depth est une profondeur/opacité MH2O normalisée : environ 0.2 à 0.6 pour une eau peu profonde, 0.7 à 1 pour une eau profonde. Utilise replace_existing=true seulement lors d'une refonte totale.
Pour rendre une jungle, forêt ou zone rocheuse vivante, recherche plusieurs arbres, buissons, petites plantes, bois mort et rochers avec search_assets puis utilise scatter_assets_on_map après le relief et l'eau. Classe chaque asset dans canopy, understory, rock, wall ou detail ; le rôle wall place ses assets en chaîne alignée sur le périmètre du polygone de sa région et sert à bâtir des murs collidables. Utilise au moins deux espèces de canopée, du sous-bois et des rochers pour une arène MOBA ; spacing_multiplier doit être plus grand pour les grands arbres et plus petit pour les détails. Décris les zones par des polygones, exclue explicitement les voies et plateformes avec des corridors, et laisse aussi l'outil éviter l'eau MH2O. cluster_scale et cluster_strength créent des massifs et des trouées à basse fréquence ; ne distribue pas toutes les espèces uniformément. N'utilise pas le scatter pour les bâtiments uniques ou les objectifs placés précisément.
Pour l'herbe fine et les fleurs au sol, utilise search_ground_effects puis apply_ground_effect_on_map sur la texture d'herbe. Pour une herbe verte dense par défaut, passe effect_id=0 : Noggit installe dans le projet un set Battle for Azeroth dense si ses assets sont disponibles. N'utilise pas scatter_assets_on_map pour simuler des milliers de brins : les GroundEffects suivent déjà les alphamaps, sont sauvegardés dans MCLY et restent absents des couches de voie, roche et eau.
Pour un rendu naturel, trace les corridors avec 5 à 10 points non colinéaires et width_variation_ratio entre 0.1 et 0.3 ; utilise 0 uniquement pour une construction volontairement régulière. Une jungle n'est pas un plateau : utilise shape=area avec height_mode=offset, un delta proche de 0, roughness_amplitude entre 3 et 8 et texture_strength entre 0.35 et 0.75 pour éviter les aplats colorés ; garde roughness_amplitude=0 et texture_strength proche de 1 sur les voies, rivières et plateformes. Utilise des corridors étroits séparés pour les crêtes ou murs. Un étang ou une clairière organique doit être une area à plusieurs points, jamais un corridor à un seul point qui produirait un cercle. Choisis edge_noise_ratio entre 0.003 et 0.012, max_slope_degrees entre 25 et 35 et smoothing_strength entre 0.3 et 0.6. Réserve height_mode=absolute aux formes qui exigent une altitude précise.
Aux croisements, donne la priorité la plus élevée à la forme qui doit conserver sa hauteur et sa texture finales.
Regroupe toutes les formes globales dans un seul appel à cet outil ; n'enchaîne jamais des change_terrain_height pour construire un layout complet et ne rappelle pas le layout uniquement pour retoucher ses textures.
Les outils *_on_map enregistrent les tuiles une par une et ne sont pas annulables avec Ctrl+Z. Rapporte uniquement leurs compteurs réels. Ne prétends jamais qu'une action a réussi sans résultat ok=true. Si un outil échoue partiellement, indique précisément ce qui a été fait et ce qui reste. Une question n'est permise que si une information indispensable ne peut pas être choisie raisonnablement.)";

    std::optional<std::string> validateScatterAssets(
      ProceduralScatter& scatter, nlohmann::json& arguments,
      BlizzardArchive::ClientData* client_data)
    {
      std::set<std::string> wall_region_roles;
      for (auto const& region : scatter.regions)
        if (proceduralScatterIsWallRegion(region))
          wall_region_roles.insert(region.role);
      for (std::size_t index = 0; index < scatter.assets.size(); ++index)
      {
        auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
          scatter.assets[index].path);
        auto const is_m2 = path.ends_with(".m2");
        auto const is_wmo = path.ends_with(".wmo");
        auto const is_wmo_group = is_wmo && path.size() >= 8
          && path[path.size() - 8] == '_'
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 7]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 6]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 5]));
        if ((!is_m2 && !is_wmo) || is_wmo_group
            || path.find("..") != std::string::npos || !client_data->exists(path))
          return "Asset M2/WMO racine introuvable dans les données client : " + path;

        if (wall_region_roles.contains(scatter.assets[index].role))
        {
          if (!is_m2)
            return "Les bandes de mur du blueprint exigent des M2 avec collision : " + path;

          BlizzardArchive::ClientFile file(path, client_data);
          ModelHeader header{};
          if (file.isEof() || file.getSize() < sizeof(header))
            return "Header M2 de mur illisible : " + path;
          std::memcpy(&header, file.getBuffer(), sizeof(header));
          auto const rangeFits = [&](std::uint32_t offset, std::uint32_t count,
                                     std::size_t element_size)
          {
            return offset <= file.getSize()
              && count <= (file.getSize() - offset) / element_size;
          };
          auto has_collision = std::memcmp(header.id, "MD20", 4) == 0
            && header.nBoundingTriangles >= 3
            && header.nBoundingTriangles % 3 == 0
            && header.nBoundingVertices >= 3
            && rangeFits(header.ofsBoundingTriangles,
                         header.nBoundingTriangles, sizeof(std::uint16_t))
            && rangeFits(header.ofsBoundingVertices,
                         header.nBoundingVertices, sizeof(float) * 3);
          for (std::uint32_t triangle = 0;
               has_collision && triangle < header.nBoundingTriangles; ++triangle)
          {
            std::uint16_t vertex = 0;
            std::memcpy(&vertex,
              file.getBuffer() + header.ofsBoundingTriangles
                + triangle * sizeof(vertex),
              sizeof(vertex));
            has_collision = vertex < header.nBoundingVertices;
          }
          file.close();
          if (!has_collision)
            return "Asset de mur sans maillage de collision M2 valide : " + path;
        }
        scatter.assets[index].path = path;
        arguments["assets"][index]["path"] = std::move(path);
      }
      return std::nullopt;
    }

    std::optional<std::string> validateBlueprintScatterAssets(
      nlohmann::json& blueprint, BlizzardArchive::ClientData* client_data)
    {
      for (auto& call : blueprint.at("next_calls"))
      {
        if (call.at("name") == "scatter_assets_on_map")
        {
          auto& arguments = call.at("arguments");
          auto parsed = parseProceduralScatter(arguments);
          if (!parsed.scatter) return parsed.error;
          if (auto const error = validateScatterAssets(
                *parsed.scatter, arguments, client_data))
            return error;
        }
        else if (call.at("name") == "place_props_on_map")
        {
          auto& arguments = call.at("arguments");
          auto const parsed = parseProceduralProps(arguments);
          if (!parsed.props) return parsed.error;
          for (std::size_t index = 0; index < parsed.props->props.size(); ++index)
          {
            auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
              parsed.props->props[index].path);
            if ((!path.ends_with(".m2") && !path.ends_with(".wmo"))
                || path.find("..") != std::string::npos
                || !client_data->exists(path))
              return "Prop du blueprint introuvable dans les données client : " + path;
            arguments["props"][index]["path"] = std::move(path);
          }
        }
        else if (call.at("name") == "apply_skybox_on_map")
        {
          auto& arguments = call.at("arguments");
          auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
            arguments.at("skybox_path").get<std::string>());
          if (!path.starts_with("environments/stars/") || !path.ends_with(".m2")
              || path.find("..") != std::string::npos
              || !client_data->exists(path))
            return "Skybox M2 du blueprint introuvable dans les données client : " + path;
          arguments["skybox_path"] = std::move(path);
        }
      }
      return std::nullopt;
    }

    class PromptEdit final : public QPlainTextEdit
    {
    public:
      explicit PromptEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
      {
      }

      std::function<void()> submit;

    protected:
      bool event(QEvent* event) override
      {
        if (event->type() == QEvent::ShortcutOverride)
        {
          event->accept();
          return true;
        }
        return QPlainTextEdit::event(event);
      }

      void keyPressEvent(QKeyEvent* event) override
      {
        auto const submit_modifier = event->modifiers().testFlag(Qt::ControlModifier)
          || event->modifiers().testFlag(Qt::MetaModifier);
        if (submit_modifier && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter))
        {
          if (submit)
          {
            submit();
          }
          event->accept();
          return;
        }
        QPlainTextEdit::keyPressEvent(event);
      }
    };

    nlohmann::json toolError(std::string const& message)
    {
      return {{"ok", false}, {"error", message}};
    }

    nlohmann::json partialToolError(std::string const& message)
    {
      return {
        {"ok", false},
        {"error", message},
        {"partial_change_possible", true},
        {"undo_available", true}
      };
    }

    QString escapedHtml(QString text)
    {
      return text.toHtmlEscaped().replace('\n', "<br>");
    }

    QString jsonErrorMessage(nlohmann::json const& response)
    {
      auto const error = response.find("error");
      if (error != response.end() && error->is_object())
      {
        auto const message = error->find("message");
        if (message != error->end() && message->is_string())
        {
          return QString::fromStdString(message->get<std::string>());
        }
      }
      return {};
    }

    bool readFiniteNumber(nlohmann::json const& arguments, char const* name, double& value)
    {
      auto const field = arguments.find(name);
      if (field == arguments.end() || !field->is_number())
      {
        return false;
      }

      value = field->get<double>();
      return std::isfinite(value);
    }

    bool circleTouchesTile(double x, double z, double radius, std::size_t tile_x, std::size_t tile_z)
    {
      auto const min_x = static_cast<double>(tile_x) * TILESIZE;
      auto const min_z = static_cast<double>(tile_z) * TILESIZE;
      auto const nearest_x = std::clamp(x, min_x, min_x + TILESIZE);
      auto const nearest_z = std::clamp(z, min_z, min_z + TILESIZE);
      return std::hypot(x - nearest_x, z - nearest_z) <= radius;
    }

    bool segmentTouchesTile(
      double x0, double z0, double x1, double z1, double margin,
      std::size_t tile_x, std::size_t tile_z)
    {
      auto const min_x = static_cast<double>(tile_x) * TILESIZE - margin;
      auto const max_x = static_cast<double>(tile_x + 1) * TILESIZE + margin;
      auto const min_z = static_cast<double>(tile_z) * TILESIZE - margin;
      auto const max_z = static_cast<double>(tile_z + 1) * TILESIZE + margin;
      auto enter = 0.0;
      auto leave = 1.0;
      auto clip = [&](double origin, double delta, double minimum, double maximum)
      {
        if (delta == 0.0)
        {
          return origin >= minimum && origin <= maximum;
        }
        auto first = (minimum - origin) / delta;
        auto second = (maximum - origin) / delta;
        if (first > second)
        {
          std::swap(first, second);
        }
        enter = std::max(enter, first);
        leave = std::min(leave, second);
        return enter <= leave;
      };
      return clip(x0, x1 - x0, min_x, max_x)
        && clip(z0, z1 - z0, min_z, max_z);
    }

    enum class MapBatchOperation
    {
      GenerateTerrain,
      ApplyTerrainLayout,
      ApplyLiquidLayout,
      ApplyGroundEffect,
      ApplySkybox,
      ScatterAssets,
      PlaceProps,
      BlendTerrainTextures,
      SetBaseTexture,
      Validate
    };

    std::optional<MapBatchOperation> mapBatchOperation(std::string_view name)
    {
      if (name == "generate_terrain_on_map") return MapBatchOperation::GenerateTerrain;
      if (name == "apply_terrain_layout_on_map") return MapBatchOperation::ApplyTerrainLayout;
      if (name == "apply_liquid_layout_on_map") return MapBatchOperation::ApplyLiquidLayout;
      if (name == "apply_ground_effect_on_map") return MapBatchOperation::ApplyGroundEffect;
      if (name == "apply_skybox_on_map") return MapBatchOperation::ApplySkybox;
      if (name == "scatter_assets_on_map") return MapBatchOperation::ScatterAssets;
      if (name == "place_props_on_map") return MapBatchOperation::PlaceProps;
      if (name == "blend_terrain_textures_on_map") return MapBatchOperation::BlendTerrainTextures;
      if (name == "set_base_texture_on_map") return MapBatchOperation::SetBaseTexture;
      if (name == "validate_map") return MapBatchOperation::Validate;
      return std::nullopt;
    }

    bool isValidation(MapBatchOperation operation)
    {
      return operation == MapBatchOperation::Validate;
    }

    bool changesHeight(MapBatchOperation operation)
    {
      return operation == MapBatchOperation::GenerateTerrain
        || operation == MapBatchOperation::ApplyTerrainLayout;
    }

    std::string lowerAscii(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    constexpr auto lush_ground_effect_id = 134532u;
    constexpr std::array<unsigned, 4> lush_ground_doodad_ids{1846u, 1847u, 1848u, 1849u};
    constexpr std::array<std::string_view, 4> lush_ground_doodad_names{
      "8kulgrass01.mdl", "8kulgrass02.mdl", "8kulgrass03.mdl", "8kulgrass04.mdl"
    };
    constexpr std::array<std::string_view, 4> previous_lush_ground_doodad_names{
      "8rivgrass01.mdl", "8rivgrass02.mdl", "8rivgrass03.mdl", "8rivgrass04.mdl"
    };
    constexpr std::array<std::string_view, 4> legacy_lush_ground_doodad_names{
      "8zulgrass01.mdl", "8zulgrass01.mdl", "8kulgrass01.mdl", "8kulgrass04.mdl"
    };
    constexpr std::array<unsigned, 4> lush_ground_doodad_flags{0u, 1u, 1u, 1u};
    constexpr std::array<unsigned, 4> lush_ground_doodad_weights{6u, 39u, 43u, 12u};
    constexpr std::array<unsigned, 4> previous_lush_ground_doodad_weights{40u, 30u, 20u, 10u};
    constexpr std::array<unsigned, 4> legacy_lush_ground_doodad_weights{80u, 10u, 7u, 3u};

    bool hasLushGroundEffectAssets()
    {
      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData()) return false;
      return std::all_of(lush_ground_doodad_names.begin(), lush_ground_doodad_names.end(),
        [&](std::string_view name)
        {
          auto path = std::string{"world/nodxt/detail/"} + std::string{name};
          path.replace(path.size() - 4, 4, ".m2");
          return application->clientData()->exists(path);
        });
    }

    std::optional<unsigned> installLushGroundEffect()
    {
      if (!hasLushGroundEffectAssets()) return std::nullopt;

      for (std::size_t i = 0; i < lush_ground_doodad_ids.size(); ++i)
      {
        auto const id = lush_ground_doodad_ids[i];
        if (!gGroundEffectDoodadDB.CheckIfIdExists(id)) continue;
        auto const existing = lowerAscii(
          gGroundEffectDoodadDB.getByID(id).getString(GroundEffectDoodadDB::Filename));
        if (existing != lush_ground_doodad_names[i]
            && existing != previous_lush_ground_doodad_names[i]
            && existing != legacy_lush_ground_doodad_names[i])
          throw std::runtime_error("GroundEffectDoodad ID " + std::to_string(id)
            + " est déjà utilisé par " + existing + '.');
      }

      auto texture_needs_migration = !gGroundEffectTextureDB.CheckIfIdExists(
        lush_ground_effect_id);
      if (!texture_needs_migration)
      {
        auto const record = gGroundEffectTextureDB.getByID(lush_ground_effect_id);
        auto is_legacy = record.getUInt(GroundEffectTextureDB::Amount) == 24u
          && record.getUInt(GroundEffectTextureDB::TerrainType) == 5u;
        auto is_previous = record.getUInt(GroundEffectTextureDB::Amount) == 24u
          && record.getUInt(GroundEffectTextureDB::TerrainType) == 5u;
        auto is_expected = record.getUInt(GroundEffectTextureDB::Amount) == 12u
          && record.getUInt(GroundEffectTextureDB::TerrainType) == 5u;
        for (std::size_t i = 0; i < lush_ground_doodad_ids.size(); ++i)
        {
          auto const uses_doodad = record.getUInt(GroundEffectTextureDB::Doodads + i)
            == lush_ground_doodad_ids[i];
          is_legacy = is_legacy && uses_doodad
            && record.getUInt(GroundEffectTextureDB::Weights + i)
              == legacy_lush_ground_doodad_weights[i];
          is_previous = is_previous && uses_doodad
            && record.getUInt(GroundEffectTextureDB::Weights + i)
              == previous_lush_ground_doodad_weights[i];
          is_expected = is_expected && uses_doodad
            && record.getUInt(GroundEffectTextureDB::Weights + i)
              == lush_ground_doodad_weights[i];
        }
        if (!is_legacy && !is_previous && !is_expected)
          throw std::runtime_error("GroundEffectTexture ID 134532 existe avec un contenu différent.");
        texture_needs_migration = !is_expected;
      }

      auto doodads_changed = false;
      for (std::size_t i = 0; i < lush_ground_doodad_ids.size(); ++i)
      {
        auto const id = lush_ground_doodad_ids[i];
        if (gGroundEffectDoodadDB.CheckIfIdExists(id))
        {
          auto record = gGroundEffectDoodadDB.getByID(id);
          auto const existing = lowerAscii(record.getString(GroundEffectDoodadDB::Filename));
          if (existing != lush_ground_doodad_names[i]
              || record.getUInt(GroundEffectDoodadDB::Flags) != lush_ground_doodad_flags[i])
          {
            record.writeString(GroundEffectDoodadDB::Filename,
                               std::string{lush_ground_doodad_names[i]});
            record.write(GroundEffectDoodadDB::Flags, lush_ground_doodad_flags[i]);
            doodads_changed = true;
          }
          continue;
        }
        auto record = gGroundEffectDoodadDB.addRecord(id);
        record.writeString(GroundEffectDoodadDB::Filename,
                           std::string{lush_ground_doodad_names[i]});
        record.write(GroundEffectDoodadDB::Flags, lush_ground_doodad_flags[i]);
        doodads_changed = true;
      }

      auto texture_changed = false;
      if (texture_needs_migration
          && gGroundEffectTextureDB.CheckIfIdExists(lush_ground_effect_id))
      {
        auto record = gGroundEffectTextureDB.getByID(lush_ground_effect_id);
        for (std::size_t i = 0; i < lush_ground_doodad_ids.size(); ++i)
        {
          record.write(GroundEffectTextureDB::Doodads + i, lush_ground_doodad_ids[i]);
          record.write(GroundEffectTextureDB::Weights + i, lush_ground_doodad_weights[i]);
        }
        record.write(GroundEffectTextureDB::Amount, 12u);
        record.write(GroundEffectTextureDB::TerrainType, 5u);
        texture_changed = true;
      }
      else if (texture_needs_migration)
      {
        auto record = gGroundEffectTextureDB.addRecord(lush_ground_effect_id);
        for (std::size_t i = 0; i < lush_ground_doodad_ids.size(); ++i)
        {
          record.write(GroundEffectTextureDB::Doodads + i, lush_ground_doodad_ids[i]);
          record.write(GroundEffectTextureDB::Weights + i, lush_ground_doodad_weights[i]);
        }
        record.write(GroundEffectTextureDB::Amount, 12u);
        record.write(GroundEffectTextureDB::TerrainType, 5u);
        texture_changed = true;
      }

      if (doodads_changed) gGroundEffectDoodadDB.save();
      if (texture_changed) gGroundEffectTextureDB.save();
      return lush_ground_effect_id;
    }

    std::optional<unsigned> automaticGroundEffectId()
    {
      unsigned best_id = 0;
      auto best_score = -1;
      for (auto it = gGroundEffectTextureDB.begin(); it != gGroundEffectTextureDB.end(); ++it)
      {
        auto const id = it->getUInt(GroundEffectTextureDB::ID);
        auto score = static_cast<int>(it->getUInt(GroundEffectTextureDB::Amount)) * 1000;
        auto valid_doodads = 0;
        for (std::size_t slot = 0; slot < 4; ++slot)
        {
          auto const doodad_id = it->getUInt(GroundEffectTextureDB::Doodads + slot);
          auto const weight = it->getUInt(GroundEffectTextureDB::Weights + slot);
          if (!doodad_id || !weight || !gGroundEffectDoodadDB.CheckIfIdExists(doodad_id))
            continue;
          ++valid_doodads;
          auto const path = lowerAscii(
            gGroundEffectDoodadDB.getByID(doodad_id).getString(
              GroundEffectDoodadDB::Filename));
          score += Noggit::Rendering::groundEffectAssetScore(path);
        }
        if (valid_doodads && score > best_score)
        {
          best_score = score;
          best_id = id;
        }
      }
      return best_id ? std::optional<unsigned>{best_id} : std::nullopt;
    }

    std::int32_t stableSeed(std::string_view value)
    {
      std::uint32_t hash = 2166136261u;
      for (unsigned char byte : value)
      {
        hash = (hash ^ byte) * 16777619u;
      }
      return static_cast<std::int32_t>(hash);
    }

    float terrainFrequency(std::string_view preset)
    {
      if (preset == "plains")
      {
        return 0.006f;
      }
      if (preset == "mountains")
      {
        return 0.018f;
      }
      return 0.01f;
    }

    struct TerrainSample
    {
      float height;
      float slope_degrees;
    };

    TerrainSample sampleTerrain(MapChunk const& chunk, int alpha_x, int alpha_z)
    {
      auto const local_x = (static_cast<float>(alpha_x) + 0.5f) * TEXDETAILSIZE;
      auto const local_z = (static_cast<float>(alpha_z) + 0.5f) * TEXDETAILSIZE;
      auto const cell_x = std::clamp(static_cast<int>(local_x / UNITSIZE), 0, 7);
      auto const cell_z = std::clamp(static_cast<int>(local_z / UNITSIZE), 0, 7);
      auto const u = (local_x - cell_x * UNITSIZE) / UNITSIZE;
      auto const v = (local_z - cell_z * UNITSIZE) / UNITSIZE;

      auto const center = MapChunk::indexLoD(cell_z, cell_x);
      auto const top_left = MapChunk::indexNoLoD(cell_z, cell_x);
      auto const top_right = MapChunk::indexNoLoD(cell_z, cell_x + 1);
      auto const bottom_left = MapChunk::indexNoLoD(cell_z + 1, cell_x);
      auto const bottom_right = MapChunk::indexNoLoD(cell_z + 1, cell_x + 1);

      int second = top_left;
      int third = bottom_left;
      if (v <= u && v <= 1.0f - u)
      {
        second = top_right;
        third = top_left;
      }
      else if (u >= v && u >= 1.0f - v)
      {
        second = bottom_right;
        third = top_right;
      }
      else if (v >= u && v >= 1.0f - u)
      {
        second = bottom_left;
        third = bottom_right;
      }

      auto const& p0 = chunk.mVertices[center];
      auto const& p1 = chunk.mVertices[second];
      auto const& p2 = chunk.mVertices[third];
      auto const normal = glm::cross(p1 - p0, p2 - p0);
      auto const world_x = chunk.xbase + local_x;
      auto const world_z = chunk.zbase + local_z;
      auto const height = p0.y
        - (normal.x * (world_x - p0.x) + normal.z * (world_z - p0.z)) / normal.y;
      constexpr auto radians_to_degrees = 57.29577951308232f;
      auto const up = std::clamp(std::abs(normal.y) / glm::length(normal), 0.0f, 1.0f);
      return {height, std::acos(up) * radians_to_degrees};
    }

    TextureSet& ensureTextureSet(MapChunk& chunk, Noggit::NoggitRenderContext context)
    {
      if (!chunk.texture_set)
      {
        MapChunkHeader header{};
        chunk.texture_set = std::make_unique<TextureSet>(
          &chunk, nullptr, 0, chunk.use_big_alphamap,
          false, false, context, header);
      }
      return *chunk.texture_set;
    }
  }

  struct MapBatchState
  {
    FunctionCall call;
    MapBatchOperation operation = MapBatchOperation::Validate;
    nlohmann::json arguments;
    std::vector<TileIndex> tiles;
    std::vector<bool> keep_loaded;
    std::size_t next_tile = 0;
    std::size_t tiles_changed = 0;
    std::size_t chunks_changed = 0;
    std::size_t failures = 0;
    std::set<TileIndex> failed_tiles;
    std::size_t vertices_inspected = 0;
    std::size_t chunks_without_texture = 0;
    std::size_t chunks_with_multiple_texture_layers = 0;
    std::size_t mixed_texture_chunks = 0;
    std::size_t mixed_texture_pixels = 0;
    std::size_t max_texture_layers = 0;
    std::array<std::size_t, procedural_layout_max_texture_paths> visible_texture_pixels{};
    std::map<std::string, std::size_t> texture_chunks_by_path;
    std::map<std::string, std::size_t> visible_texture_pixels_by_path;
    std::map<std::string, std::size_t> strong_texture_pixels_by_path;
    std::map<std::string, std::uint8_t> max_texture_alpha_by_path;
    std::map<unsigned, std::size_t> ground_effect_layers_by_id;
    std::map<std::string, std::size_t> ground_effect_layers_by_texture;
    std::size_t ground_effect_layers_matched = 0;
    float min_height = std::numeric_limits<float>::max();
    float max_height = std::numeric_limits<float>::lowest();
    float min_slope = std::numeric_limits<float>::max();
    float max_slope = std::numeric_limits<float>::lowest();
    std::size_t min_tile_x = 63;
    std::size_t max_tile_x = 0;
    std::size_t min_tile_z = 63;
    std::size_t max_tile_z = 0;
    bool recalculating_normals = false;
    bool map_view_was_enabled = true;
    std::size_t normals_recalculated = 0;
    std::size_t vertices_changed = 0;
    std::size_t height_chunks_changed = 0;
    std::optional<ProceduralLayout> procedural_layout;
    std::optional<ProceduralLiquidLayout> procedural_liquid_layout;
    std::optional<ProceduralScatter> procedural_scatter;
    std::vector<scoped_blp_texture_reference> procedural_textures;
    std::vector<std::size_t> feature_core_pixels;
    std::vector<std::size_t> feature_transition_pixels;
    std::array<std::size_t, procedural_layout_max_texture_paths> strong_texture_pixels{};
    std::array<std::uint8_t, procedural_layout_max_texture_paths> max_texture_alpha{};
    std::vector<TileIndex> height_changed_tiles;
    std::vector<TileIndex> normal_tiles;
    std::vector<TileIndex> normal_neighborhood;
    std::vector<bool> normal_keep_loaded;
    std::size_t liquid_chunks_changed = 0;
    std::size_t liquid_cells_changed = 0;
    std::size_t liquid_cells_cropped = 0;
    std::size_t liquid_chunks_inspected = 0;
    std::size_t liquid_cells_inspected = 0;
    std::size_t liquid_cells_under_terrain = 0;
    std::vector<std::size_t> liquid_feature_cells;
    std::map<int, std::size_t> liquid_cells_by_type;
    std::map<int, float> min_liquid_height_by_type;
    std::map<int, float> max_liquid_height_by_type;
    std::map<int, float> min_liquid_depth_by_type;
    std::map<int, float> max_liquid_depth_by_type;
    std::set<std::uint32_t> known_instance_uids;
    std::vector<std::pair<glm::vec2, float>> occupied_positions;
    std::size_t scatter_candidates = 0;
    std::size_t scatter_placed = 0;
    std::size_t scatter_rejected_outside = 0;
    std::size_t scatter_rejected_exclusion = 0;
    std::size_t scatter_rejected_terrain = 0;
    std::size_t scatter_rejected_liquid = 0;
    std::size_t scatter_rejected_spacing = 0;
    std::map<std::string, std::size_t> scatter_by_asset;
    std::map<std::string, std::size_t> scatter_by_region;
    std::optional<ProceduralProps> procedural_props;
    std::size_t props_placed = 0;
    std::map<std::string, std::size_t> props_by_path;
  };

  AssistantDock::AssistantDock(MapView* map_view, QWidget* parent)
    : QDockWidget(tr("AI Assistant"), parent)
    , _map_view(map_view)
    , _network(new QNetworkAccessManager(this))
    , _transcript(new QTextBrowser(this))
    , _api_key(new QLineEdit(this))
    , _prompt(new PromptEdit(this))
    , _send_button(new QPushButton(tr("Envoyer"), this))
    , _reset_button(new QPushButton(tr("Nouvelle conversation"), this))
    , _approve_button(new QPushButton(tr("Approuver et exécuter"), this))
    , _blueprint_lab_button(new QPushButton(tr("Lab blueprint MOBA"), this))
    , _status(new QLabel(this))
    , _input(nlohmann::json::array())
    , _pending_plan(nlohmann::json::object())
  {
    setObjectName("aiAssistantDock");
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetMovable
                | QDockWidget::DockWidgetFloatable
                | QDockWidget::DockWidgetClosable);
    setMinimumWidth(340);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    auto* api_key_row = new QHBoxLayout();
    auto* buttons = new QHBoxLayout();

    _transcript->setReadOnly(true);
    _api_key->setEchoMode(QLineEdit::Password);
    _api_key->setClearButtonEnabled(true);
    _api_key->setPlaceholderText(tr("Non enregistrée"));
    _api_key->setToolTip(tr("Clé utilisée uniquement en mémoire pendant cette session."));
    _prompt->setPlaceholderText(tr("Exemple : transforme cette carte en île naturelle avec des collines et une belle herbe."));
    _prompt->setMaximumHeight(110);
    _approve_button->setVisible(false);
    _status->setWordWrap(true);

    buttons->addWidget(_reset_button);
    buttons->addWidget(_approve_button);
    buttons->addWidget(_blueprint_lab_button);
    buttons->addStretch();
    buttons->addWidget(_send_button);
    api_key_row->addWidget(new QLabel(tr("Clé API OpenAI :"), this));
    api_key_row->addWidget(_api_key, 1);
    layout->addLayout(api_key_row);
    layout->addWidget(_transcript, 1);
    layout->addWidget(_prompt);
    layout->addLayout(buttons);
    layout->addWidget(_status);
    setWidget(content);

    static_cast<PromptEdit*>(_prompt)->submit = [this] { submitPrompt(); };
    connect(_send_button, &QPushButton::clicked, this, [this]
    {
      if (_busy)
      {
        cancelPending();
      }
      else
      {
        submitPrompt();
      }
    });
    connect(_reset_button, &QPushButton::clicked, this, [this] { resetConversation(); });
    connect(_approve_button, &QPushButton::clicked, this, [this] { approvePlan(); });
    connect(_blueprint_lab_button, &QPushButton::clicked, this,
            [this] { openMobaBlueprintLab(); });

    appendTranscript(tr("Noggit"), tr("Décris la carte que tu veux. Pour une création globale, je proposerai d'abord un plan à approuver, puis je l'exécuterai sur la carte ouverte."));
    if (qEnvironmentVariableIsEmpty("OPENAI_API_KEY"))
    {
      _status->setText(tr("Saisis une clé API OpenAI ci-dessus."));
    }
    else
    {
      _status->setText(tr("Prêt — Ctrl+Entrée pour envoyer."));
    }
  }

  AssistantDock::~AssistantDock() = default;

  void AssistantDock::openMobaBlueprintLab()
  {
    if (_busy || !_map_view || !_map_view->getWorld()) return;

    auto* world = _map_view->getWorld();
    auto const project_path = std::filesystem::path(
      Noggit::Project::CurrentProject::get()->ProjectPath);
    auto const map_relative_path = std::filesystem::path(
      BlizzardArchive::ClientData::normalizeFilenameInternal(
        "world/maps/" + world->basename));
    auto const map_path = project_path / map_relative_path;
    auto const snapshot_path = project_path / ".noggit" / "moba-blueprint-snapshots"
      / BlizzardArchive::ClientData::normalizeFilenameInternal(world->basename);
    auto const can_regenerate = std::filesystem::is_directory(snapshot_path);

    static auto const default_specification = R"json({
  "texture_paths": [
    "tileset/6.0/tanaanjungle/data/6tj_junglegrass02_512.blp",
    "tileset/6.0/tanaanjungle/data/6tj_road_02_512.blp",
    "tileset/6.0/tanaanjungle/data/6tj_mud_01_512.blp",
    "tileset/6.0/tanaanjungle/data/6tj_rock_02_1024.blp"
  ],
  "liquid_type_id": 1,
  "ground_effect_texture_id": 0,
  "skybox_path": "environments/stars/tanaan_patch_junglesky01.m2",
  "skybox_flags": 1,
  "assets": [
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchtree_bigcanopy_c01.m2","role":"canopy","weight":3,"min_scale":0.8,"max_scale":1.15,"spacing_multiplier":1.25},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchtree_bigcanopy_c02.m2","role":"canopy","weight":2,"min_scale":0.8,"max_scale":1.15,"spacing_multiplier":1.2},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchtree_bigcanopy_c03.m2","role":"canopy","weight":2,"min_scale":0.8,"max_scale":1.2,"spacing_multiplier":1.2},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchswamp_bush_b01.m2","role":"understory","weight":4,"min_scale":0.7,"max_scale":1.15,"spacing_multiplier":0.55},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchbushtall_b02.m2","role":"understory","weight":4,"min_scale":0.7,"max_scale":1.15,"spacing_multiplier":0.5},
    {"path":"world/expansion06/doodads/suramar/7sr_boundarywall_short.m2","role":"wall","weight":5,"min_scale":0.95,"max_scale":1.05,"spacing_multiplier":1.0},
    {"path":"world/expansion06/doodads/suramar/7sr_boundarywall_broken05.m2","role":"wall","weight":1,"min_scale":0.95,"max_scale":1.05,"spacing_multiplier":1.0},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_rocksmall_a01.m2","role":"rock","weight":2,"min_scale":0.9,"max_scale":1.6,"spacing_multiplier":0.8},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_rocksmall_a03.m2","role":"rock","weight":2,"min_scale":0.9,"max_scale":1.6,"spacing_multiplier":0.8},
    {"path":"world/expansion05/doodads/tanaanjungle/doodads/6tj_patchfernleaf_01.m2","role":"detail","weight":8,"min_scale":0.65,"max_scale":1.1,"spacing_multiplier":0.35}
  ],
  "prop_paths": {
    "base_landmark": "world/expansion07/doodads/human/8hu_kultiras_fountain01.m2",
    "objective_landmark": "world/expansion06/doodads/dungeon/doodads/7du_tombofsargeras_elunestatue01.m2",
    "camp_marker": "world/expansion08/doodads/oribos/9ob_oribos_brazier01.m2",
    "lane_lamp": "world/expansion06/doodads/nightelf/7ne_nightelf_streetlight01.m2",
    "team_left_light": "world/noggit/lights/noggit_light_deepskyblue01.m2",
    "team_right_light": "world/noggit/lights/noggit_light_orange01.m2",
    "river_light": "world/noggit/lights/noggit_light_purple_withshadows01.m2",
    "flame_light": "world/noggit/lights/noggit_light_orange_withshadows01.m2",
    "lamp_light": "world/noggit/lights/noggit_light_dimwhite01.m2"
  },
  "seed": "moba-lab-1",
  "base_height": 20,
  "river_depth": 8,
  "lane_width_ratio": 0.02,
  "river_width_ratio": 0.03,
  "lane_curvature": 0.6,
  "river_curvature": 0.5,
  "jungle_roughness": 5,
  "vegetation_density_per_tile": 96
})json";

    QDialog dialog(this);
    dialog.setWindowTitle(tr("MOBA Blueprint Lab — sans OpenAI"));
    dialog.resize(760, 680);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(tr(
      "Édite la spécification, compile les formes, puis exécute exactement "
      "terrain → eau → herbe au sol → murs de jungle et de base → murs de chemin → props et lumières → végétation → skybox. Carte carrée complète de 2×2 à 4×4 tuiles. "
      "%1").arg(can_regenerate
        ? tr("Une baseline existe : la carte sera restaurée avant la régénération.")
        : tr("La première exécution enregistrera la carte actuelle comme baseline.")), &dialog);
    description->setWordWrap(true);
    auto* editor = new QPlainTextEdit(&dialog);
    QSettings settings;
    static constexpr auto specification_keys = std::array{
      "ai/mobaBlueprintLabSpecificationV13",
      "ai/mobaBlueprintLabSpecificationV12",
      "ai/mobaBlueprintLabSpecificationV11",
      "ai/mobaBlueprintLabSpecificationV10",
      "ai/mobaBlueprintLabSpecificationV9",
      "ai/mobaBlueprintLabSpecificationV8",
      "ai/mobaBlueprintLabSpecificationV7",
      "ai/mobaBlueprintLabSpecificationV6",
      "ai/mobaBlueprintLabSpecificationV5",
      "ai/mobaBlueprintLabSpecificationV4",
      "ai/mobaBlueprintLabSpecificationV3"
    };
    auto saved_specification = settings.value(specification_keys.front());
    if (!saved_specification.isValid())
    {
      for (std::size_t index = 1;
           index < specification_keys.size() && !saved_specification.isValid(); ++index)
      {
        saved_specification = settings.value(specification_keys[index]);
      }
      if (!saved_specification.isValid())
        saved_specification = QString::fromUtf8(default_specification);
      auto migrated = saved_specification.toString();
      try
      {
        auto specification = nlohmann::json::parse(migrated.toStdString());
        auto const defaults = nlohmann::json::parse(default_specification);
        auto unique_assets = nlohmann::json::array();
        std::set<std::string> paths;
        static auto const obsolete_wall_paths = std::set<std::string>{
          "world/expansion07/doodads/riverzone/8riv_rockwall_01.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_02.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_03.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_tall_01.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_tall_02.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_tall_03.m2",
          "world/expansion07/doodads/riverzone/8riv_rockwall_tall_pillar_01.m2",
          "world/expansion07/doodads/bloodtroll/8tr_ancientblood_wallshort01.m2",
          "world/expansion07/doodads/bloodtroll/8tr_ancientblood_wallshort02.m2",
          "world/expansion07/doodads/bloodtroll/8tr_ancientblood_walltall01.m2",
          "world/expansion07/doodads/bloodtroll/8tr_ancientblood_walltall02.m2"
        };
        for (auto asset : specification.at("assets"))
        {
          auto const path = asset.at("path").get<std::string>();
          if (obsolete_wall_paths.contains(path)) continue;
          if (paths.insert(path).second)
            unique_assets.push_back(std::move(asset));
        }
        auto wall_count = static_cast<std::size_t>(std::count_if(
          unique_assets.begin(), unique_assets.end(), [](auto const& asset)
          { return asset.value("role", std::string{}) == "wall"; }));
        auto const add_entire_default_family = wall_count == 0;
        if (wall_count < 2)
          for (auto const& asset : defaults.at("assets"))
          {
            if (asset.at("role") != "wall") continue;
            if ((!add_entire_default_family && wall_count >= 2)
                || unique_assets.size() >= 16)
              break;
            auto const path = asset.at("path").get<std::string>();
            if (paths.insert(path).second)
            {
              unique_assets.push_back(asset);
              ++wall_count;
            }
          }
        specification["assets"] = std::move(unique_assets);
        if (!specification.contains("prop_paths"))
          specification["prop_paths"] = defaults.at("prop_paths");
        if (!specification.contains("skybox_path"))
          specification["skybox_path"] = defaults.at("skybox_path");
        if (!specification.contains("skybox_flags"))
          specification["skybox_flags"] = defaults.at("skybox_flags");
        migrated = QString::fromStdString(specification.dump(2));
      }
      catch (std::exception const&)
      {
        // Keep the user's text editable; compilation will report malformed JSON.
      }
      saved_specification = migrated;
    }
    editor->setPlainText(saved_specification.toString());
    auto* status = new QLabel(tr("Pas encore compilé."), &dialog);
    status->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* reset_button = buttons->addButton(tr("Réinitialiser"), QDialogButtonBox::ResetRole);
    auto* compile_button = buttons->addButton(tr("Compiler"), QDialogButtonBox::ActionRole);
    auto* execute_button = buttons->addButton(
      can_regenerate ? tr("Régénérer") : tr("Compiler et exécuter"),
      QDialogButtonBox::AcceptRole);
    layout->addWidget(description);
    layout->addWidget(editor, 1);
    layout->addWidget(status);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto compile = [this, editor, status]() -> std::optional<nlohmann::json>
    {
      try
      {
        std::vector<std::pair<std::size_t, std::size_t>> tiles;
        auto* world = _map_view->getWorld();
        for (std::size_t z = 0; z < 64; ++z)
          for (std::size_t x = 0; x < 64; ++x)
            if (world->mapIndex.hasTile(TileIndex(x, z))) tiles.emplace_back(x, z);
        if (auto const error = validateMobaArenaFootprint(tiles))
          throw std::invalid_argument(*error);
        auto blueprint = compileMobaArenaBlueprint(
          nlohmann::json::parse(editor->toPlainText().toStdString()),
          static_cast<std::size_t>(std::lround(std::sqrt(tiles.size()))));
        auto* application = Noggit::Application::NoggitApplication::instance();
        if (!application->hasClientData())
          throw std::invalid_argument("Aucune donnée client n'est chargée.");
        if (auto const error = validateBlueprintScatterAssets(
              blueprint, application->clientData()))
          throw std::invalid_argument(*error);
        auto const features = blueprint.at("next_calls").at(0).at("arguments")
          .at("features").size();
        status->setText(tr("Compilation valide : %1 formes, %2 appels exacts.")
                          .arg(features).arg(blueprint.at("next_calls").size()));
        return blueprint;
      }
      catch (std::exception const& exception)
      {
        status->setText(tr("Erreur : %1").arg(QString::fromUtf8(exception.what())));
        return std::nullopt;
      }
    };
    connect(reset_button, &QPushButton::clicked, &dialog, [editor, status]
    {
      QSettings settings;
      for (auto const* key : specification_keys) settings.remove(key);
      editor->setPlainText(QString::fromUtf8(default_specification));
      status->setText(tr("Spécification par défaut restaurée ; pas encore compilée."));
    });
    connect(compile_button, &QPushButton::clicked, &dialog, [compile] { compile(); });
    connect(execute_button, &QPushButton::clicked, &dialog, [&, compile]
    {
      auto blueprint = compile();
      if (!blueprint) return;
      if (QMessageBox::warning(&dialog, tr("Exécuter le blueprint"),
          can_regenerate
            ? tr("La carte actuelle sera remplacée par la baseline, puis régénérée. "
                 "L'opération n'est pas annulable avec Ctrl+Z.")
            : tr("La carte actuelle deviendra la baseline des prochaines régénérations. "
                 "Les opérations ne seront pas annulables avec Ctrl+Z."),
          QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) return;

      std::vector<TileIndex> loaded_tiles;
      for (std::size_t z = 0; z < 64; ++z)
        for (std::size_t x = 0; x < 64; ++x)
          if (world->mapIndex.tileLoaded(TileIndex(x, z)))
            loaded_tiles.emplace_back(x, z);

      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      if (can_regenerate)
      {
        if (auto const error = restoreBlueprintSnapshot(snapshot_path, map_path))
        {
          status->setText(tr("Erreur de restauration : %1")
                            .arg(QString::fromStdString(*error)));
          return;
        }
        NOGGIT_ACTION_MGR->purge();
        for (auto const& tile : loaded_tiles) world->reload_tile(tile);
      }
      else
      {
        world->mapIndex.saveChanged(world);
        world->horizon.save_wdl(world);
        for (auto const& tile : loaded_tiles)
        {
          if (world->mapIndex.has_unsaved_changes(tile))
          {
            status->setText(tr("Impossible de créer la baseline : la carte contient "
                               "encore des changements non sauvegardés."));
            return;
          }
        }
        if (auto const error = captureBlueprintSnapshot(map_path, snapshot_path))
        {
          status->setText(tr("Erreur de création de la baseline : %1")
                            .arg(QString::fromStdString(*error)));
          return;
        }
        NOGGIT_ACTION_MGR->purge();
      }
      // A specification identical to the shipped default is not pinned: the
      // next Noggit update can then evolve the default without a stale copy
      // shadowing it. Only genuinely customized specifications are persisted.
      auto matches_default = false;
      try
      {
        matches_default = nlohmann::json::parse(editor->toPlainText().toStdString())
          == nlohmann::json::parse(default_specification);
      }
      catch (std::exception const&)
      {
      }
      if (matches_default)
      {
        for (auto const* key : specification_keys)
          settings.remove(key);
      }
      else
      {
        settings.setValue(specification_keys.front(), editor->toPlainText());
      }
      _direct_blueprint_calls = blueprint->at("next_calls");
      _direct_blueprint_calls.push_back({{"name", "validate_map"},
                                         {"arguments", nlohmann::json::object()}});
      _direct_blueprint_results = nlohmann::json::array();
      _direct_blueprint_running = true;
      _plan_checkpoint_saved = true;
      dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && _direct_blueprint_running)
    {
      appendTranscript(tr("Lab MOBA"), tr("Blueprint compilé localement ; exécution des 8 appels exacts puis validation."));
      _tool_rounds = 0;
      _cancel_requested = false;
      setBusy(true);
      startNextDirectBlueprintCall();
    }
  }

  void AssistantDock::startNextDirectBlueprintCall()
  {
    if (!_direct_blueprint_running) return;
    if (_cancel_requested || _direct_blueprint_calls.empty())
    {
      auto all_ok = !_direct_blueprint_results.empty();
      QStringList operations;
      for (auto const& result : _direct_blueprint_results)
      {
        auto const ok = result.value("ok", false);
        all_ok = all_ok && ok;
        auto operation = QString::fromStdString(
          result.value("operation", std::string{"?"})) + (ok ? " ✓" : " ✗");
        if (!ok)
        {
          auto const error = result.value("error", std::string{});
          if (!error.empty()) operation += " : " + QString::fromStdString(error);
        }
        operations.push_back(std::move(operation));
      }
      appendTranscript(tr("Lab MOBA"), tr("Exécution %1 : %2")
        .arg(all_ok ? tr("terminée") : tr("terminée avec erreurs"), operations.join(" — ")));
      _direct_blueprint_running = false;
      _cancel_requested = false;
      setBusy(false);
      return;
    }

    auto call_json = _direct_blueprint_calls.front();
    _direct_blueprint_calls.erase(_direct_blueprint_calls.begin());
    FunctionCall call{
      "noggit-direct-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
      call_json.at("name").get<std::string>(),
      call_json.at("arguments").dump()
    };
    _status->setText(tr("Lab MOBA : exécution de %1…").arg(QString::fromStdString(call.name)));
    if (!startMapBatch(call))
      finishDirectBlueprintCall(call, toolError("L'appel compilé n'est pas une opération globale."));
  }

  void AssistantDock::finishDirectBlueprintCall(
    FunctionCall const&, nlohmann::json const& result)
  {
    _direct_blueprint_results.push_back(result);
    if (!result.value("ok", false)) _direct_blueprint_calls.clear();
    ++_tool_rounds;
    QTimer::singleShot(0, this, [this] { startNextDirectBlueprintCall(); });
  }

  void AssistantDock::submitPrompt()
  {
    if (_busy)
    {
      return;
    }

    auto const prompt = _prompt->toPlainText().trimmed();
    if (prompt.isEmpty())
    {
      return;
    }

    if (_api_key->text().trimmed().isEmpty() && qEnvironmentVariableIsEmpty("OPENAI_API_KEY"))
    {
      _status->setText(tr("Saisis une clé API OpenAI."));
      return;
    }

    appendTranscript(tr("Vous"), prompt);
    _input.push_back({{"role", "user"}, {"content", prompt.toStdString()}});
    _prompt->clear();
    _tool_rounds = 0;
    _cancel_requested = false;
    setBusy(true);
    sendRequest();
  }

  void AssistantDock::cancelPending()
  {
    if (_map_batch)
    {
      _cancel_requested = true;
      _status->setText(tr("Annulation demandée — arrêt après la tuile en cours…"));
      return;
    }

    if (_active_reply)
    {
      _cancel_requested = true;
      _active_reply->abort();
      return;
    }

    if (_busy)
    {
      _cancel_requested = true;
      _status->setText(tr("Requête annulée."));
      setBusy(false);
    }
  }

  void AssistantDock::approvePlan()
  {
    if (_busy || _pending_plan.empty())
    {
      return;
    }

    _plan_approved = true;
    _plan_checkpoint_saved = false;
    _approve_button->setVisible(false);
    auto const title = QString::fromStdString(_pending_plan.value("title", std::string{"Plan de carte"}));
    appendTranscript(tr("Vous"), tr("Plan approuvé : %1").arg(title));
    _input.push_back({
      {"role", "user"},
      {"content", "[APPROBATION HOTE] Le plan proposé est approuvé. Exécute maintenant ses étapes de façon autonome, vérifie les résultats réels des outils et ne redemande pas les choix déjà délégués."}
    });
    _tool_rounds = 0;
    _cancel_requested = false;
    setBusy(true);
    sendRequest();
  }

  void AssistantDock::resetConversation()
  {
    if (_busy)
    {
      return;
    }

    _input = nlohmann::json::array();
    _pending_plan = nlohmann::json::object();
    _plan_approved = false;
    _plan_checkpoint_saved = false;
    _approve_button->setVisible(false);
    _transcript->clear();
    appendTranscript(tr("Noggit"), tr("Nouvelle conversation. Le contexte de la carte sera relu à la demande."));
    _status->setText(tr("Prêt — Ctrl+Entrée pour envoyer."));
  }

  void AssistantDock::sendRequest()
  {
    sendRequest(nlohmann::json::array());
  }

  void AssistantDock::sendRequest(nlohmann::json const& extra_input)
  {
    if (!_map_view)
    {
      failTurn(tr("La vue de carte a été fermée."));
      return;
    }

    auto key = _api_key->text().trimmed().toUtf8();
    if (key.isEmpty())
    {
      key = qgetenv("OPENAI_API_KEY");
    }
    if (key.isEmpty())
    {
      failTurn(tr("Clé API OpenAI manquante."));
      return;
    }

    auto model = qgetenv("OPENAI_MODEL");
    if (model.isEmpty())
    {
      model = default_model;
    }

    auto request_input = _input;
    for (auto const& item : extra_input)
    {
      request_input.push_back(item);
    }
    auto request_body = nlohmann::json{
      {"model", model.toStdString()},
      {"instructions", system_instructions},
      {"input", std::move(request_input)},
      {"tools", toolDefinitions()},
      {"parallel_tool_calls", false},
      {"max_output_tokens", max_response_output_tokens},
      {"store", false},
      {"include", {"reasoning.encrypted_content"}}
    };

    postRequest(QByteArray::fromStdString(request_body.dump()), 0);
  }

  void AssistantDock::postRequest(QByteArray const& body, int attempt)
  {
    auto key = _api_key->text().trimmed().toUtf8();
    if (key.isEmpty())
    {
      key = qgetenv("OPENAI_API_KEY");
    }

    auto const client_request_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    QNetworkRequest request(QUrl("https://api.openai.com/v1/responses"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key);
    request.setRawHeader("User-Agent", "Noggit-Purple/AI");
    request.setRawHeader("X-Client-Request-Id", client_request_id);

    auto* reply = _network->post(request, body);
    reply->setProperty("noggitRequestBody", body);
    reply->setProperty("noggitRequestAttempt", attempt);
    reply->setProperty("noggitClientRequestId", client_request_id);
    _active_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleReply(reply); });
    QTimer::singleShot(network_timeout_ms, reply, [reply]
    {
      if (reply->isRunning())
      {
        reply->setProperty("noggitTimedOut", true);
        reply->abort();
      }
    });
    _status->setText(attempt == 0
      ? tr("OpenAI réfléchit…")
      : tr("OpenAI réfléchit… tentative %1/%2")
          .arg(attempt + 1).arg(max_network_retries + 1));
  }

  void AssistantDock::handleReply(QNetworkReply* reply)
  {
    auto const body = reply->readAll();
    auto const status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto const network_error = reply->error();
    auto network_error_text = reply->errorString();
    auto const request_id = QString::fromUtf8(reply->rawHeader("x-request-id"));
    auto const client_request_id = QString::fromUtf8(
      reply->property("noggitClientRequestId").toByteArray());
    auto const request_body = reply->property("noggitRequestBody").toByteArray();
    auto const attempt = reply->property("noggitRequestAttempt").toInt();
    auto const timed_out = reply->property("noggitTimedOut").toBool();
    if (timed_out)
    {
      network_error_text = tr("délai de réponse dépassé");
    }
    if (_active_reply == reply)
    {
      _active_reply.clear();
    }
    reply->deleteLater();

    if (_cancel_requested)
    {
      _cancel_requested = false;
      _status->setText(tr("Requête annulée."));
      setBusy(false);
      return;
    }

    auto const retryable_http = status == 408 || status == 409 || status == 429
      || (status >= 500 && status <= 599);
    auto const retryable_transport = status == 0
      && network_error != QNetworkReply::NoError
      && network_error != QNetworkReply::OperationCanceledError;
    if (attempt < max_network_retries
        && (retryable_http || retryable_transport || timed_out))
    {
      auto delay_ms = 1000 * (1 << attempt);
      auto retry_after_ok = false;
      auto const retry_after_seconds = QString::fromUtf8(
        reply->rawHeader("Retry-After")).toInt(&retry_after_ok);
      if (retry_after_ok && retry_after_seconds >= 0)
      {
        delay_ms = std::min(retry_after_seconds * 1000, 30000);
      }
      qWarning().noquote()
        << "OpenAI request retry"
        << attempt + 1
        << "HTTP" << status
        << "Qt error" << static_cast<int>(network_error)
        << network_error_text
        << "x-request-id" << request_id
        << "client-request-id" << client_request_id;
      _status->setText(tr("Connexion interrompue — nouvelle tentative dans %1 s…")
        .arg(static_cast<double>(delay_ms) / 1000.0, 0, 'g', 2));
      QTimer::singleShot(delay_ms, this, [this, request_body, attempt]
      {
        if (_busy && !_cancel_requested && !_active_reply)
        {
          postRequest(request_body, attempt + 1);
        }
      });
      return;
    }

    if (network_error != QNetworkReply::NoError)
    {
      qWarning().noquote()
        << "OpenAI request failed"
        << "HTTP" << status
        << "Qt error" << static_cast<int>(network_error)
        << network_error_text
        << "x-request-id" << request_id
        << "client-request-id" << client_request_id;
    }

    nlohmann::json response;
    try
    {
      response = nlohmann::json::parse(body.constData(), body.constData() + body.size());
    }
    catch (std::exception const&)
    {
      if (network_error != QNetworkReply::NoError)
      {
        failTurn(tr("Erreur réseau : %1 (code Qt %2, requête cliente %3)")
          .arg(network_error_text)
          .arg(static_cast<int>(network_error))
          .arg(client_request_id));
      }
      else
      {
        failTurn(tr("OpenAI a renvoyé une réponse JSON invalide (HTTP %1).").arg(status));
      }
      return;
    }

    if (status < 200 || status >= 300 || network_error != QNetworkReply::NoError)
    {
      auto message = jsonErrorMessage(response);
      if (message.isEmpty())
      {
        message = network_error != QNetworkReply::NoError
          ? network_error_text
          : tr("Erreur HTTP %1").arg(status);
      }
      if (!request_id.isEmpty())
      {
        message += tr(" (requête %1)").arg(request_id);
      }
      failTurn(message);
      return;
    }

    auto const response_error = jsonErrorMessage(response);
    if (!response_error.isEmpty())
    {
      failTurn(response_error);
      return;
    }

    auto response_status = std::string{};
    auto const status_field = response.find("status");
    if (status_field != response.end() && status_field->is_string())
    {
      response_status = status_field->get<std::string>();
    }
    if (response_status == "failed" || response_status == "incomplete")
    {
      auto reason = std::string{};
      auto const details = response.find("incomplete_details");
      if (details != response.end() && details->is_object())
      {
        auto const field = details->find("reason");
        if (field != details->end() && field->is_string())
        {
          reason = field->get<std::string>();
        }
      }
      if (reason == "max_output_tokens")
      {
        failTurn(tr("OpenAI a atteint la limite de %1 tokens de raisonnement et de sortie. "
                    "Réessayez dans une nouvelle conversation ou simplifiez la demande.")
                   .arg(max_response_output_tokens));
      }
      else
      {
        failTurn(reason.empty()
          ? tr("OpenAI n'a pas pu terminer la réponse.")
          : tr("OpenAI n'a pas pu terminer la réponse : %1").arg(QString::fromStdString(reason)));
      }
      return;
    }

    auto const output = response.find("output");
    if (output == response.end() || !output->is_array())
    {
      failTurn(tr("La réponse OpenAI ne contient aucun élément de sortie."));
      return;
    }

    for (auto const& item : *output)
    {
      _input.push_back(item);
    }

    auto const calls = functionCalls(response);
    if (calls.size() > 1)
    {
      failTurn(tr("OpenAI a demandé plusieurs outils alors que les appels parallèles sont désactivés."));
      return;
    }

    if (!calls.empty())
    {
      if (_tool_rounds >= max_tool_rounds)
      {
        failTurn(tr("La limite de 32 appels d'outils a été atteinte."));
        return;
      }

      nlohmann::json result;
      try
      {
        if (startMapBatch(calls.front()))
        {
          return;
        }
        result = executeTool(calls.front());
      }
      catch (std::exception const& exception)
      {
        result = toolError(std::string{"Erreur interne de l'outil : "} + exception.what());
      }
      catch (...)
      {
        result = toolError("Erreur interne inconnue de l'outil.");
      }

      continueAfterTool(calls.front(), result);
      return;
    }

    auto const answer = outputText(response);
    if (answer.empty())
    {
      failTurn(tr("OpenAI a terminé sans fournir de texte."));
      return;
    }
    finishTurn(QString::fromStdString(answer));
  }

  void AssistantDock::continueAfterTool(FunctionCall const& call, nlohmann::json const& result)
  {
    auto public_result = result;
    auto image_data_url = std::string{};
    auto const image = public_result.find(preview_image_key);
    if (image != public_result.end() && image->is_string())
    {
      image_data_url = image->get<std::string>();
      public_result.erase(image);
    }
    _input.push_back({
      {"type", "function_call_output"},
      {"call_id", call.call_id},
      {"output", public_result.dump()}
    });
    ++_tool_rounds;
    _status->setText(tr("Noggit a terminé %1, poursuite du plan…")
                       .arg(QString::fromStdString(call.name)));
    if (image_data_url.empty())
    {
      sendRequest();
      return;
    }
    auto const is_map_capture = call.name == "inspect_map_view";
    auto const image_prompt = is_map_capture
      ? "Capture actuelle de la vue 3D de Noggit. Évalue la continuité, les formes trop géométriques, les aplats de texture et les défauts visuels évidents. Rapporte-les honnêtement sans lancer automatiquement une nouvelle modification globale."
      : "Aperçu produit par Noggit pour les texture_paths du résultat précédent. Chaque case porte son index ; la moitié gauche montre l'aperçu BLP et la moitié droite sa répétition 2x2. Compare les couleurs, le contraste et les motifs répétitifs avant de choisir.";
    sendRequest(nlohmann::json::array({{
      {"role", "user"},
      {"content", nlohmann::json::array({
        {{"type", "input_text"}, {"text", image_prompt}},
        {{"type", "input_image"}, {"image_url", image_data_url},
         {"detail", is_map_capture ? "high" : "low"}}
      })}
    }}));
  }

  bool AssistantDock::startMapBatch(FunctionCall const& call)
  {
    auto const operation = mapBatchOperation(call.name);
    if (!operation)
    {
      return false;
    }

    auto completeWith = [&](nlohmann::json const& result)
    {
      if (_direct_blueprint_running)
        finishDirectBlueprintCall(call, result);
      else
        continueAfterTool(call, result);
      return true;
    };

    if (!_map_view || !_map_view->getWorld())
    {
      return completeWith(toolError("Aucune carte n'est ouverte."));
    }
    auto const mutates_map = !isValidation(*operation);
    if (mutates_map && !_plan_approved && !_direct_blueprint_running)
    {
      return completeWith(toolError(
        "Cette opération globale exige un plan approuvé avec le bouton « Approuver et exécuter »."));
    }
    if (_map_batch)
    {
      return completeWith(toolError("Une opération globale est déjà en cours."));
    }
    if (NOGGIT_ACTION_MGR->getCurrentAction())
    {
      return completeWith(toolError("Une action utilisateur est en cours. Termine-la puis réessaie."));
    }
    if (!_map_view->context())
    {
      return completeWith(toolError("Le contexte OpenGL de la vue n'est pas disponible."));
    }

    nlohmann::json arguments;
    try
    {
      arguments = nlohmann::json::parse(call.arguments);
    }
    catch (std::exception const&)
    {
      return completeWith(toolError("Les arguments de l'outil ne sont pas un objet JSON valide."));
    }
    if (!arguments.is_object())
    {
      return completeWith(toolError("Les arguments de l'outil doivent être un objet JSON."));
    }
    std::optional<ProceduralLayout> procedural_layout;
    std::optional<ProceduralLiquidLayout> procedural_liquid_layout;
    std::optional<ProceduralScatter> procedural_scatter;
    std::optional<ProceduralProps> procedural_props;

    if (*operation == MapBatchOperation::Validate)
    {
      if (!arguments.empty())
      {
        return completeWith(toolError("validate_map n'accepte aucun argument."));
      }
    }
    else if (*operation == MapBatchOperation::GenerateTerrain)
    {
      static auto const fields = std::set<std::string>{
        "preset", "seed", "base_height", "height_scale"
      };
      if (arguments.size() != fields.size())
      {
        return completeWith(toolError(
          "generate_terrain_on_map exige exactement preset, seed, base_height et height_scale."));
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.count(name))
        {
          return completeWith(toolError("Argument non autorisé : " + name));
        }
      }

      auto const preset_field = arguments.find("preset");
      auto const seed_field = arguments.find("seed");
      if (preset_field == arguments.end() || !preset_field->is_string()
          || seed_field == arguments.end() || !seed_field->is_string())
      {
        return completeWith(toolError("preset et seed doivent être des chaînes."));
      }
      static auto const presets = std::set<std::string>{
        "plains", "rolling_hills", "mountains", "island"
      };
      auto const preset = preset_field->get<std::string>();
      auto const seed = seed_field->get<std::string>();
      if (!presets.count(preset) || seed.empty() || seed.size() > 64)
      {
        return completeWith(toolError(
          "preset est invalide ou seed doit contenir entre 1 et 64 caractères."));
      }
      double base_height = 0.0;
      double height_scale = 0.0;
      if (!readFiniteNumber(arguments, "base_height", base_height)
          || !readFiniteNumber(arguments, "height_scale", height_scale)
          || base_height < -500.0 || base_height > 5000.0
          || height_scale < 1.0 || height_scale > 1500.0)
      {
        return completeWith(toolError(
          "base_height doit être entre -500 et 5000 et height_scale entre 1 et 1500."));
      }
    }
    else if (*operation == MapBatchOperation::ApplyTerrainLayout)
    {
      auto parsed = parseProceduralLayout(arguments);
      if (!parsed.layout)
      {
        return completeWith(toolError(parsed.error.empty()
          ? "Le layout de terrain est invalide." : parsed.error));
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return completeWith(toolError("Aucune donnée client n'est chargée."));
      }
      std::set<std::string> unique_paths;
      for (std::size_t layer = 0; layer < parsed.layout->texture_paths.size(); ++layer)
      {
        auto path = parsed.layout->texture_paths[layer];
        if (path.empty() || path.size() > 260
            || std::any_of(path.begin(), path.end(), [](unsigned char c)
            {
              return c < 32 || c > 126;
            }))
        {
          return completeWith(toolError(
            "Chaque texture du layout doit être un chemin WoW ASCII valide."));
        }
        path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(path));
        if (!path.starts_with("tileset/") || !path.ends_with(".blp")
            || path.find("..") != std::string::npos)
        {
          return completeWith(toolError(
            "Chaque texture du layout doit désigner un fichier tileset/*.blp."));
        }
        if (!application->clientData()->exists(path))
        {
          return completeWith(toolError(
            "La texture n'existe pas dans les données client chargées : " + path));
        }
        if (!unique_paths.insert(path).second)
        {
          return completeWith(toolError(
            "Chaque couche du layout doit utiliser une texture différente : " + path));
        }
        parsed.layout->texture_paths[layer] = path;
        arguments["texture_paths"][layer] = std::move(path);
      }
      procedural_layout = std::move(*parsed.layout);
    }
    else if (*operation == MapBatchOperation::ApplyLiquidLayout)
    {
      auto parsed = parseProceduralLiquidLayout(arguments);
      if (!parsed.layout)
      {
        return completeWith(toolError(parsed.error.empty()
          ? "Le layout liquide est invalide." : parsed.error));
      }
      if (QSettings{}.value("use_mclq_liquids_export", false).toBool())
      {
        return completeWith(toolError(
          "L'export MCLQ est activé dans les réglages. Désactive-le avant de créer de l'eau MH2O."));
      }

      static auto const forbidden_wmo_types = std::set<int>{
        LIQUID_WMO_Water, LIQUID_WMO_Ocean, LIQUID_WMO_Water_Interior,
        LIQUID_WMO_Magma, LIQUID_WMO_Slime
      };
      for (auto const& feature : parsed.layout->features)
      {
        auto const id = static_cast<int>(feature.liquid_type_id);
        if (forbidden_wmo_types.contains(id) || !gLiquidTypeDB.CheckIfIdExists(id))
        {
          return completeWith(toolError(
            "LiquidType.dbc ne contient pas ce type terrain autorisé : "
            + std::to_string(id)));
        }
        auto const basic_type = LiquidTypeDB::getLiquidType(id);
        if (basic_type != liquid_basic_types_water
            && basic_type != liquid_basic_types_ocean)
        {
          return completeWith(toolError(
            "Cette première version accepte uniquement les types eau et océan ; ID refusé : "
            + std::to_string(id)));
        }
      }
      procedural_liquid_layout = std::move(*parsed.layout);
    }
    else if (*operation == MapBatchOperation::ApplyGroundEffect)
    {
      static auto const fields = std::set<std::string>{
        "texture_path", "effect_id", "overwrite"
      };
      if (arguments.size() != fields.size()
          || !arguments.contains("texture_path")
          || !arguments.contains("effect_id")
          || !arguments.contains("overwrite")
          || !arguments.at("texture_path").is_string()
          || !arguments.at("effect_id").is_number_integer()
          || !arguments.at("overwrite").is_boolean())
      {
        return completeWith(toolError(
          "apply_ground_effect_on_map exige exactement texture_path, effect_id et overwrite."));
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.contains(name))
          return completeWith(toolError("Argument non autorisé : " + name));
      }
      auto* application = Noggit::Application::NoggitApplication::instance();
      auto texture_path = BlizzardArchive::ClientData::normalizeFilenameInternal(
        arguments.at("texture_path").get<std::string>());
      if (!application->hasClientData() || !texture_path.starts_with("tileset/")
          || !texture_path.ends_with(".blp") || texture_path.find("..") != std::string::npos
          || !application->clientData()->exists(texture_path))
      {
        return completeWith(toolError(
          "La texture cible du GroundEffect n'existe pas dans le client : " + texture_path));
      }
      auto effect_id = arguments.at("effect_id").get<long long>();
      if (effect_id < 0 || effect_id > std::numeric_limits<unsigned>::max())
        return completeWith(toolError("effect_id est hors limites."));
      if (effect_id == 0 || effect_id == lush_ground_effect_id)
      {
        try
        {
          auto const lush = installLushGroundEffect();
          if (lush)
            effect_id = *lush;
          else if (effect_id == lush_ground_effect_id)
            return completeWith(toolError(
              "Les modèles 8kulgrass01 à 8kulgrass04 requis ne sont pas disponibles dans le client."));
        }
        catch (std::exception const& error)
        {
          return completeWith(toolError(
            "Impossible d'installer le GroundEffect dense Battle for Azeroth dans le projet : "
            + std::string{error.what()}));
        }
      }
      if (effect_id == 0)
      {
        auto const fallback = automaticGroundEffectId();
        if (!fallback)
          return completeWith(toolError("Aucun GroundEffectTexture valide n'est disponible."));
        effect_id = *fallback;
      }
      if (!gGroundEffectTextureDB.CheckIfIdExists(static_cast<unsigned>(effect_id)))
        return completeWith(toolError(
          "GroundEffectTexture.dbc ne contient pas l'ID " + std::to_string(effect_id)));
      arguments["texture_path"] = std::move(texture_path);
      arguments["effect_id"] = effect_id;
    }
    else if (*operation == MapBatchOperation::ApplySkybox)
    {
      static auto const fields = std::set<std::string>{"skybox_path", "flags"};
      if (arguments.size() != fields.size()
          || !arguments.contains("skybox_path") || !arguments.at("skybox_path").is_string()
          || !arguments.contains("flags") || !arguments.at("flags").is_number_integer())
        return completeWith(toolError(
          "apply_skybox_on_map exige exactement skybox_path et flags."));
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.contains(name))
          return completeWith(toolError("Argument non autorisé : " + name));
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
        arguments.at("skybox_path").get<std::string>());
      auto const flags = arguments.at("flags").get<long long>();
      if (!application->hasClientData() || !path.starts_with("environments/stars/")
          || !path.ends_with(".m2") || path.find("..") != std::string::npos
          || !application->clientData()->exists(path))
        return completeWith(toolError(
          "La skybox M2 n'existe pas sous environments/stars dans le client : " + path));
      if (flags < 0 || flags > 3)
        return completeWith(toolError("flags doit être compris entre 0 et 3."));

      try
      {
        auto const update = _map_view->applyGlobalSkybox(
          path, static_cast<std::uint32_t>(flags));
        return completeWith({
          {"ok", true}, {"operation", call.name}, {"skybox_path", path},
          {"flags", flags}, {"changed", update.changed}, {"saved", update.changed},
          {"light_id", update.light_id},
          {"light_params_id", update.light_params_id},
          {"light_skybox_id", update.skybox_id},
          {"noggit_preview_supported", true}, {"undoable", false}
        });
      }
      catch (std::exception const& error)
      {
        return completeWith(toolError(
          "Impossible de rattacher la skybox à la lumière globale : "
          + std::string{error.what()}));
      }
    }
    else if (*operation == MapBatchOperation::ScatterAssets)
    {
      auto parsed = parseProceduralScatter(arguments);
      if (!parsed.scatter)
      {
        return completeWith(toolError(parsed.error.empty()
          ? "Le scatter d'assets est invalide." : parsed.error));
      }
      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return completeWith(toolError("Aucune donnée client n'est chargée."));
      }
      if (auto const error = validateScatterAssets(
            *parsed.scatter, arguments, application->clientData()))
        return completeWith(toolError(*error));
      procedural_scatter = std::move(*parsed.scatter);
    }
    else if (*operation == MapBatchOperation::PlaceProps)
    {
      auto parsed = parseProceduralProps(arguments);
      if (!parsed.props)
      {
        return completeWith(toolError(parsed.error.empty()
          ? "Le placement de props est invalide." : parsed.error));
      }
      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return completeWith(toolError("Aucune donnée client n'est chargée."));
      }
      for (std::size_t index = 0; index < parsed.props->props.size(); ++index)
      {
        auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
          parsed.props->props[index].path);
        auto const is_m2 = path.ends_with(".m2");
        auto const is_wmo = path.ends_with(".wmo");
        auto const is_wmo_group = is_wmo && path.size() >= 8
          && path[path.size() - 8] == '_'
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 7]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 6]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 5]));
        if ((!is_m2 && !is_wmo) || is_wmo_group
            || path.find("..") != std::string::npos
            || !application->clientData()->exists(path))
          return completeWith(toolError(
            "Prop M2/WMO racine introuvable dans les données client : " + path));
        parsed.props->props[index].path = path;
        arguments["props"][index]["path"] = std::move(path);
      }
      procedural_props = std::move(*parsed.props);
    }
    else if (*operation == MapBatchOperation::BlendTerrainTextures)
    {
      static auto const fields = std::set<std::string>{
        "base_texture_path", "low_texture_path", "steep_texture_path",
        "high_texture_path", "seed", "low_height", "high_height",
        "blend_width", "slope_start_degrees", "slope_full_degrees",
        "noise_strength"
      };
      if (arguments.size() != fields.size())
      {
        return completeWith(toolError(
          "blend_terrain_textures_on_map exige exactement ses 11 paramètres."));
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.count(name))
        {
          return completeWith(toolError("Argument non autorisé : " + name));
        }
      }

      auto const seed_field = arguments.find("seed");
      if (seed_field == arguments.end() || !seed_field->is_string())
      {
        return completeWith(toolError("seed doit être une chaîne."));
      }
      auto const seed = seed_field->get<std::string>();
      if (seed.empty() || seed.size() > 64)
      {
        return completeWith(toolError("seed doit contenir entre 1 et 64 caractères."));
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return completeWith(toolError("Aucune donnée client n'est chargée."));
      }
      std::set<std::string> unique_paths;
      auto normalizeTexture = [&](char const* field, bool nullable) -> std::string
      {
        auto const value = arguments.find(field);
        if (value == arguments.end())
        {
          return std::string{field} + " est absent.";
        }
        if (nullable && value->is_null())
        {
          return {};
        }
        if (!value->is_string())
        {
          return std::string{field} + " doit être une chaîne.";
        }
        auto path = value->get<std::string>();
        if (path.empty() || path.size() > 260
            || std::any_of(path.begin(), path.end(), [](unsigned char c)
            {
              return c < 32 || c > 126;
            }))
        {
          return std::string{field} + " doit être un chemin WoW ASCII valide.";
        }
        path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(path));
        if (!path.starts_with("tileset/") || !path.ends_with(".blp")
            || path.find("..") != std::string::npos)
        {
          return std::string{field} + " doit désigner un fichier tileset/*.blp.";
        }
        if (!application->clientData()->exists(path))
        {
          return "La texture n'existe pas dans les données client chargées : " + path;
        }
        if (!unique_paths.insert(path).second)
        {
          return "Chaque rôle procédural doit utiliser une texture différente : " + path;
        }
        arguments[field] = std::move(path);
        return {};
      };
      for (auto const& [field, nullable] : std::array{
             std::pair{"base_texture_path", false},
             std::pair{"low_texture_path", false},
             std::pair{"steep_texture_path", false},
             std::pair{"high_texture_path", true}})
      {
        if (auto const error = normalizeTexture(field, nullable); !error.empty())
        {
          return completeWith(toolError(error));
        }
      }

      auto const has_high = !arguments.at("high_texture_path").is_null();
      if (has_high != !arguments.at("high_height").is_null())
      {
        return completeWith(toolError(
          "high_texture_path et high_height doivent être tous les deux définis ou tous les deux null."));
      }

      double low_height = 0.0;
      double high_height = 0.0;
      double blend_width = 0.0;
      double slope_start = 0.0;
      double slope_full = 0.0;
      double noise_strength = 0.0;
      if (!readFiniteNumber(arguments, "low_height", low_height)
          || !readFiniteNumber(arguments, "blend_width", blend_width)
          || !readFiniteNumber(arguments, "slope_start_degrees", slope_start)
          || !readFiniteNumber(arguments, "slope_full_degrees", slope_full)
          || !readFiniteNumber(arguments, "noise_strength", noise_strength)
          || (has_high && !readFiniteNumber(arguments, "high_height", high_height)))
      {
        return completeWith(toolError("Les paramètres procéduraux numériques doivent être finis."));
      }
      if (low_height < -32768.0 || low_height > 32767.0
          || (has_high && (high_height < -32768.0 || high_height > 32767.0))
          || blend_width < 0.5 || blend_width > 1000.0
          || slope_start < 0.0 || slope_start > 89.0
          || slope_full < 1.0 || slope_full > 90.0
          || slope_start >= slope_full
          || noise_strength < 0.0 || noise_strength > 1.0)
      {
        return completeWith(toolError(
          "Seuils procéduraux invalides : vérifie hauteurs, largeur, angles et bruit."));
      }
      if (has_high && (low_height >= high_height
          || 2.0 * blend_width > high_height - low_height))
      {
        return completeWith(toolError(
          "high_height doit dépasser low_height d'au moins deux fois blend_width."));
      }
    }
    else
    {
      if (arguments.size() != 1 || !arguments.contains("texture_path")
          || !arguments["texture_path"].is_string())
      {
        return completeWith(toolError("set_base_texture_on_map exige exactement texture_path."));
      }
      auto texture_path = arguments["texture_path"].get<std::string>();
      if (texture_path.empty() || texture_path.size() > 260
          || std::any_of(texture_path.begin(), texture_path.end(), [](unsigned char c)
          {
            return c < 32 || c > 126;
          }))
      {
        return completeWith(toolError("texture_path doit être un chemin WoW ASCII valide."));
      }
      texture_path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(texture_path));
      if (!texture_path.starts_with("tileset/") || !texture_path.ends_with(".blp")
          || texture_path.find("..") != std::string::npos)
      {
        return completeWith(toolError(
          "texture_path doit désigner un fichier tileset/*.blp sans traversée de chemin."));
      }
      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData() || !application->clientData()->exists(texture_path))
      {
        return completeWith(toolError(
          "La texture n'existe pas dans les données client chargées : " + texture_path));
      }
      arguments["texture_path"] = std::move(texture_path);
    }

    auto* world = _map_view->getWorld();
    auto batch = std::make_unique<MapBatchState>();
    batch->call = call;
    batch->operation = *operation;
    batch->arguments = std::move(arguments);
    batch->procedural_layout = std::move(procedural_layout);
    batch->procedural_liquid_layout = std::move(procedural_liquid_layout);
    batch->procedural_scatter = std::move(procedural_scatter);
    batch->procedural_props = std::move(procedural_props);
    for (std::size_t z = 0; z < 64; ++z)
    {
      for (std::size_t x = 0; x < 64; ++x)
      {
        auto const tile = TileIndex(x, z);
        if (!world->mapIndex.hasTile(tile))
        {
          continue;
        }
        batch->tiles.push_back(tile);
        batch->keep_loaded.push_back(
          world->mapIndex.tileLoaded(tile) || world->mapIndex.tileAwaitingLoading(tile));
        batch->min_tile_x = std::min(batch->min_tile_x, x);
        batch->max_tile_x = std::max(batch->max_tile_x, x);
        batch->min_tile_z = std::min(batch->min_tile_z, z);
        batch->max_tile_z = std::max(batch->max_tile_z, z);
      }
    }
    if (batch->tiles.empty())
    {
      return completeWith(toolError("La carte ouverte ne contient aucune tuile."));
    }
    if (batch->procedural_scatter)
    {
      auto requested = std::size_t{0};
      auto const tile_width = static_cast<float>(batch->max_tile_x - batch->min_tile_x + 1);
      auto const tile_height = static_cast<float>(batch->max_tile_z - batch->min_tile_z + 1);
      for (auto const& tile : batch->tiles)
      {
        auto const u_min = static_cast<float>(tile.x - batch->min_tile_x) / tile_width;
        auto const u_max = static_cast<float>(tile.x - batch->min_tile_x + 1) / tile_width;
        auto const v_min = static_cast<float>(tile.z - batch->min_tile_z) / tile_height;
        auto const v_max = static_cast<float>(tile.z - batch->min_tile_z + 1) / tile_height;
        for (auto const& region : batch->procedural_scatter->regions)
          if (proceduralScatterRegionIntersects(region, u_min, u_max, v_min, v_max))
            requested += region.density_per_tile;
      }
      if (requested > max_scatter_candidates)
      {
        return completeWith(toolError(
          "Le scatter dépasse 16384 candidats. Réduis density_per_tile ou le nombre de régions."));
      }
    }
    if (batch->procedural_layout)
    {
      auto const feature_count = batch->procedural_layout->features.size();
      batch->feature_core_pixels.resize(feature_count);
      batch->feature_transition_pixels.resize(feature_count);

      auto const map_min_x = static_cast<double>(batch->min_tile_x) * TILESIZE;
      auto const map_min_z = static_cast<double>(batch->min_tile_z) * TILESIZE;
      auto const map_width = static_cast<double>(
        batch->max_tile_x - batch->min_tile_x + 1) * TILESIZE;
      auto const map_height = static_cast<double>(
        batch->max_tile_z - batch->min_tile_z + 1) * TILESIZE;
      auto const short_side = std::min(map_width, map_height);
      for (auto const& feature : batch->procedural_layout->features)
      {
        if (batch->procedural_layout->max_slope_degrees
            && feature.shape == ProceduralLayoutShape::Corridor)
        {
          constexpr double degrees_to_radians = 0.017453292519943295;
          auto const maximum_gradient = std::tan(
            *batch->procedural_layout->max_slope_degrees * degrees_to_radians);
          for (std::size_t point = 1; point < feature.points.size(); ++point)
          {
            auto const& first = feature.points[point - 1];
            auto const& second = feature.points[point];
            auto const segment_length = std::hypot(
              (second.u - first.u) * map_width,
              (second.v - first.v) * map_height);
            if (std::abs(second.height - first.height)
                > maximum_gradient * segment_length)
            {
              return completeWith(toolError(
                "La variation longitudinale dépasse max_slope_degrees dans la feature : "
                + feature.name));
            }
          }
        }

        auto const radius = static_cast<double>(
          feature.half_width_ratio) * short_side;
        ProceduralLayout area_core;
        if (feature.shape == ProceduralLayoutShape::Area)
        {
          area_core.texture_paths.resize(std::max(
            std::size_t{2}, feature.texture_layer + 1));
          area_core.features.push_back(feature);
        }
        auto intersects_map = false;
        for (auto const& tile : batch->tiles)
        {
          for (auto const& point : feature.points)
          {
            auto const world_x = map_min_x + point.u * map_width;
            auto const world_z = map_min_z + point.v * map_height;
            if (circleTouchesTile(world_x, world_z, radius, tile.x, tile.z))
            {
              intersects_map = true;
              break;
            }
          }
          for (std::size_t point = 1;
               !intersects_map && point < feature.points.size(); ++point)
          {
            auto const& first = feature.points[point - 1];
            auto const& second = feature.points[point];
            intersects_map = segmentTouchesTile(
              map_min_x + first.u * map_width,
              map_min_z + first.v * map_height,
              map_min_x + second.u * map_width,
              map_min_z + second.v * map_height,
              radius, tile.x, tile.z);
          }
          if (!intersects_map
              && feature.shape == ProceduralLayoutShape::Area)
          {
            auto const& first = feature.points.back();
            auto const& second = feature.points.front();
            intersects_map = segmentTouchesTile(
              map_min_x + first.u * map_width,
              map_min_z + first.v * map_height,
              map_min_x + second.u * map_width,
              map_min_z + second.v * map_height,
              radius, tile.x, tile.z);
          }
          if (!intersects_map
              && feature.shape == ProceduralLayoutShape::Area)
          {
            auto const u = static_cast<float>(
              (static_cast<double>(tile.x) + 0.5 - batch->min_tile_x)
              / (batch->max_tile_x - batch->min_tile_x + 1));
            auto const v = static_cast<float>(
              (static_cast<double>(tile.z) + 0.5 - batch->min_tile_z)
              / (batch->max_tile_z - batch->min_tile_z + 1));
            intersects_map = sampleProceduralLayout(
              area_core, u, v, 0.0f, 0.0f,
              static_cast<float>(map_width), static_cast<float>(map_height))
                .feature_masks.front() >= 0.999f;
          }
          if (intersects_map)
          {
            break;
          }
        }
        if (!intersects_map)
        {
          return completeWith(toolError(
            "Le cœur de la feature n'intersecte aucune tuile existante : "
            + feature.name));
        }
      }

      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      batch->procedural_textures.reserve(
        batch->procedural_layout->texture_paths.size());
      for (auto const& path : batch->procedural_layout->texture_paths)
      {
        batch->procedural_textures.emplace_back(
          path, _map_view->getRenderContext());
      }
      for (std::size_t layer = 0; layer < batch->procedural_textures.size(); ++layer)
      {
        auto& texture = batch->procedural_textures[layer];
        texture.get()->wait_until_loaded();
        if (texture->loading_failed())
        {
          return completeWith(toolError(
            "Impossible de charger la texture du layout : "
            + batch->procedural_layout->texture_paths[layer]));
        }
      }
    }
    if (batch->procedural_liquid_layout)
    {
      auto const& layout = *batch->procedural_liquid_layout;
      batch->liquid_feature_cells.assign(layout.features.size(), 0);
    }

    if (mutates_map && !_plan_checkpoint_saved)
    {
      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      world->mapIndex.saveChanged(world);
      world->horizon.save_wdl(world);
      NOGGIT_ACTION_MGR->purge();
      for (auto const& tile : batch->tiles)
      {
        if (world->mapIndex.has_unsaved_changes(tile))
        {
          return completeWith(toolError(
            "La génération globale exige d'abord une sauvegarde des changements actuels."));
        }
      }
      _plan_checkpoint_saved = true;
    }

    batch->map_view_was_enabled = _map_view->isEnabled();
    _map_view->setEnabled(false);
    _map_batch = std::move(batch);
    _cancel_requested = false;
    _status->setText(tr("Démarrage de %1 sur %2 tuile(s)…")
                       .arg(QString::fromStdString(call.name))
                       .arg(_map_batch->tiles.size()));
    QTimer::singleShot(0, this, [this] { processMapBatch(); });
    return true;
  }

  void AssistantDock::processMapBatch()
  {
    if (!_map_batch || !_map_view || !_map_view->getWorld())
    {
      return;
    }

    if (_cancel_requested)
    {
      auto* world = _map_view->getWorld();
      for (std::size_t i = 0; i < _map_batch->tiles.size(); ++i)
      {
        if (!_map_batch->keep_loaded[i]
            && world->mapIndex.tileAwaitingLoading(_map_batch->tiles[i]))
        {
          _status->setText(tr("Annulation — fin du chargement en cours…"));
          QTimer::singleShot(25, this, [this] { processMapBatch(); });
          return;
        }
      }
      finishMapBatch({
        {"ok", false},
        {"cancelled", true},
        {"partial_change_possible", !isValidation(_map_batch->operation)
          && _map_batch->tiles_changed > 0},
        {"error", "Opération annulée entre deux tuiles."}
      });
      return;
    }
    auto* world = _map_view->getWorld();
    auto const phase_tiles = _map_batch->recalculating_normals
      ? _map_batch->normal_tiles.size() : _map_batch->tiles.size();
    if (_map_batch->next_tile >= phase_tiles)
    {
      if (changesHeight(_map_batch->operation) && !_map_batch->recalculating_normals
          && !_map_batch->height_changed_tiles.empty())
      {
        std::set<TileIndex> normal_tiles;
        for (auto const& changed : _map_batch->height_changed_tiles)
        {
          for (int z = std::max(0, static_cast<int>(changed.z) - 1);
               z <= std::min(63, static_cast<int>(changed.z) + 1); ++z)
          {
            for (int x = std::max(0, static_cast<int>(changed.x) - 1);
                 x <= std::min(63, static_cast<int>(changed.x) + 1); ++x)
            {
              auto const neighbor = TileIndex(
                static_cast<std::size_t>(x), static_cast<std::size_t>(z));
              if (world->mapIndex.hasTile(neighbor))
              {
                normal_tiles.insert(neighbor);
              }
            }
          }
        }
        _map_batch->normal_tiles.assign(normal_tiles.begin(), normal_tiles.end());
        _map_batch->recalculating_normals = true;
        _map_batch->next_tile = 0;
        _status->setText(tr("Recalcul des normales du terrain…"));
        QTimer::singleShot(0, this, [this] { processMapBatch(); });
        return;
      }
      finishMapBatch(nlohmann::json::object());
      return;
    }

    auto const tile_index = _map_batch->recalculating_normals
      ? _map_batch->normal_tiles[_map_batch->next_tile]
      : _map_batch->tiles[_map_batch->next_tile];
    if (_map_batch->recalculating_normals)
    {
      if (_map_batch->normal_neighborhood.empty())
      {
        for (int z = std::max(0, static_cast<int>(tile_index.z) - 1);
             z <= std::min(63, static_cast<int>(tile_index.z) + 1); ++z)
        {
          for (int x = std::max(0, static_cast<int>(tile_index.x) - 1);
               x <= std::min(63, static_cast<int>(tile_index.x) + 1); ++x)
          {
            auto const neighbor = TileIndex(
              static_cast<std::size_t>(x), static_cast<std::size_t>(z));
            if (!world->mapIndex.hasTile(neighbor))
            {
              continue;
            }
            _map_batch->normal_neighborhood.push_back(neighbor);
            _map_batch->normal_keep_loaded.push_back(
              world->mapIndex.tileLoaded(neighbor)
              || world->mapIndex.tileAwaitingLoading(neighbor));
            if (!world->mapIndex.getTile(neighbor))
            {
              world->mapIndex.loadTile(neighbor, false, true, true);
            }
          }
        }
      }

      bool waiting = false;
      bool failed = false;
      for (auto const& neighbor : _map_batch->normal_neighborhood)
      {
        auto* tile = world->mapIndex.getTile(neighbor);
        if (!tile)
        {
          failed = true;
          _map_batch->failed_tiles.insert(neighbor);
        }
        else if (!tile->finishedLoading())
        {
          waiting = true;
        }
        else if (tile->loading_failed())
        {
          failed = true;
          _map_batch->failed_tiles.insert(neighbor);
        }
      }
      if (waiting)
      {
        _status->setText(tr("Chargement des voisines de %1,%2 pour les normales…")
                           .arg(tile_index.x).arg(tile_index.z));
        QTimer::singleShot(25, this, [this] { processMapBatch(); });
        return;
      }

      auto unloadNeighborhood = [&]
      {
        for (std::size_t i = 0; i < _map_batch->normal_neighborhood.size(); ++i)
        {
          if (!_map_batch->normal_keep_loaded[i])
          {
            world->mapIndex.unloadTile(_map_batch->normal_neighborhood[i]);
          }
        }
        _map_batch->normal_neighborhood.clear();
        _map_batch->normal_keep_loaded.clear();
      };

      try
      {
        auto* tile = world->mapIndex.getTile(tile_index);
        if (failed || !tile)
        {
          ++_map_batch->failures;
        }
        else
        {
          _map_view->makeCurrent();
          OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
          for (unsigned z = 0; z < 16; ++z)
            for (unsigned x = 0; x < 16; ++x)
              tile->getChunk(x, z)->recalcNorms();
          world->mapIndex.setChanged(tile);
          world->mapIndex.saveTile(tile_index, world);
          tile->changed = false;
          ++_map_batch->normals_recalculated;
        }
      }
      catch (...)
      {
        unloadNeighborhood();
        ++_map_batch->failures;
        _map_batch->failed_tiles.insert(tile_index);
        finishMapBatch({
          {"ok", false},
          {"partial_change_possible", true},
          {"error", "Le recalcul des normales du terrain a échoué."}
        });
        return;
      }

      unloadNeighborhood();
      ++_map_batch->next_tile;
      _status->setText(tr("Normales : %1/%2 tuile(s)…")
                         .arg(_map_batch->next_tile).arg(_map_batch->normal_tiles.size()));
      QTimer::singleShot(0, this, [this] { processMapBatch(); });
      return;
    }

    auto* tile = world->mapIndex.getTile(tile_index);
    if (!tile)
    {
      // Saving an ADT without loading its placements would drop its M2/WMO tables.
      tile = world->mapIndex.loadTile(tile_index, false, true, true);
    }
    if (!tile)
    {
      ++_map_batch->failures;
      _map_batch->failed_tiles.insert(tile_index);
      ++_map_batch->next_tile;
      QTimer::singleShot(0, this, [this] { processMapBatch(); });
      return;
    }
    if (!tile->finishedLoading())
    {
      _status->setText(tr("Chargement de la tuile %1,%2 (%3/%4)…")
                         .arg(tile_index.x).arg(tile_index.z)
                         .arg(_map_batch->next_tile + 1).arg(_map_batch->tiles.size()));
      QTimer::singleShot(25, this, [this] { processMapBatch(); });
      return;
    }
    if (tile->loading_failed())
    {
      ++_map_batch->failures;
      _map_batch->failed_tiles.insert(tile_index);
      if (!_map_batch->keep_loaded[_map_batch->next_tile])
      {
        world->mapIndex.unloadTile(tile_index);
      }
      ++_map_batch->next_tile;
      QTimer::singleShot(0, this, [this] { processMapBatch(); });
      return;
    }

    bool tile_height_changed = false;
    std::size_t tile_height_chunks_changed = 0;
    std::size_t tile_vertices_changed = 0;
    bool tile_was_modified = !isValidation(_map_batch->operation)
      && _map_batch->operation != MapBatchOperation::ApplyLiquidLayout
      && _map_batch->operation != MapBatchOperation::ApplyTerrainLayout
      && _map_batch->operation != MapBatchOperation::ApplyGroundEffect
      && _map_batch->operation != MapBatchOperation::ScatterAssets
      && _map_batch->operation != MapBatchOperation::PlaceProps;
    std::size_t tile_liquid_chunks_changed = 0;
    try
    {
      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      if (tile_was_modified)
      {
        world->mapIndex.setChanged(tile);
      }
      if (_map_batch->operation == MapBatchOperation::Validate)
      {
        for (unsigned z = 0; z < 16; ++z)
        {
          for (unsigned x = 0; x < 16; ++x)
          {
            auto* chunk = tile->getChunk(x, z);
            auto* texture_set = chunk->getTextureSet();
            if (!texture_set || texture_set->num() == 0)
            {
              ++_map_batch->chunks_without_texture;
            }
            else
            {
              auto const layer_count = texture_set->num();
              _map_batch->max_texture_layers = std::max(
                _map_batch->max_texture_layers, layer_count);
              if (layer_count > 1)
              {
                ++_map_batch->chunks_with_multiple_texture_layers;
              }
              for (std::size_t layer = 0; layer < layer_count; ++layer)
              {
                auto const& path = texture_set->filename(layer);
                ++_map_batch->texture_chunks_by_path[path];
                auto const effect_id = texture_set->effect(layer);
                if (effect_id != 0xffffffffu && effect_id != 0)
                {
                  ++_map_batch->ground_effect_layers_by_id[effect_id];
                  ++_map_batch->ground_effect_layers_by_texture[path];
                }
              }

              auto const& temporary = texture_set->getTempAlphamaps();
              auto* persistent = texture_set->getAlphamaps();
              bool chunk_is_mixed = false;
              for (std::size_t pixel = 0; pixel < 64 * 64; ++pixel)
              {
                std::array<int, 4> weights{};
                auto overlay_total = 0;
                for (std::size_t layer = 1; layer < layer_count; ++layer)
                {
                  auto const& alphamap = (*persistent)[layer - 1];
                  weights[layer] = temporary
                    ? std::clamp(static_cast<int>(
                        std::lround((*temporary)[layer][pixel])), 0, 255)
                    : (alphamap ? alphamap->getAlpha(pixel) : 0);
                  overlay_total += weights[layer];
                }
                weights[0] = std::max(0, 255 - overlay_total);

                auto visible_layers = 0;
                for (std::size_t layer = 0; layer < layer_count; ++layer)
                {
                  auto const& path = texture_set->filename(layer);
                  auto const alpha = static_cast<std::uint8_t>(
                    std::clamp(weights[layer], 0, 255));
                  _map_batch->max_texture_alpha_by_path[path] = std::max(
                    _map_batch->max_texture_alpha_by_path[path], alpha);
                  if (weights[layer] >= 8)
                  {
                    ++visible_layers;
                    ++_map_batch->visible_texture_pixels[layer];
                    ++_map_batch->visible_texture_pixels_by_path[path];
                  }
                  if (weights[layer] >= 64)
                  {
                    ++_map_batch->strong_texture_pixels_by_path[path];
                  }
                }
                if (visible_layers >= 2)
                {
                  ++_map_batch->mixed_texture_pixels;
                  chunk_is_mixed = true;
                }
              }
              if (chunk_is_mixed)
              {
                ++_map_batch->mixed_texture_chunks;
              }
            }
            for (auto const& vertex : chunk->mVertices)
            {
              _map_batch->min_height = std::min(_map_batch->min_height, vertex.y);
              _map_batch->max_height = std::max(_map_batch->max_height, vertex.y);
              ++_map_batch->vertices_inspected;
            }

            auto* water_chunk = tile->Water.getChunk(x, z);
            auto chunk_has_liquid = false;
            for (auto const& layer : *water_chunk->getLayers())
            {
              auto const id = layer.liquidID();
              _map_batch->min_liquid_height_by_type.try_emplace(
                id, std::numeric_limits<float>::max());
              _map_batch->max_liquid_height_by_type.try_emplace(
                id, std::numeric_limits<float>::lowest());
              _map_batch->min_liquid_depth_by_type.try_emplace(
                id, std::numeric_limits<float>::max());
              _map_batch->max_liquid_depth_by_type.try_emplace(
                id, std::numeric_limits<float>::lowest());
              auto const& vertices = layer.getVertices();
              for (int cell_z = 0; cell_z < 8; ++cell_z)
              {
                for (int cell_x = 0; cell_x < 8; ++cell_x)
                {
                  if (!layer.hasSubchunk(cell_x, cell_z))
                  {
                    continue;
                  }
                  chunk_has_liquid = true;
                  ++_map_batch->liquid_cells_inspected;
                  ++_map_batch->liquid_cells_by_type[id];
                  auto const water_index = cell_z * 9 + cell_x;
                  auto const terrain_index = cell_z * 17 + cell_x;
                  auto cell_below_terrain = true;
                  for (auto const [water_offset, terrain_offset]
                       : std::array<std::pair<int, int>, 4>{
                           std::pair{0, 0}, {1, 1}, {9, 17}, {10, 18}})
                  {
                    auto const& vertex = vertices[water_index + water_offset];
                    _map_batch->min_liquid_height_by_type[id] = std::min(
                      _map_batch->min_liquid_height_by_type[id], vertex.position.y);
                    _map_batch->max_liquid_height_by_type[id] = std::max(
                      _map_batch->max_liquid_height_by_type[id], vertex.position.y);
                    _map_batch->min_liquid_depth_by_type[id] = std::min(
                      _map_batch->min_liquid_depth_by_type[id], vertex.depth);
                    _map_batch->max_liquid_depth_by_type[id] = std::max(
                      _map_batch->max_liquid_depth_by_type[id], vertex.depth);
                    cell_below_terrain = cell_below_terrain
                      && vertex.position.y
                        < chunk->mVertices[terrain_index + terrain_offset].y;
                  }
                  _map_batch->liquid_cells_under_terrain
                    += cell_below_terrain ? 1 : 0;
                }
              }
            }
            _map_batch->liquid_chunks_inspected += chunk_has_liquid ? 1 : 0;
          }
        }
      }
      else if (_map_batch->operation == MapBatchOperation::GenerateTerrain)
      {
        auto const preset = _map_batch->arguments.at("preset").get<std::string>();
        auto const seed = _map_batch->arguments.at("seed").get<std::string>();
        auto const base_height = static_cast<float>(
          _map_batch->arguments.at("base_height").get<double>());
        auto const height_scale = static_cast<float>(
          _map_batch->arguments.at("height_scale").get<double>());

        auto generator = FastNoise::New<FastNoise::Simplex>();
        std::vector<float> noise(257 * 257);
        generator->GenUniformGrid2D(
          noise.data(), static_cast<int>(tile_index.x * 256),
          static_cast<int>(tile_index.z * 256), 257, 257,
          terrainFrequency(preset), stableSeed(seed));

        QImage heightmap(257, 257, QImage::Format_RGBA64);
        auto const map_width = static_cast<float>(
          (_map_batch->max_tile_x - _map_batch->min_tile_x + 1) * 256);
        auto const map_height = static_cast<float>(
          (_map_batch->max_tile_z - _map_batch->min_tile_z + 1) * 256);
        for (int y = 0; y < 257; ++y)
        {
          for (int x = 0; x < 257; ++x)
          {
            auto const global_x = static_cast<float>(
              (tile_index.x - _map_batch->min_tile_x) * 256) + x;
            auto const global_z = static_cast<float>(
              (tile_index.z - _map_batch->min_tile_z) * 256) + y;
            auto const normalized_x = global_x / map_width;
            auto const normalized_z = global_z / map_height;
            auto const dx = 2.0f * normalized_x - 1.0f;
            auto const dz = 2.0f * normalized_z - 1.0f;
            auto const edge_distance = std::clamp(
              1.0f - std::hypot(dx, dz), 0.0f, 1.0f);
            auto const ratio = terrainRatio(
              preset, noise[static_cast<std::size_t>(y * 257 + x)], edge_distance);
            heightmap.setPixelColor(x, y, QColor::fromRgbF(ratio, ratio, ratio, 1.0f));
          }
        }
        tile->setHeightmapImage(
          heightmap, base_height - height_scale, base_height + height_scale, 0, false);
        tile_height_changed = true;
        tile_height_chunks_changed = 256;
      }
      else if (_map_batch->operation == MapBatchOperation::ApplyTerrainLayout)
      {
        constexpr int visible_alpha = 8;
        constexpr int strong_alpha = 64;
        if (!_map_batch->procedural_layout)
        {
          throw std::runtime_error("Le layout procédural validé est absent.");
        }
        auto const& layout = *_map_batch->procedural_layout;
        auto const map_min_x = static_cast<float>(_map_batch->min_tile_x) * TILESIZE;
        auto const map_min_z = static_cast<float>(_map_batch->min_tile_z) * TILESIZE;
        auto const map_width = static_cast<float>(
          _map_batch->max_tile_x - _map_batch->min_tile_x + 1) * TILESIZE;
        auto const map_height = static_cast<float>(
          _map_batch->max_tile_z - _map_batch->min_tile_z + 1) * TILESIZE;
        auto normalizeWorld = [](float coordinate, float minimum, float extent)
        {
          return std::clamp((coordinate - minimum) / extent, 0.0f, 1.0f);
        };
        auto const& textures = _map_batch->procedural_textures;
        if (textures.size() != layout.texture_paths.size())
        {
          throw std::runtime_error("Les textures prévalidées du layout sont absentes.");
        }

        constexpr auto alpha_pixels_per_chunk = std::size_t{64 * 64};
        auto alphaSourceIndex = [](unsigned chunk_x, unsigned chunk_z,
                                   int alpha_x, int alpha_z)
        {
          return (static_cast<std::size_t>(chunk_z * 16 + chunk_x)
                    * alpha_pixels_per_chunk)
            + static_cast<std::size_t>(alpha_z * 64 + alpha_x);
        };
        // Preserve the source used to widen height transitions; texture masks
        // must not recompute that width from the already modified terrain.
        std::vector<float> source_alpha_heights(256 * alpha_pixels_per_chunk);
        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto const* chunk = tile->getChunk(chunk_x, chunk_z);
            for (int alpha_z = 0; alpha_z < 64; ++alpha_z)
            {
              for (int alpha_x = 0; alpha_x < 64; ++alpha_x)
              {
                source_alpha_heights[alphaSourceIndex(
                  chunk_x, chunk_z, alpha_x, alpha_z)]
                  = sampleTerrain(*chunk, alpha_x, alpha_z).height;
              }
            }
          }
        }

        std::array<std::vector<std::size_t>, 256> chunk_texture_layers;
        // ponytail: resampling is cheaper than caching every 16-layer pixel for
        // a whole tile; add a compact mask cache only if profiling needs it.
        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* chunk = tile->getChunk(chunk_x, chunk_z);
            std::array<bool, procedural_layout_max_texture_paths> active_layers{};
            for (int alpha_z = 0; alpha_z < 64; ++alpha_z)
            {
              for (int alpha_x = 0; alpha_x < 64; ++alpha_x)
              {
                auto const world_x = chunk->xbase
                  + (static_cast<float>(alpha_x) + 0.5f) * TEXDETAILSIZE;
                auto const world_z = chunk->zbase
                  + (static_cast<float>(alpha_z) + 0.5f) * TEXDETAILSIZE;
                auto const sample = sampleProceduralLayout(
                  layout,
                  normalizeWorld(world_x, map_min_x, map_width),
                  normalizeWorld(world_z, map_min_z, map_height),
                  source_alpha_heights[alphaSourceIndex(
                    chunk_x, chunk_z, alpha_x, alpha_z)],
                  0.0f, map_width, map_height);
                for (std::size_t layer = 0;
                     layer < layout.texture_paths.size(); ++layer)
                {
                  active_layers[layer] = active_layers[layer]
                    || sample.semantic_weights[layer] > 0.0f;
                }
              }
            }
            if (layout.steep)
            {
              active_layers[layout.steep->texture_layer] = true;
            }
            auto& local_layers = chunk_texture_layers[chunk_z * 16 + chunk_x];
            for (std::size_t layer = 0;
                 layer < layout.texture_paths.size(); ++layer)
            {
              if (active_layers[layer])
              {
                local_layers.push_back(layer);
              }
            }
            if (local_layers.size() > 4)
            {
              throw std::runtime_error(
                "Le chunk " + std::to_string(chunk_x) + ","
                + std::to_string(chunk_z) + " de la tuile "
                + std::to_string(tile_index.x) + ","
                + std::to_string(tile_index.z)
                + " recevrait plus de quatre textures actives.");
            }
          }
        }
        tile_was_modified = true;
        world->mapIndex.setChanged(tile);

        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* chunk = tile->getChunk(chunk_x, chunk_z);
            bool chunk_height_changed = false;
            for (auto& vertex : chunk->mVertices)
            {
              auto const u = normalizeWorld(vertex.x, map_min_x, map_width);
              auto const v = normalizeWorld(vertex.z, map_min_z, map_height);
              auto const height = sampleSmoothedProceduralLayoutHeight(
                layout, u, v, vertex.y, map_width, map_height, UNITSIZE * 0.5f);
              if (!std::isfinite(height))
              {
                throw std::runtime_error("Le layout a produit une hauteur non finie.");
              }
              if (height != vertex.y)
              {
                vertex.y = height;
                chunk_height_changed = true;
                ++tile_vertices_changed;
              }
            }
            if (chunk_height_changed)
            {
              chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
              chunk->updateVerticesData();
              tile_height_changed = true;
              ++tile_height_chunks_changed;
            }
          }
        }

        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* chunk = tile->getChunk(chunk_x, chunk_z);
            auto const& local_layers
              = chunk_texture_layers[chunk_z * 16 + chunk_x];
            auto& texture_set = ensureTextureSet(
              *chunk, _map_view->getRenderContext());
            texture_set.eraseTextures();
            for (std::size_t local = 0; local < local_layers.size(); ++local)
            {
              if (texture_set.addTexture(textures[local_layers[local]])
                  != static_cast<int>(local))
              {
                throw std::runtime_error("Impossible d'ajouter les couches du layout.");
              }
            }
            _map_batch->chunks_with_multiple_texture_layers
              += local_layers.size() > 1 ? 1 : 0;
            _map_batch->max_texture_layers = std::max(
              _map_batch->max_texture_layers, local_layers.size());

            texture_set.create_temporary_alphamaps_if_needed();
            auto* alpha = texture_set.getTempAlphamaps().get();
            bool chunk_is_mixed = false;
            for (int alpha_z = 0; alpha_z < 64; ++alpha_z)
            {
              for (int alpha_x = 0; alpha_x < 64; ++alpha_x)
              {
                auto const pixel = static_cast<std::size_t>(alpha_z * 64 + alpha_x);
                auto const terrain = sampleTerrain(*chunk, alpha_x, alpha_z);
                auto const world_x = chunk->xbase
                  + (static_cast<float>(alpha_x) + 0.5f) * TEXDETAILSIZE;
                auto const world_z = chunk->zbase
                  + (static_cast<float>(alpha_z) + 0.5f) * TEXDETAILSIZE;
                auto const sample = sampleProceduralLayout(
                  layout,
                  normalizeWorld(world_x, map_min_x, map_width),
                  normalizeWorld(world_z, map_min_z, map_height),
                  source_alpha_heights[alphaSourceIndex(
                    chunk_x, chunk_z, alpha_x, alpha_z)],
                  terrain.slope_degrees, map_width, map_height);
                for (std::size_t feature = 0; feature < layout.features.size(); ++feature)
                {
                  auto const mask = sample.feature_masks[feature];
                  if (mask >= 0.999f)
                  {
                    ++_map_batch->feature_core_pixels[feature];
                  }
                  else if (mask > 0.0f)
                  {
                    ++_map_batch->feature_transition_pixels[feature];
                  }
                }

                for (std::size_t local = 0; local < 4; ++local)
                {
                  if (alpha)
                  {
                    (*alpha)[local][pixel] = 0.0f;
                  }
                }
                auto visible_layers = 0;
                for (std::size_t local = 0; local < local_layers.size(); ++local)
                {
                  auto const global = local_layers[local];
                  auto const value = sample.quantized_weights[global];
                  if (alpha)
                  {
                    (*alpha)[local][pixel] = value;
                  }
                  _map_batch->max_texture_alpha[global] = std::max(
                    _map_batch->max_texture_alpha[global], value);
                  if (value >= visible_alpha)
                  {
                    ++visible_layers;
                    ++_map_batch->visible_texture_pixels[global];
                  }
                  if (value >= strong_alpha)
                  {
                    ++_map_batch->strong_texture_pixels[global];
                  }
                }
                if (visible_layers >= 2)
                {
                  ++_map_batch->mixed_texture_pixels;
                  chunk_is_mixed = true;
                }
                _map_batch->min_height = std::min(
                  _map_batch->min_height, terrain.height);
                _map_batch->max_height = std::max(
                  _map_batch->max_height, terrain.height);
                _map_batch->min_slope = std::min(
                  _map_batch->min_slope, terrain.slope_degrees);
                _map_batch->max_slope = std::max(
                  _map_batch->max_slope, terrain.slope_degrees);
              }
            }
            if (chunk_is_mixed)
            {
              ++_map_batch->mixed_texture_chunks;
            }
            if (local_layers.size() > 1
                && !texture_set.apply_alpha_changes())
            {
              throw std::runtime_error("Impossible d'appliquer les alphamaps du layout.");
            }
          }
        }
      }
      else if (_map_batch->operation == MapBatchOperation::ApplyLiquidLayout)
      {
        if (!_map_batch->procedural_liquid_layout)
        {
          throw std::runtime_error("Le layout liquide validé est absent.");
        }
        auto const& layout = *_map_batch->procedural_liquid_layout;
        auto const map_min_x = static_cast<float>(_map_batch->min_tile_x) * TILESIZE;
        auto const map_min_z = static_cast<float>(_map_batch->min_tile_z) * TILESIZE;
        auto const map_width = static_cast<float>(
          _map_batch->max_tile_x - _map_batch->min_tile_x + 1) * TILESIZE;
        auto const map_height = static_cast<float>(
          _map_batch->max_tile_z - _map_batch->min_tile_z + 1) * TILESIZE;
        std::set<int> effective_liquid_types;
        for (auto const& feature : layout.features)
        {
          effective_liquid_types.insert(feature.liquid_type_id);
        }
        if (!layout.replace_existing)
        {
          for (unsigned existing_z = 0; existing_z < 16; ++existing_z)
          {
            for (unsigned existing_x = 0; existing_x < 16; ++existing_x)
            {
              auto* existing = tile->Water.getChunk(existing_x, existing_z);
              for (auto const& layer : *existing->getLayers())
              {
                if (!layer.empty())
                {
                  effective_liquid_types.insert(layer.liquidID());
                }
              }
            }
          }
        }
        if (effective_liquid_types.size()
            > procedural_liquid_max_distinct_types)
        {
          throw std::runtime_error(
            "La fusion dépasserait 14 types de liquide actifs sur cette tuile. "
            "Réduis les IDs du layout ou utilise replace_existing=true pour une refonte totale.");
        }
        auto normalizeWorld = [](float coordinate, float minimum, float extent)
        {
          return std::clamp((coordinate - minimum) / extent, 0.0f, 1.0f);
        };
        constexpr auto no_feature = procedural_layout_max_features;
        constexpr std::array<std::array<float, 2>, 4> corner_offsets{{
          {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}
        }};

        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* terrain = tile->getChunk(chunk_x, chunk_z);
            auto* water = tile->Water.getChunk(chunk_x, chunk_z);
            ChunkWater::CellUpdates updates{};
            std::array<std::size_t, 8 * 8> cell_features{};
            cell_features.fill(no_feature);

            for (unsigned cell_z = 0; cell_z < 8; ++cell_z)
            {
              for (unsigned cell_x = 0; cell_x < 8; ++cell_x)
              {
                auto const cell = static_cast<std::size_t>(cell_z * 8 + cell_x);
                auto const world_x = terrain->xbase
                  + (static_cast<float>(cell_x) + 0.5f) * UNITSIZE;
                auto const world_z = terrain->zbase
                  + (static_cast<float>(cell_z) + 0.5f) * UNITSIZE;
                auto const center = sampleProceduralLiquidLayout(
                  layout,
                  normalizeWorld(world_x, map_min_x, map_width),
                  normalizeWorld(world_z, map_min_z, map_height),
                  map_width, map_height);
                if (!center.has_liquid)
                {
                  continue;
                }

                auto& update = updates[cell];
                update.touched = true;
                update.active = true;
                update.liquid_id = center.liquid_type_id;
                cell_features[cell] = center.feature_index;
                for (std::size_t corner = 0; corner < corner_offsets.size(); ++corner)
                {
                  auto const corner_x = terrain->xbase
                    + (static_cast<float>(cell_x) + corner_offsets[corner][0])
                      * UNITSIZE;
                  auto const corner_z = terrain->zbase
                    + (static_cast<float>(cell_z) + corner_offsets[corner][1])
                      * UNITSIZE;
                  auto const corner_sample = sampleProceduralLiquidFeature(
                    layout, center.feature_index,
                    normalizeWorld(corner_x, map_min_x, map_width),
                    normalizeWorld(corner_z, map_min_z, map_height),
                    map_width, map_height);
                  update.vertex_heights[corner] = corner_sample.height;
                  update.vertex_depths[corner] = corner_sample.depth;
                }
              }
            }

            std::map<std::tuple<int, unsigned, unsigned>, std::pair<float, float>>
              shared_vertices;
            for (unsigned cell_z = 0; cell_z < 8; ++cell_z)
            {
              for (unsigned cell_x = 0; cell_x < 8; ++cell_x)
              {
                auto& update = updates[cell_z * 8 + cell_x];
                if (!update.touched || !update.active)
                {
                  continue;
                }
                for (std::size_t corner = 0; corner < corner_offsets.size(); ++corner)
                {
                  auto const vertex_x = cell_x
                    + static_cast<unsigned>(corner_offsets[corner][0]);
                  auto const vertex_z = cell_z
                    + static_cast<unsigned>(corner_offsets[corner][1]);
                  auto const key = std::tuple{
                    update.liquid_id, vertex_x, vertex_z};
                  auto const [position, inserted] = shared_vertices.emplace(
                    key, std::pair{update.vertex_heights[corner],
                                   update.vertex_depths[corner]});
                  if (!inserted)
                  {
                    update.vertex_heights[corner] = position->second.first;
                    update.vertex_depths[corner] = position->second.second;
                  }
                }
              }
            }

            auto const stats = water->applyCellUpdates(
              updates, layout.replace_existing, terrain);
            _map_batch->liquid_cells_changed += stats.changed_cells;
            _map_batch->liquid_cells_cropped += stats.cropped_cells;
            _map_batch->liquid_chunks_changed += stats.changed_cells > 0 ? 1 : 0;
            tile_liquid_chunks_changed += stats.changed_cells > 0 ? 1 : 0;
            if (stats.changed_cells > 0 && !tile_was_modified)
            {
              tile_was_modified = true;
              world->mapIndex.setChanged(tile);
            }

            for (std::size_t cell = 0; cell < cell_features.size(); ++cell)
            {
              auto const feature = cell_features[cell];
              if (feature == no_feature || feature >= layout.features.size())
              {
                continue;
              }
              auto const x = static_cast<int>(cell % 8);
              auto const z = static_cast<int>(cell / 8);
              auto const id = static_cast<int>(layout.features[feature].liquid_type_id);
              auto const visible = std::any_of(
                water->getLayers()->begin(), water->getLayers()->end(),
                [=](liquid_layer const& layer)
                {
                  return layer.liquidID() == id && layer.hasSubchunk(x, z);
                });
              _map_batch->liquid_feature_cells[feature] += visible ? 1 : 0;
            }
          }
        }
      }
      else if (_map_batch->operation == MapBatchOperation::ScatterAssets)
      {
        auto const& scatter = *_map_batch->procedural_scatter;
        auto const map_width = static_cast<float>(
          _map_batch->max_tile_x - _map_batch->min_tile_x + 1) * TILESIZE;
        auto const map_height = static_cast<float>(
          _map_batch->max_tile_z - _map_batch->min_tile_z + 1) * TILESIZE;
        auto const map_min_x = static_cast<float>(_map_batch->min_tile_x) * TILESIZE;
        auto const map_min_z = static_cast<float>(_map_batch->min_tile_z) * TILESIZE;
        auto const short_side = std::min(map_width, map_height);
        auto const u_min = (static_cast<float>(tile_index.x) * TILESIZE - map_min_x) / map_width;
        auto const v_min = (static_cast<float>(tile_index.z) * TILESIZE - map_min_z) / map_height;
        auto const u_max = (static_cast<float>(tile_index.x + 1) * TILESIZE - map_min_x) / map_width;
        auto const v_max = (static_cast<float>(tile_index.z + 1) * TILESIZE - map_min_z) / map_height;

        auto rememberInstance = [&](auto const& instance)
        {
          if (_map_batch->known_instance_uids.insert(instance.uid).second)
            _map_batch->occupied_positions.emplace_back(
              glm::vec2{instance.pos.x, instance.pos.z}, 0.0f);
        };
        auto const wall_batch = std::all_of(
          scatter.regions.begin(), scatter.regions.end(),
          [](ProceduralScatterRegion const& region)
          {
            return proceduralScatterIsWallRegion(region);
          });
        if (!wall_batch)
        {
          world->getModelInstanceStorage().for_each_m2_instance(rememberInstance);
          world->getModelInstanceStorage().for_each_wmo_instance(rememberInstance);
        }

        for (std::size_t region_index = 0; region_index < scatter.regions.size(); ++region_index)
        {
          auto const& region = scatter.regions[region_index];
          for (std::size_t candidate_index = 0;
               candidate_index < region.density_per_tile; ++candidate_index)
          {
            ++_map_batch->scatter_candidates;
            auto const candidate = proceduralScatterCandidate(
              scatter, region_index, tile_index.x, tile_index.z, candidate_index,
              u_min, u_max, v_min, v_max);
            if (!candidate.active)
            {
              ++_map_batch->scatter_rejected_outside;
              continue;
            }
            if (!proceduralScatterIsWallRegion(region)
                && !proceduralScatterContains(region.points, candidate.u, candidate.v))
            {
              ++_map_batch->scatter_rejected_outside;
              continue;
            }
            if (proceduralScatterExcluded(
                  scatter, candidate.u, candidate.v, map_width, map_height))
            {
              ++_map_batch->scatter_rejected_exclusion;
              continue;
            }

            auto const world_x = map_min_x + candidate.u * map_width;
            auto const world_z = map_min_z + candidate.v * map_height;
            auto const local_x = std::clamp(
              world_x - static_cast<float>(tile_index.x) * TILESIZE,
              0.0f, TILESIZE - std::numeric_limits<float>::epsilon());
            auto const local_z = std::clamp(
              world_z - static_cast<float>(tile_index.z) * TILESIZE,
              0.0f, TILESIZE - std::numeric_limits<float>::epsilon());
            auto const chunk_x = std::clamp(static_cast<int>(local_x / CHUNKSIZE), 0, 15);
            auto const chunk_z = std::clamp(static_cast<int>(local_z / CHUNKSIZE), 0, 15);
            auto* chunk = tile->getChunk(chunk_x, chunk_z);
            auto const chunk_local_x = local_x - chunk_x * CHUNKSIZE;
            auto const chunk_local_z = local_z - chunk_z * CHUNKSIZE;
            auto const alpha_x = std::clamp(
              static_cast<int>(chunk_local_x / TEXDETAILSIZE), 0, 63);
            auto const alpha_z = std::clamp(
              static_cast<int>(chunk_local_z / TEXDETAILSIZE), 0, 63);
            auto const terrain = sampleTerrain(*chunk, alpha_x, alpha_z);
            if (terrain.height < region.min_height || terrain.height > region.max_height
                || terrain.slope_degrees < region.min_slope_degrees
                || terrain.slope_degrees > region.max_slope_degrees)
            {
              ++_map_batch->scatter_rejected_terrain;
              continue;
            }

            auto const cell_x = std::clamp(alpha_x / 8, 0, 7);
            auto const cell_z = std::clamp(alpha_z / 8, 0, 7);
            auto const* layers = chunk->liquid_chunk()->getLayers();
            if (std::any_of(layers->begin(), layers->end(), [&](liquid_layer const& layer)
                { return layer.hasSubchunk(cell_x, cell_z); }))
            {
              ++_map_batch->scatter_rejected_liquid;
              continue;
            }

            auto const& asset = scatter.assets[candidate.asset_index];
            auto const is_wall_region = proceduralScatterIsWallRegion(region);
            auto const spacing = region.min_spacing_ratio * asset.spacing_multiplier
              * std::max(candidate.scale, 0.5f) * short_side;
            auto const too_close = !is_wall_region && std::any_of(
                _map_batch->occupied_positions.begin(), _map_batch->occupied_positions.end(),
                [&](std::pair<glm::vec2, float> const& occupied)
                {
                  auto const& position = occupied.first;
                  auto const dx = position.x - world_x;
                  auto const dz = position.y - world_z;
                  auto const required = std::max(spacing, occupied.second);
                  return dx * dx + dz * dz < required * required;
                });
            // ponytail: bounded O(n²) scan (16384 candidates); use a spatial
            // hash only if real maps make this batch measurably slow.
            if (too_close)
            {
              ++_map_batch->scatter_rejected_spacing;
              continue;
            }

            auto const position = glm::vec3{world_x, terrain.height, world_z};
            auto const rotation = math::degrees::vec3{0.0f, candidate.yaw_degrees, 0.0f};
            if (asset.path.ends_with(".m2"))
              world->addM2(asset.path, position, candidate.scale, rotation, nullptr, false);
            else
              world->addWMO(asset.path, position, candidate.scale, rotation, nullptr, false);
            if (!is_wall_region)
              _map_batch->occupied_positions.emplace_back(
                glm::vec2{world_x, world_z}, spacing * 0.45f);
            ++_map_batch->scatter_placed;
            ++_map_batch->scatter_by_asset[asset.path];
            ++_map_batch->scatter_by_region[region.name];
            tile_was_modified = true;
          }
        }
      }
      else if (_map_batch->operation == MapBatchOperation::PlaceProps)
      {
        auto const& props = *_map_batch->procedural_props;
        auto const map_width = static_cast<float>(
          _map_batch->max_tile_x - _map_batch->min_tile_x + 1) * TILESIZE;
        auto const map_height = static_cast<float>(
          _map_batch->max_tile_z - _map_batch->min_tile_z + 1) * TILESIZE;
        auto const map_min_x = static_cast<float>(_map_batch->min_tile_x) * TILESIZE;
        auto const map_min_z = static_cast<float>(_map_batch->min_tile_z) * TILESIZE;
        auto const u_min = (static_cast<float>(tile_index.x) * TILESIZE - map_min_x) / map_width;
        auto const v_min = (static_cast<float>(tile_index.z) * TILESIZE - map_min_z) / map_height;
        auto const u_max = (static_cast<float>(tile_index.x + 1) * TILESIZE - map_min_x) / map_width;
        auto const v_max = (static_cast<float>(tile_index.z + 1) * TILESIZE - map_min_z) / map_height;
        for (auto const& prop : props.props)
        {
          if (prop.u < u_min || prop.u >= u_max
              || prop.v < v_min || prop.v >= v_max)
            continue;
          auto const world_x = map_min_x + prop.u * map_width;
          auto const world_z = map_min_z + prop.v * map_height;
          auto const local_x = std::clamp(
            world_x - static_cast<float>(tile_index.x) * TILESIZE,
            0.0f, TILESIZE - std::numeric_limits<float>::epsilon());
          auto const local_z = std::clamp(
            world_z - static_cast<float>(tile_index.z) * TILESIZE,
            0.0f, TILESIZE - std::numeric_limits<float>::epsilon());
          auto const chunk_x = std::clamp(static_cast<int>(local_x / CHUNKSIZE), 0, 15);
          auto const chunk_z = std::clamp(static_cast<int>(local_z / CHUNKSIZE), 0, 15);
          auto* chunk = tile->getChunk(chunk_x, chunk_z);
          auto const chunk_local_x = local_x - chunk_x * CHUNKSIZE;
          auto const chunk_local_z = local_z - chunk_z * CHUNKSIZE;
          auto const alpha_x = std::clamp(
            static_cast<int>(chunk_local_x / TEXDETAILSIZE), 0, 63);
          auto const alpha_z = std::clamp(
            static_cast<int>(chunk_local_z / TEXDETAILSIZE), 0, 63);
          auto const terrain = sampleTerrain(*chunk, alpha_x, alpha_z);
          auto const position = glm::vec3{
            world_x, terrain.height + prop.height_offset, world_z};
          auto const rotation = math::degrees::vec3{0.0f, prop.yaw_degrees, 0.0f};
          if (prop.path.ends_with(".m2"))
            world->addM2(prop.path, position, prop.scale, rotation, nullptr, false);
          else
            world->addWMO(prop.path, position, prop.scale, rotation, nullptr, false);
          ++_map_batch->props_placed;
          ++_map_batch->props_by_path[prop.path];
          tile_was_modified = true;
        }
      }
      else if (_map_batch->operation == MapBatchOperation::ApplyGroundEffect)
      {
        auto const texture_path = _map_batch->arguments.at("texture_path").get<std::string>();
        auto const effect_id = _map_batch->arguments.at("effect_id").get<unsigned>();
        auto const overwrite = _map_batch->arguments.at("overwrite").get<bool>();
        for (unsigned z = 0; z < 16; ++z)
        {
          for (unsigned x = 0; x < 16; ++x)
          {
            auto* chunk = tile->getChunk(x, z);
            if (chunk->setGroundEffectForTexture(texture_path, effect_id, overwrite))
            {
              if (!tile_was_modified) world->mapIndex.setChanged(tile);
              tile_was_modified = true;
              ++_map_batch->chunks_changed;
            }
            auto* textures = chunk->getTextureSet();
            if (!textures) continue;
            for (std::size_t layer = 0; layer < textures->num(); ++layer)
            {
              if (textures->filename(layer) == texture_path
                  && textures->getEffectForLayer(layer) == effect_id)
                ++_map_batch->ground_effect_layers_matched;
            }
          }
        }
        // The reserved default can migrate to new doodad models while keeping
        // the same effect ID, so rebuilding cannot depend only on MCLY changes.
        tile->renderer()->invalidateGroundEffectPreview();
      }
      else if (_map_batch->operation == MapBatchOperation::BlendTerrainTextures)
      {
        constexpr int alpha_tile_size = 16 * 64;
        constexpr float noise_scale_world = 120.0f;
        constexpr int visible_alpha = 8;

        auto const seed = _map_batch->arguments.at("seed").get<std::string>();
        auto const low_height = static_cast<float>(
          _map_batch->arguments.at("low_height").get<double>());
        auto const blend_width = static_cast<float>(
          _map_batch->arguments.at("blend_width").get<double>());
        auto const slope_start = static_cast<float>(
          _map_batch->arguments.at("slope_start_degrees").get<double>());
        auto const slope_full = static_cast<float>(
          _map_batch->arguments.at("slope_full_degrees").get<double>());
        auto const noise_strength = static_cast<float>(
          _map_batch->arguments.at("noise_strength").get<double>());
        auto const has_high = !_map_batch->arguments.at("high_texture_path").is_null();
        auto const high_height = has_high
          ? static_cast<float>(_map_batch->arguments.at("high_height").get<double>())
          : 0.0f;

        std::vector<std::string> paths{
          _map_batch->arguments.at("base_texture_path").get<std::string>(),
          _map_batch->arguments.at("low_texture_path").get<std::string>(),
          _map_batch->arguments.at("steep_texture_path").get<std::string>()
        };
        if (has_high)
        {
          paths.push_back(
            _map_batch->arguments.at("high_texture_path").get<std::string>());
        }
        std::vector<scoped_blp_texture_reference> textures;
        textures.reserve(paths.size());
        for (auto const& path : paths)
        {
          textures.emplace_back(path, _map_view->getRenderContext());
        }

        auto generator = FastNoise::New<FastNoise::Simplex>();
        std::vector<float> noise(alpha_tile_size * alpha_tile_size);
        generator->GenUniformGrid2D(
          noise.data(),
          static_cast<int>(tile_index.x * alpha_tile_size),
          static_cast<int>(tile_index.z * alpha_tile_size),
          alpha_tile_size, alpha_tile_size,
          TEXDETAILSIZE / noise_scale_world, stableSeed(seed));

        for (unsigned chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* chunk = tile->getChunk(chunk_x, chunk_z);
            auto& texture_set = ensureTextureSet(
              *chunk, _map_view->getRenderContext());
            texture_set.eraseTextures();
            for (std::size_t layer = 0; layer < textures.size(); ++layer)
            {
              if (texture_set.addTexture(textures[layer]) != static_cast<int>(layer))
              {
                throw std::runtime_error("Impossible d'ajouter les couches de texture.");
              }
            }
            ++_map_batch->chunks_with_multiple_texture_layers;
            _map_batch->max_texture_layers = std::max(
              _map_batch->max_texture_layers, paths.size());

            texture_set.create_temporary_alphamaps_if_needed();
            auto& alpha = *texture_set.getTempAlphamaps();
            bool chunk_is_mixed = false;
            for (int alpha_z = 0; alpha_z < 64; ++alpha_z)
            {
              for (int alpha_x = 0; alpha_x < 64; ++alpha_x)
              {
                auto const pixel = static_cast<std::size_t>(alpha_z * 64 + alpha_x);
                auto const noise_x = static_cast<int>(chunk_x) * 64 + alpha_x;
                auto const noise_z = static_cast<int>(chunk_z) * 64 + alpha_z;
                auto const noise_index = static_cast<std::size_t>(
                  noise_z * alpha_tile_size + noise_x);
                auto const terrain = sampleTerrain(*chunk, alpha_x, alpha_z);
                _map_batch->min_height = std::min(
                  _map_batch->min_height, terrain.height);
                _map_batch->max_height = std::max(
                  _map_batch->max_height, terrain.height);
                _map_batch->min_slope = std::min(
                  _map_batch->min_slope, terrain.slope_degrees);
                _map_batch->max_slope = std::max(
                  _map_batch->max_slope, terrain.slope_degrees);
                auto const alphas = textureBlendAlphas(
                  terrain.height, terrain.slope_degrees, noise[noise_index],
                  low_height, high_height, has_high, blend_width,
                  slope_start, slope_full, noise_strength);

                auto visible_layers = 0;
                for (std::size_t layer = 0; layer < 4; ++layer)
                {
                  alpha[layer][pixel] = alphas[layer];
                  if (layer < paths.size()
                      && alphas[layer] >= visible_alpha)
                  {
                    ++visible_layers;
                    ++_map_batch->visible_texture_pixels[layer];
                  }
                }
                if (visible_layers >= 2)
                {
                  ++_map_batch->mixed_texture_pixels;
                  chunk_is_mixed = true;
                }
              }
            }
            if (chunk_is_mixed)
            {
              ++_map_batch->mixed_texture_chunks;
            }
            if (!texture_set.apply_alpha_changes())
            {
              throw std::runtime_error("Impossible d'appliquer les alphamaps.");
            }
          }
        }
      }
      else
      {
        auto texture = scoped_blp_texture_reference(
          _map_batch->arguments.at("texture_path").get<std::string>(),
          _map_view->getRenderContext());
        for (unsigned z = 0; z < 16; ++z)
        {
          for (unsigned x = 0; x < 16; ++x)
          {
            auto* chunk = tile->getChunk(x, z);
            chunk->eraseTextures();
            chunk->addTexture(texture);
          }
        }
      }

      if (!isValidation(_map_batch->operation) && tile_was_modified)
      {
        world->mapIndex.saveTile(tile_index, world);
        tile->changed = false;
      }
      if (tile_height_changed)
      {
        _map_batch->height_changed_tiles.push_back(tile_index);
        _map_batch->height_chunks_changed += tile_height_chunks_changed;
        _map_batch->vertices_changed += tile_vertices_changed;
      }
      if (isValidation(_map_batch->operation))
      {
        ++_map_batch->tiles_changed;
        _map_batch->chunks_changed += 256;
      }
      else if (_map_batch->operation == MapBatchOperation::ApplyLiquidLayout)
      {
        _map_batch->tiles_changed += tile_was_modified ? 1 : 0;
        _map_batch->chunks_changed += tile_liquid_chunks_changed;
      }
      else if (_map_batch->operation == MapBatchOperation::ScatterAssets)
      {
        _map_batch->tiles_changed += tile_was_modified ? 1 : 0;
      }
      else if (_map_batch->operation == MapBatchOperation::ApplyGroundEffect)
      {
        _map_batch->tiles_changed += tile_was_modified ? 1 : 0;
      }
      else
      {
        ++_map_batch->tiles_changed;
        _map_batch->chunks_changed += 256;
      }
      if (!_map_batch->keep_loaded[_map_batch->next_tile])
      {
        world->mapIndex.unloadTile(tile_index);
      }
      ++_map_batch->next_tile;
    }
    catch (std::exception const& exception)
    {
      ++_map_batch->failures;
      _map_batch->failed_tiles.insert(tile_index);
      finishMapBatch({
        {"ok", false},
        {"partial_change_possible", !isValidation(_map_batch->operation)},
        {"current_tile_unsaved", !isValidation(_map_batch->operation)},
        {"error", std::string{"Le traitement de la carte a échoué : "} + exception.what()}
      });
      return;
    }
    catch (...)
    {
      ++_map_batch->failures;
      _map_batch->failed_tiles.insert(tile_index);
      finishMapBatch({
        {"ok", false},
        {"partial_change_possible", !isValidation(_map_batch->operation)},
        {"current_tile_unsaved", !isValidation(_map_batch->operation)},
        {"error", "Le traitement de la carte a échoué."}
      });
      return;
    }

    _status->setText(tr("%1 : %2/%3 tuile(s), %4 chunks traités…")
                       .arg(QString::fromStdString(_map_batch->call.name))
                       .arg(_map_batch->next_tile).arg(_map_batch->tiles.size())
                       .arg(_map_batch->chunks_changed));
    QTimer::singleShot(0, this, [this] { processMapBatch(); });
  }

  void AssistantDock::finishMapBatch(nlohmann::json result)
  {
    if (!_map_batch)
    {
      return;
    }

    auto batch = std::move(*_map_batch);
    _map_batch.reset();
    if (_map_view && _map_view->getWorld())
    {
      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(
        ::gl, _map_view->context());
      auto* world = _map_view->getWorld();
      if (!isValidation(batch.operation))
      {
        if (batch.operation == MapBatchOperation::ScatterAssets)
        {
          world->mapIndex.saveChanged(world);
        }
        world->horizon.save_wdl(world);
        _map_view->invalidate();
      }
      for (std::size_t i = 0; i < batch.tiles.size(); ++i)
      {
        if (!batch.keep_loaded[i]
            && world->mapIndex.tileLoaded(batch.tiles[i])
            && !world->mapIndex.has_unsaved_changes(batch.tiles[i]))
        {
          world->mapIndex.unloadTile(batch.tiles[i]);
        }
      }
    }
    if (_map_view)
    {
      _map_view->setEnabled(batch.map_view_was_enabled);
    }

    if (result.empty())
    {
      if (batch.operation == MapBatchOperation::Validate)
      {
        result = {
          {"ok", batch.failures == 0
            && batch.tiles_changed == batch.tiles.size()
            && batch.chunks_changed == batch.tiles.size() * 256
            && batch.chunks_without_texture == 0},
          {"operation", "validate_map"},
          {"tiles_total", batch.tiles.size()},
          {"tiles_inspected", batch.tiles_changed},
          {"chunks_inspected", batch.chunks_changed},
          {"vertices_inspected", batch.vertices_inspected},
          {"chunks_without_texture", batch.chunks_without_texture},
          {"chunks_with_multiple_texture_layers", batch.chunks_with_multiple_texture_layers},
          {"mixed_texture_chunks", batch.mixed_texture_chunks},
          {"mixed_texture_pixels", batch.mixed_texture_pixels},
          {"max_texture_layers", batch.max_texture_layers},
          {"mixed_alpha_threshold", 8},
          {"tiles_failed", batch.failed_tiles.size()},
          {"min_height", batch.vertices_inspected > 0
            ? nlohmann::json(batch.min_height) : nlohmann::json(nullptr)},
          {"max_height", batch.vertices_inspected > 0
            ? nlohmann::json(batch.max_height) : nlohmann::json(nullptr)},
          {"saved", false}
        };
      }
      else
      {
        auto blend_is_visible = true;
        if (batch.operation == MapBatchOperation::BlendTerrainTextures)
        {
          auto const layers = batch.arguments.at("high_texture_path").is_null() ? 3U : 4U;
          blend_is_visible = batch.mixed_texture_pixels > 0;
          for (std::size_t layer = 0; layer < layers; ++layer)
          {
            blend_is_visible = blend_is_visible
              && batch.visible_texture_pixels[layer] > 0;
          }
        }
        auto layout_is_valid = true;
        if (batch.operation == MapBatchOperation::ApplyTerrainLayout)
        {
          layout_is_valid = std::all_of(
            batch.feature_core_pixels.begin(), batch.feature_core_pixels.end(),
            [](std::size_t pixels) { return pixels > 0; });
          if (batch.procedural_layout)
          {
            for (std::size_t layer = 0;
                 layer < batch.procedural_layout->texture_paths.size(); ++layer)
            {
              layout_is_valid = layout_is_valid
                && batch.strong_texture_pixels[layer] >= 64
                && batch.max_texture_alpha[layer] >= 64;
            }
          }
          else
          {
            layout_is_valid = false;
          }
        }
        auto liquid_layout_is_valid = true;
        if (batch.operation == MapBatchOperation::ApplyLiquidLayout)
        {
          liquid_layout_is_valid = batch.procedural_liquid_layout
            && std::all_of(
              batch.liquid_feature_cells.begin(), batch.liquid_feature_cells.end(),
              [](std::size_t cells) { return cells > 0; });
        }
        auto const scatter_is_valid = batch.operation != MapBatchOperation::ScatterAssets
          || batch.scatter_placed > 0;
        auto const props_are_valid = batch.operation != MapBatchOperation::PlaceProps
          || batch.props_placed > 0;
        auto const ground_effect_is_valid
          = batch.operation != MapBatchOperation::ApplyGroundEffect
            || batch.ground_effect_layers_matched > 0;
        auto const normals_are_current = !changesHeight(batch.operation)
          || batch.normals_recalculated == batch.normal_tiles.size();
        result = {
          {"ok", batch.failures == 0
            && normals_are_current && blend_is_visible && layout_is_valid
            && liquid_layout_is_valid && scatter_is_valid && props_are_valid
            && ground_effect_is_valid},
          {"operation", batch.call.name},
          {"tiles_total", batch.tiles.size()},
          {"tiles_changed", batch.tiles_changed},
          {"chunks_changed", batch.chunks_changed},
          {"tiles_failed", batch.failed_tiles.size()},
          {"covers_entire_map", batch.tiles_changed == batch.tiles.size()},
          {"undoable", false},
          {"saved", batch.tiles_changed > 0}
        };
      }
      if (batch.failures > 0)
      {
        result["error"] = "Une ou plusieurs tuiles n'ont pas pu être chargées.";
      }
      else if (batch.operation == MapBatchOperation::BlendTerrainTextures
               && !result.at("ok").get<bool>())
      {
        result["error"] = "Le mélange a été enregistré, mais au moins un rôle de texture n'est pas visible ou aucun pixel n'est réellement mélangé. Ajuste les seuils à la plage de hauteurs et de pentes puis relance l'outil.";
        result["partial_change_possible"] = true;
      }
      else if (batch.operation == MapBatchOperation::ApplyTerrainLayout
               && !result.at("ok").get<bool>())
      {
        result["error"] = "Le layout a été enregistré, mais une feature n'a aucun cœur effectif ou une texture n'atteint pas 64 pixels fortement visibles. Ne relance pas automatiquement la géométrie : rapporte les compteurs et demande une correction explicite.";
        result["partial_change_possible"] = true;
      }
      else if (batch.operation == MapBatchOperation::ApplyLiquidLayout
               && !result.at("ok").get<bool>())
      {
        result["error"] = "Le layout liquide a été enregistré, mais au moins une feature ne possède aucune cellule MH2O visible après découpe par le terrain. Rapporte les compteurs et corrige le niveau ou la largeur.";
        result["partial_change_possible"] = true;
      }
      else if (batch.operation == MapBatchOperation::ScatterAssets
               && !result.at("ok").get<bool>())
      {
        result["error"] = "Aucun asset n'a été placé. Vérifie les régions, exclusions, pentes, hauteurs et l'espacement.";
      }
      else if (batch.operation == MapBatchOperation::ApplyGroundEffect
               && !result.at("ok").get<bool>())
      {
        result["error"] = "La texture cible n'est utilisée par aucun chunk ; aucun GroundEffect n'a été appliqué.";
      }
      if (batch.operation == MapBatchOperation::GenerateTerrain)
      {
        result["preset"] = batch.arguments.at("preset");
        result["seed"] = batch.arguments.at("seed");
        result["tiles_with_normals_recalculated"] = batch.normals_recalculated;
      }
      else if (batch.operation == MapBatchOperation::SetBaseTexture)
      {
        result["texture_path"] = batch.arguments.at("texture_path");
      }
      else if (batch.operation == MapBatchOperation::ApplyGroundEffect)
      {
        result["texture_path"] = batch.arguments.at("texture_path");
        result["effect_id"] = batch.arguments.at("effect_id");
        result["overwrite"] = batch.arguments.at("overwrite");
        result["matching_layers"] = batch.ground_effect_layers_matched;
        result["noggit_preview_supported"] = true;
      }
    }
    else
    {
      result["operation"] = batch.call.name;
      result["tiles_total"] = batch.tiles.size();
      result[isValidation(batch.operation) ? "tiles_inspected" : "tiles_changed"]
        = batch.tiles_changed;
      result[isValidation(batch.operation) ? "chunks_inspected" : "chunks_changed"]
        = batch.chunks_changed;
      result["tiles_failed"] = batch.failed_tiles.size();
      result["undoable"] = false;
      result["saved"] = !isValidation(batch.operation) && batch.tiles_changed > 0;
      if (changesHeight(batch.operation))
      {
        result["tiles_with_normals_recalculated"] = batch.normals_recalculated;
      }
    }

    if (batch.operation == MapBatchOperation::BlendTerrainTextures)
    {
      auto paths = nlohmann::json::array({
        batch.arguments.at("base_texture_path"),
        batch.arguments.at("low_texture_path"),
        batch.arguments.at("steep_texture_path")
      });
      if (!batch.arguments.at("high_texture_path").is_null())
      {
        paths.push_back(batch.arguments.at("high_texture_path"));
      }
      auto visible_pixels = nlohmann::json::array();
      for (std::size_t layer = 0; layer < paths.size(); ++layer)
      {
        visible_pixels.push_back(batch.visible_texture_pixels[layer]);
      }
      result["texture_paths"] = std::move(paths);
      result["layers_requested"] = visible_pixels.size();
      result["visible_texture_pixels"] = std::move(visible_pixels);
      result["chunks_with_multiple_texture_layers"]
        = batch.chunks_with_multiple_texture_layers;
      result["mixed_texture_chunks"] = batch.mixed_texture_chunks;
      result["mixed_texture_pixels"] = batch.mixed_texture_pixels;
      result["mixed_alpha_threshold"] = 8;
      result["seed"] = batch.arguments.at("seed");
      auto const sampled = batch.min_height <= batch.max_height
        && batch.min_slope <= batch.max_slope;
      result["sampled_min_height"] = sampled
        ? nlohmann::json(batch.min_height) : nlohmann::json(nullptr);
      result["sampled_max_height"] = sampled
        ? nlohmann::json(batch.max_height) : nlohmann::json(nullptr);
      result["sampled_min_slope_degrees"] = sampled
        ? nlohmann::json(batch.min_slope) : nlohmann::json(nullptr);
      result["sampled_max_slope_degrees"] = sampled
        ? nlohmann::json(batch.max_slope) : nlohmann::json(nullptr);
    }
    else if (batch.operation == MapBatchOperation::ApplyTerrainLayout
             && batch.procedural_layout)
    {
      auto feature_stats = nlohmann::json::array();
      auto features_with_core = std::size_t{0};
      for (std::size_t feature = 0;
           feature < batch.procedural_layout->features.size(); ++feature)
      {
        auto const core = batch.feature_core_pixels[feature];
        features_with_core += core > 0 ? 1 : 0;
        auto const& definition = batch.procedural_layout->features[feature];
        feature_stats.push_back({
          {"name", definition.name},
          {"shape", definition.shape == ProceduralLayoutShape::Area
            ? "area" : "corridor"},
          {"height_mode", definition.height_mode == ProceduralLayoutHeightMode::Offset
            ? "offset" : "absolute"},
          {"point_count", definition.points.size()},
          {"half_width_ratio", definition.half_width_ratio},
          {"transition_width_ratio", definition.transition_width_ratio},
          {"texture_layer", definition.texture_layer},
          {"roughness_amplitude", definition.roughness_amplitude},
          {"texture_strength", definition.texture_strength},
          {"width_variation_ratio", definition.width_variation_ratio},
          {"priority", definition.priority},
          {"effective_core_pixels", core},
          {"effective_transition_pixels", batch.feature_transition_pixels[feature]}
        });
      }

      auto texture_stats = nlohmann::json::array();
      auto layers_with_strong_coverage = std::size_t{0};
      for (std::size_t layer = 0;
           layer < batch.procedural_layout->texture_paths.size(); ++layer)
      {
        layers_with_strong_coverage += batch.strong_texture_pixels[layer] >= 64
          && batch.max_texture_alpha[layer] >= 64 ? 1 : 0;
        texture_stats.push_back({
          {"layer", layer},
          {"path", batch.procedural_layout->texture_paths[layer]},
          {"visible_pixels", batch.visible_texture_pixels[layer]},
          {"strong_pixels", batch.strong_texture_pixels[layer]},
          {"max_alpha", batch.max_texture_alpha[layer]}
        });
      }

      auto const map_min_x = static_cast<double>(batch.min_tile_x) * TILESIZE;
      auto const map_min_z = static_cast<double>(batch.min_tile_z) * TILESIZE;
      auto const map_max_x = static_cast<double>(batch.max_tile_x + 1) * TILESIZE;
      auto const map_max_z = static_cast<double>(batch.max_tile_z + 1) * TILESIZE;
      result["features"] = std::move(feature_stats);
      result["features_requested"] = batch.procedural_layout->features.size();
      result["features_with_core"] = features_with_core;
      result["feature_mask_semantics"] = "effective_after_priority_composition";
      result["edge_noise_ratio"] = batch.procedural_layout->edge_noise_ratio;
      result["max_slope_degrees"] = batch.procedural_layout->max_slope_degrees
        ? nlohmann::json(*batch.procedural_layout->max_slope_degrees)
        : nlohmann::json(nullptr);
      result["smoothing_strength"] = batch.procedural_layout->smoothing_strength;
      result["textures"] = std::move(texture_stats);
      result["layers_requested"] = batch.procedural_layout->texture_paths.size();
      result["map_palette_limit"] = procedural_layout_max_texture_paths;
      result["chunk_texture_limit"] = 4;
      result["layers_with_strong_coverage"] = layers_with_strong_coverage;
      result["visible_alpha_threshold"] = 8;
      result["strong_alpha_threshold"] = 64;
      result["minimum_strong_pixels_per_layer"] = 64;
      result["mixed_texture_chunks"] = batch.mixed_texture_chunks;
      result["mixed_texture_pixels"] = batch.mixed_texture_pixels;
      result["chunks_with_multiple_texture_layers"]
        = batch.chunks_with_multiple_texture_layers;
      result["tiles_with_height_changes"] = batch.height_changed_tiles.size();
      result["height_chunks_changed"] = batch.height_chunks_changed;
      result["vertices_changed"] = batch.vertices_changed;
      result["normal_tiles_total"] = batch.normal_tiles.size();
      result["tiles_with_normals_recalculated"] = batch.normals_recalculated;
      result["map_bounds_world"] = {
        {"min_x", map_min_x}, {"min_z", map_min_z},
        {"max_x", map_max_x}, {"max_z", map_max_z},
        {"width", map_max_x - map_min_x}, {"height", map_max_z - map_min_z}
      };
      auto const sampled = batch.min_height <= batch.max_height
        && batch.min_slope <= batch.max_slope;
      result["sampled_min_height"] = sampled
        ? nlohmann::json(batch.min_height) : nlohmann::json(nullptr);
      result["sampled_max_height"] = sampled
        ? nlohmann::json(batch.max_height) : nlohmann::json(nullptr);
      result["sampled_min_slope_degrees"] = sampled
        ? nlohmann::json(batch.min_slope) : nlohmann::json(nullptr);
      result["sampled_max_slope_degrees"] = sampled
        ? nlohmann::json(batch.max_slope) : nlohmann::json(nullptr);
    }
    else if (batch.operation == MapBatchOperation::ApplyLiquidLayout
             && batch.procedural_liquid_layout)
    {
      auto features = nlohmann::json::array();
      for (std::size_t index = 0;
           index < batch.procedural_liquid_layout->features.size(); ++index)
      {
        auto const& feature = batch.procedural_liquid_layout->features[index];
        features.push_back({
          {"name", feature.name},
          {"shape", feature.shape == ProceduralLayoutShape::Area
            ? "area" : "corridor"},
          {"liquid_type_id", feature.liquid_type_id},
          {"liquid_name", LiquidTypeDB::getLiquidName(feature.liquid_type_id)},
          {"priority", feature.priority},
          {"cells_visible", batch.liquid_feature_cells[index]}
        });
      }
      result["features"] = std::move(features);
      result["features_requested"] = batch.procedural_liquid_layout->features.size();
      result["features_with_visible_cells"] = std::count_if(
        batch.liquid_feature_cells.begin(), batch.liquid_feature_cells.end(),
        [](std::size_t cells) { return cells > 0; });
      result["replace_existing"] = batch.procedural_liquid_layout->replace_existing;
      result["edge_noise_ratio"] = batch.procedural_liquid_layout->edge_noise_ratio;
      result["liquid_chunks_changed"] = batch.liquid_chunks_changed;
      result["liquid_cells_changed"] = batch.liquid_cells_changed;
      result["liquid_cells_cropped"] = batch.liquid_cells_cropped;
      result["format"] = "MH2O";
    }
    else if (batch.operation == MapBatchOperation::ScatterAssets
             && batch.procedural_scatter)
    {
      auto assets = nlohmann::json::array();
      for (auto const& asset : batch.procedural_scatter->assets)
      {
        assets.push_back({
          {"path", asset.path},
          {"placed", batch.scatter_by_asset[asset.path]}
        });
      }
      auto regions = nlohmann::json::array();
      for (auto const& region : batch.procedural_scatter->regions)
      {
        regions.push_back({
          {"name", region.name},
          {"placed", batch.scatter_by_region[region.name]}
        });
      }
      result["seed"] = batch.procedural_scatter->seed;
      result["assets"] = std::move(assets);
      result["regions"] = std::move(regions);
      result["candidates"] = batch.scatter_candidates;
      result["instances_placed"] = batch.scatter_placed;
      result["rejected"] = {
        {"outside_regions", batch.scatter_rejected_outside},
        {"exclusions", batch.scatter_rejected_exclusion},
        {"terrain_filters", batch.scatter_rejected_terrain},
        {"liquid", batch.scatter_rejected_liquid},
        {"spacing_or_existing_objects", batch.scatter_rejected_spacing}
      };
      result["tiles_processed"] = batch.next_tile;
      result["avoids_visible_liquid"] = true;
    }
    else if (batch.operation == MapBatchOperation::PlaceProps
             && batch.procedural_props)
    {
      auto paths = nlohmann::json::array();
      for (auto const& [path, placed] : batch.props_by_path)
        paths.push_back({{"path", path}, {"placed", placed}});
      result["props_requested"] = batch.procedural_props->props.size();
      result["props_placed"] = batch.props_placed;
      result["props_by_path"] = std::move(paths);
      result["props_outside_footprint"]
        = batch.procedural_props->props.size() - batch.props_placed;
    }
    else if (batch.operation == MapBatchOperation::Validate)
    {
      auto textures = nlohmann::json::array();
      for (auto const& [path, max_alpha] : batch.max_texture_alpha_by_path)
      {
        textures.push_back({
          {"texture_path", path},
          {"chunks", batch.texture_chunks_by_path[path]},
          {"visible_pixels", batch.visible_texture_pixels_by_path[path]},
          {"strong_pixels", batch.strong_texture_pixels_by_path[path]},
          {"max_alpha", max_alpha}
        });
      }
      result["textures_by_path"] = std::move(textures);
      auto ground_effects = nlohmann::json::array();
      for (auto const& [id, layers] : batch.ground_effect_layers_by_id)
      {
        ground_effects.push_back({{"effect_id", id}, {"layers", layers}});
      }
      result["ground_effects"] = std::move(ground_effects);
      result["ground_effect_layers_by_texture"] = batch.ground_effect_layers_by_texture;
      result["visible_alpha_threshold"] = 8;
      result["strong_alpha_threshold"] = 64;

      auto liquids = nlohmann::json::array();
      for (auto const& [id, cells] : batch.liquid_cells_by_type)
      {
        liquids.push_back({
          {"liquid_type_id", id},
          {"liquid_name", LiquidTypeDB::getLiquidName(id)},
          {"basic_type", LiquidTypeDB::getLiquidType(id)},
          {"cells", cells},
          {"min_surface_height", batch.min_liquid_height_by_type[id]},
          {"max_surface_height", batch.max_liquid_height_by_type[id]},
          {"min_depth", batch.min_liquid_depth_by_type[id]},
          {"max_depth", batch.max_liquid_depth_by_type[id]}
        });
      }
      result["liquids_by_type"] = std::move(liquids);
      result["liquid_chunks"] = batch.liquid_chunks_inspected;
      result["liquid_cells"] = batch.liquid_cells_inspected;
      result["liquid_cells_fully_under_terrain"] = batch.liquid_cells_under_terrain;
      result["liquid_format"] = "MH2O";
    }

    _cancel_requested = false;
    if (_direct_blueprint_running)
      finishDirectBlueprintCall(batch.call, result);
    else
      continueAfterTool(batch.call, result);
  }

  void AssistantDock::setBusy(bool busy)
  {
    _busy = busy;
    _send_button->setText(busy ? tr("Annuler") : tr("Envoyer"));
    _reset_button->setEnabled(!busy);
    _approve_button->setEnabled(!busy && !_pending_plan.empty() && !_plan_approved);
    _blueprint_lab_button->setEnabled(!busy);
    _api_key->setEnabled(!busy);
    _prompt->setReadOnly(busy);
  }

  void AssistantDock::finishTurn(QString const& answer)
  {
    appendTranscript(tr("Assistant"), answer);
    _status->setText(!_pending_plan.empty() && !_plan_approved
      ? tr("Plan en attente — clique sur « Approuver et exécuter ».")
      : tr("Prêt — Ctrl+Entrée pour envoyer."));
    setBusy(false);
    _prompt->setFocus();
  }

  void AssistantDock::failTurn(QString const& message)
  {
    appendTranscript(tr("Erreur"), message);
    _status->setText(message);
    setBusy(false);
  }

  void AssistantDock::appendTranscript(QString const& speaker, QString const& text)
  {
    _transcript->append(QString{"<b>%1</b><br>%2"}.arg(escapedHtml(speaker), escapedHtml(text)));
  }

  nlohmann::json AssistantDock::executeTool(FunctionCall const& call)
  {
    if (!_map_view || !_map_view->getWorld())
    {
      return toolError("Aucune carte n'est ouverte.");
    }

    nlohmann::json arguments;
    try
    {
      arguments = nlohmann::json::parse(call.arguments);
    }
    catch (std::exception const&)
    {
      return toolError("Les arguments de l'outil ne sont pas un objet JSON valide.");
    }

    if (!arguments.is_object())
    {
      return toolError("Les arguments de l'outil doivent être un objet JSON.");
    }

    auto* world = _map_view->getWorld();
    if (call.name == "submit_map_plan")
    {
      static auto const fields = std::set<std::string>{"title", "summary", "steps"};
      if (arguments.size() != fields.size())
      {
        return toolError("submit_map_plan exige exactement title, summary et steps.");
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.count(name))
        {
          return toolError("Argument non autorisé : " + name);
        }
      }

      auto const title = arguments.find("title");
      auto const summary = arguments.find("summary");
      auto const steps = arguments.find("steps");
      if (title == arguments.end() || !title->is_string()
          || summary == arguments.end() || !summary->is_string()
          || steps == arguments.end() || !steps->is_array())
      {
        return toolError("title et summary doivent être des chaînes, steps un tableau.");
      }
      auto const title_text = title->get<std::string>();
      auto const summary_text = summary->get<std::string>();
      if (title_text.empty() || title_text.size() > 120
          || summary_text.empty() || summary_text.size() > 1500
          || steps->empty() || steps->size() > 12)
      {
        return toolError("Le plan dépasse les limites autorisées.");
      }

      QStringList display_steps;
      std::size_t step_number = 1;
      for (auto const& step : *steps)
      {
        if (!step.is_string())
        {
          return toolError("Chaque étape doit être une chaîne.");
        }
        auto const text = step.get<std::string>();
        if (text.empty() || text.size() > 300)
        {
          return toolError("Une étape est vide ou dépasse 300 caractères.");
        }
        display_steps.push_back(QString{"%1. %2"}.arg(step_number++).arg(
          QString::fromStdString(text)));
      }

      _pending_plan = arguments;
      _plan_approved = false;
      _plan_checkpoint_saved = false;
      _approve_button->setVisible(true);
      appendTranscript(
        tr("Plan proposé — %1").arg(QString::fromStdString(title_text)),
        QString::fromStdString(summary_text) + "\n\n" + display_steps.join('\n')
          + tr("\n\nLes opérations sur toute la carte enregistrent chaque tuile et ne sont pas annulables avec Ctrl+Z."));
      _status->setText(tr("Plan prêt — vérifie-le puis clique sur « Approuver et exécuter »."));
      return {
        {"ok", true},
        {"operation", "submit_map_plan"},
        {"status", "awaiting_user_approval"},
        {"steps", steps->size()},
        {"global_operations_are_saved", true},
        {"global_operations_are_undoable", false}
      };
    }

    if (call.name == "get_editor_context")
    {
      if (!arguments.empty())
      {
        return toolError("get_editor_context n'accepte aucun argument.");
      }

      auto const cursor = _map_view->cursorPosition();
      auto const cursor_valid = _map_view->hasValidCursorPosition()
        && std::isfinite(cursor.x) && std::isfinite(cursor.y) && std::isfinite(cursor.z);
      auto const cursor_tile = cursor_valid ? TileIndex(cursor) : TileIndex(64, 64);
      auto const* camera = _map_view->getCamera();
      auto const& selected_texture = Noggit::Ui::selected_texture::texture;

      return {
        {"ok", true},
        {"map", {
          {"id", world->getMapID()},
          {"name", world->basename},
          {"loaded_tiles", world->mapIndex.getNLoadedTiles()},
          {"existing_tiles", world->mapIndex.getNumExistingTiles()}
        }},
        {"camera", {
          {"x", camera->position.x},
          {"y", camera->position.y},
          {"z", camera->position.z},
          {"yaw_degrees", camera->yaw()._},
          {"pitch_degrees", camera->pitch()._}
        }},
        {"cursor", {
          {"valid", cursor_valid && cursor_tile.is_valid()},
          {"x", cursor_valid ? cursor.x : 0.0f},
          {"y", cursor_valid ? cursor.y : 0.0f},
          {"z", cursor_valid ? cursor.z : 0.0f},
          {"tile_x", cursor_tile.is_valid() ? static_cast<int>(cursor_tile.x) : -1},
          {"tile_z", cursor_tile.is_valid() ? static_cast<int>(cursor_tile.z) : -1},
          {"tile_exists", cursor_tile.is_valid() && world->mapIndex.hasTile(cursor_tile)},
          {"tile_loaded", cursor_tile.is_valid() && world->mapIndex.tileLoaded(cursor_tile)}
        }},
        {"selected_texture", selected_texture && selected_texture->get()
          ? nlohmann::json(selected_texture->get()->file_key().filepath())
          : nlohmann::json(nullptr)},
        {"selection_count", world->current_selection().size()},
        {"editor_busy", NOGGIT_ACTION_MGR->getCurrentAction() != nullptr},
        {"coordinate_system", "WoW world units; terrain uses X and Z"}
      };
    }

    if (call.name == "create_moba_arena_blueprint")
    {
      try
      {
        std::vector<std::pair<std::size_t, std::size_t>> tiles;
        auto* world = _map_view->getWorld();
        for (std::size_t z = 0; z < 64; ++z)
          for (std::size_t x = 0; x < 64; ++x)
            if (world->mapIndex.hasTile(TileIndex(x, z))) tiles.emplace_back(x, z);
        if (auto const error = validateMobaArenaFootprint(tiles))
          return toolError(*error);
        auto blueprint = compileMobaArenaBlueprint(
          arguments,
          static_cast<std::size_t>(std::lround(std::sqrt(tiles.size()))));
        auto* application = Noggit::Application::NoggitApplication::instance();
        if (!application->hasClientData())
          return toolError("Aucune donnée client n'est chargée.");
        if (auto const error = validateBlueprintScatterAssets(
              blueprint, application->clientData()))
          return toolError(*error);
        return blueprint;
      }
      catch (std::exception const& exception)
      {
        return toolError(exception.what());
      }
    }

    if (call.name == "inspect_map")
    {
      if (!arguments.empty())
      {
        return toolError("inspect_map n'accepte aucun argument.");
      }

      nlohmann::json rows = nlohmann::json::array();
      std::size_t existing_tiles = 0;
      std::size_t min_x = 64;
      std::size_t max_x = 0;
      std::size_t min_z = 64;
      std::size_t max_z = 0;
      for (std::size_t z = 0; z < 64; ++z)
      {
        nlohmann::json ranges = nlohmann::json::array();
        std::size_t x = 0;
        while (x < 64)
        {
          while (x < 64 && !world->mapIndex.hasTile(TileIndex(x, z)))
          {
            ++x;
          }
          if (x == 64)
          {
            break;
          }
          auto const first = x;
          while (x + 1 < 64 && world->mapIndex.hasTile(TileIndex(x + 1, z)))
          {
            ++x;
          }
          auto const last = x;
          ranges.push_back({first, last});
          existing_tiles += last - first + 1;
          min_x = std::min(min_x, first);
          max_x = std::max(max_x, last);
          min_z = std::min(min_z, z);
          max_z = std::max(max_z, z);
          ++x;
        }
        if (!ranges.empty())
        {
          rows.push_back({{"z", z}, {"x_ranges", std::move(ranges)}});
        }
      }

      auto min_height = std::numeric_limits<float>::max();
      auto max_height = std::numeric_limits<float>::lowest();
      std::size_t sampled_vertices = 0;
      for (MapTile* tile : world->mapIndex.loaded_tiles())
      {
        for (unsigned z = 0; z < 16; ++z)
        {
          for (unsigned x = 0; x < 16; ++x)
          {
            auto* chunk = tile->getChunk(x, z);
            for (auto const& vertex : chunk->mVertices)
            {
              min_height = std::min(min_height, vertex.y);
              max_height = std::max(max_height, vertex.y);
              ++sampled_vertices;
            }
          }
        }
      }

      static auto const forbidden_wmo_types = std::set<int>{
        LIQUID_WMO_Water, LIQUID_WMO_Ocean, LIQUID_WMO_Water_Interior,
        LIQUID_WMO_Magma, LIQUID_WMO_Slime
      };
      auto available_liquid_types = nlohmann::json::array();
      for (auto record = gLiquidTypeDB.begin(); record != gLiquidTypeDB.end(); ++record)
      {
        auto const id = record->getInt(LiquidTypeDB::ID);
        auto const basic_type = record->getInt(LiquidTypeDB::Type);
        if (id < 1 || id > 65535 || forbidden_wmo_types.contains(id)
            || (basic_type != liquid_basic_types_water
                && basic_type != liquid_basic_types_ocean))
        {
          continue;
        }
        auto const basic_name = basic_type == liquid_basic_types_ocean ? "ocean"
          : "water";
        available_liquid_types.push_back({
          {"id", id},
          {"name", record->getString(LiquidTypeDB::Name)},
          {"basic_type", basic_name}
        });
      }
      std::sort(available_liquid_types.begin(), available_liquid_types.end(),
        [](nlohmann::json const& left, nlohmann::json const& right)
        {
          return left.at("id").get<int>() < right.at("id").get<int>();
        });

      return {
        {"ok", true},
        {"operation", "inspect_map"},
        {"map", {
          {"id", world->getMapID()},
          {"name", world->basename},
          {"existing_tiles", existing_tiles},
          {"loaded_tiles", world->mapIndex.getNLoadedTiles()},
          {"tile_bounds", existing_tiles > 0
            ? nlohmann::json{{"min_x", min_x}, {"max_x", max_x},
                             {"min_z", min_z}, {"max_z", max_z}}
            : nlohmann::json(nullptr)},
          {"tile_rows", std::move(rows)}
        }},
        {"terrain_sample", sampled_vertices > 0
          ? nlohmann::json{{"scope", "loaded_tiles"}, {"vertices", sampled_vertices},
                           {"min_height", min_height}, {"max_height", max_height}}
          : nlohmann::json(nullptr)},
        {"available_liquid_types", std::move(available_liquid_types)},
        {"global_grid", {{"width", 64}, {"height", 64}}}
      };
    }

    if (call.name == "search_textures")
    {
      static auto const expected_fields = std::set<std::string>{
        "query", "limit", "offset"};
      if (arguments.size() != expected_fields.size())
      {
        return toolError("search_textures exige exactement query, limit et offset.");
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!expected_fields.count(name))
        {
          return toolError("Argument non autorisé : " + name);
        }
      }

      auto const query_field = arguments.find("query");
      if (query_field == arguments.end() || !query_field->is_string())
      {
        return toolError("query doit être une chaîne.");
      }
      auto query = query_field->get<std::string>();
      if (query.size() > 128
          || std::any_of(query.begin(), query.end(), [](unsigned char c)
          {
            return c < 32 || c > 126;
          }))
      {
        return toolError("query doit contenir au plus 128 caractères ASCII imprimables.");
      }

      double limit_value = 0.0;
      double offset_value = 0.0;
      if (!readFiniteNumber(arguments, "limit", limit_value)
          || std::floor(limit_value) != limit_value
          || limit_value < 1.0 || limit_value > 100.0)
      {
        return toolError("limit doit être un entier compris entre 1 et 100.");
      }
      if (!readFiniteNumber(arguments, "offset", offset_value)
          || std::floor(offset_value) != offset_value
          || offset_value < 0.0 || offset_value > 1000000.0)
      {
        return toolError("offset doit être un entier compris entre 0 et 1000000.");
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return toolError("Aucune donnée client n'est chargée.");
      }

      query = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(query));
      std::vector<std::string> matches;
      for (auto const& [path, file_data_id]
           : application->clientData()->listfile()->pathToFileDataIDMap())
      {
        static_cast<void>(file_data_id);
        if (path.starts_with("tileset/") && path.ends_with(".blp")
            && (query.empty() || path.find(query) != std::string::npos))
        {
          matches.push_back(path);
        }
      }
      std::sort(matches.begin(), matches.end());

      auto const total_matches = matches.size();
      auto const limit = static_cast<std::size_t>(limit_value);
      auto const offset = std::min(
        static_cast<std::size_t>(offset_value), total_matches);
      auto const end = std::min(offset + limit, total_matches);
      std::vector<std::string> page(
        matches.begin() + static_cast<std::ptrdiff_t>(offset),
        matches.begin() + static_cast<std::ptrdiff_t>(end));

      return {
        {"ok", true},
        {"operation", "search_textures"},
        {"query", query},
        {"textures", page},
        {"offset", offset},
        {"returned", page.size()},
        {"total_matches", total_matches},
        {"next_offset", end < total_matches
          ? nlohmann::json(end) : nlohmann::json(nullptr)},
        {"truncated", end < total_matches},
        {"source", "client_listfile"}
      };
    }

    if (call.name == "preview_textures")
    {
      if (arguments.size() != 1 || !arguments.contains("texture_paths")
          || !arguments.at("texture_paths").is_array())
      {
        return toolError("preview_textures exige exactement texture_paths.");
      }
      auto const& requested_paths = arguments.at("texture_paths");
      if (requested_paths.empty() || requested_paths.size() > 12)
      {
        return toolError("texture_paths doit contenir entre une et 12 textures.");
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return toolError("Aucune donnée client n'est chargée.");
      }

      std::vector<std::string> paths;
      std::set<std::string> unique_paths;
      for (auto const& value : requested_paths)
      {
        if (!value.is_string())
        {
          return toolError("Chaque texture doit être un chemin texte.");
        }
        auto path = BlizzardArchive::ClientData::normalizeFilenameInternal(
          value.get<std::string>());
        if (path.size() > 260 || !path.starts_with("tileset/")
            || !path.ends_with(".blp"))
        {
          return toolError("Chaque chemin doit désigner une texture tileset/*.blp.");
        }
        if (!unique_paths.insert(path).second)
        {
          return toolError("Les textures à prévisualiser doivent être uniques.");
        }
        if (!application->clientData()->exists(path))
        {
          return toolError("Texture introuvable : " + path);
        }
        paths.push_back(std::move(path));
      }

      constexpr auto columns = 3;
      constexpr auto cell_width = 300;
      constexpr auto cell_height = 176;
      constexpr auto preview_size = 128;
      auto const rows = static_cast<int>((paths.size() + columns - 1) / columns);
      QImage sheet(columns * cell_width, rows * cell_height, QImage::Format_RGB32);
      sheet.fill(QColor(28, 31, 35));
      QPainter painter(&sheet);
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
      auto font = painter.font();
      font.setBold(true);
      font.setPixelSize(20);
      painter.setFont(font);

      auto mapping = nlohmann::json::array();
      for (std::size_t index = 0; index < paths.size(); ++index)
      {
        auto const column = static_cast<int>(index % columns);
        auto const row = static_cast<int>(index / columns);
        auto const left = column * cell_width;
        auto const top = row * cell_height;
        auto* pixmap = Noggit::BLPRenderer::getInstance().render_blp_to_pixmap(
          paths[index], preview_size, preview_size);
        if (!pixmap || pixmap->isNull())
        {
          return toolError("Impossible de rendre la texture : " + paths[index]);
        }
        auto const preview = pixmap->toImage().scaled(
          preview_size, preview_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter.drawImage(left + 8, top + 38, preview);
        auto const tile = preview.scaled(64, 64, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
        for (auto y = 0; y < 2; ++y)
        {
          for (auto x = 0; x < 2; ++x)
          {
            painter.drawImage(left + 156 + x * 64, top + 38 + y * 64, tile);
          }
        }
        painter.setPen(Qt::white);
        painter.drawText(left + 8, top + 27,
                         QString::number(static_cast<int>(index + 1)));
        mapping.push_back({
          {"index", index + 1},
          {"path", paths[index]}
        });
      }
      painter.end();

      QByteArray png;
      QBuffer buffer(&png);
      if (!buffer.open(QIODevice::WriteOnly) || !sheet.save(&buffer, "PNG"))
      {
        return toolError("Impossible d'encoder l'aperçu des textures.");
      }
      auto const data_url = QByteArray("data:image/png;base64,") + png.toBase64();
      return {
        {"ok", true},
        {"operation", "preview_textures"},
        {"textures", std::move(mapping)},
        {"count", paths.size()},
        {"layout", "left=BLP preview; right=2x2 repetition"},
        {preview_image_key, data_url.toStdString()}
      };
    }

    if (call.name == "inspect_map_view")
    {
      if (!arguments.empty())
      {
        return toolError("inspect_map_view n'accepte aucun argument.");
      }
      auto capture = [&]
      {
        _map_view->invalidate();
        return _map_view->grabFramebuffer();
      };
      auto screenshot = capture();
      if (screenshot.isNull())
      {
        return toolError("Impossible de capturer la vue 3D actuelle.");
      }
      auto const is_black = [](QImage const& image)
      {
        auto const step_x = std::max(1, image.width() / 64);
        auto const step_y = std::max(1, image.height() / 64);
        for (auto y = 0; y < image.height(); y += step_y)
          for (auto x = 0; x < image.width(); x += step_x)
            if (qRed(image.pixel(x, y)) + qGreen(image.pixel(x, y))
                + qBlue(image.pixel(x, y)) > 12)
              return false;
        return true;
      };
      if (is_black(screenshot))
      {
        _map_view->repaint();
        screenshot = capture();
      }
      if (is_black(screenshot))
      {
        return toolError(
          "La vue OpenGL a produit une capture entièrement noire après un nouveau rendu.");
      }
      if (screenshot.width() > 1280 || screenshot.height() > 1280)
      {
        screenshot = screenshot.scaled(
          1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
      QByteArray jpeg;
      QBuffer buffer(&jpeg);
      if (!buffer.open(QIODevice::WriteOnly) || !screenshot.save(&buffer, "JPEG", 82))
      {
        return toolError("Impossible d'encoder la capture de la vue 3D.");
      }
      auto const data_url = QByteArray("data:image/jpeg;base64,") + jpeg.toBase64();
      return {
        {"ok", true},
        {"operation", "inspect_map_view"},
        {"width", screenshot.width()},
        {"height", screenshot.height()},
        {"instruction", "Inspection visuelle uniquement ; ne pas remodifier automatiquement la carte."},
        {preview_image_key, data_url.toStdString()}
      };
    }

    if (call.name == "search_ground_effects")
    {
      static auto const fields = std::set<std::string>{"query", "limit"};
      if (arguments.size() != fields.size() || !arguments.contains("query")
          || !arguments.contains("limit") || !arguments.at("query").is_string()
          || !arguments.at("limit").is_number_integer())
        return toolError("search_ground_effects exige exactement query et limit.");
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.contains(name)) return toolError("Argument non autorisé : " + name);
      }
      auto query = lowerAscii(arguments.at("query").get<std::string>());
      auto const limit = arguments.at("limit").get<int>();
      if (query.size() > 128 || limit < 1 || limit > 50
          || std::any_of(query.begin(), query.end(), [](unsigned char c)
             { return c < 32 || c > 126; }))
        return toolError("query doit être ASCII imprimable et limit compris entre 1 et 50.");

      auto matches = nlohmann::json::array();
      auto total_matches = std::size_t{0};
      for (auto it = gGroundEffectTextureDB.begin(); it != gGroundEffectTextureDB.end(); ++it)
      {
        auto doodads = nlohmann::json::array();
        std::string searchable;
        for (std::size_t slot = 0; slot < 4; ++slot)
        {
          auto const doodad_id = it->getUInt(GroundEffectTextureDB::Doodads + slot);
          auto const weight = it->getUInt(GroundEffectTextureDB::Weights + slot);
          if (!doodad_id || !weight || !gGroundEffectDoodadDB.CheckIfIdExists(doodad_id))
            continue;
          auto path = std::string{"world/nodxt/detail/"}
            + gGroundEffectDoodadDB.getByID(doodad_id).getString(
              GroundEffectDoodadDB::Filename);
          auto const dot = path.find_last_of('.');
          if (dot != std::string::npos) path.replace(dot, std::string::npos, ".m2");
          path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(path));
          searchable += path + ' ';
          doodads.push_back({{"path", path}, {"weight", weight}});
        }
        if (doodads.empty() || (!query.empty() && searchable.find(query) == std::string::npos))
          continue;
        ++total_matches;
        if (matches.size() < static_cast<std::size_t>(limit))
        {
          matches.push_back({
            {"effect_id", it->getUInt(GroundEffectTextureDB::ID)},
            {"amount", it->getUInt(GroundEffectTextureDB::Amount)},
            {"terrain_type", it->getUInt(GroundEffectTextureDB::TerrainType)},
            {"doodads", std::move(doodads)}
          });
        }
      }
      auto const returned = matches.size();
      return {
        {"ok", true}, {"operation", "search_ground_effects"}, {"query", query},
        {"effects", std::move(matches)}, {"returned", returned},
        {"total_matches", total_matches}, {"truncated", total_matches > returned},
        {"automatic_effect_id", hasLushGroundEffectAssets()
          ? lush_ground_effect_id : automaticGroundEffectId().value_or(0)}
      };
    }

    if (call.name == "search_assets")
    {
      static auto const fields = std::set<std::string>{"query", "kind", "limit"};
      if (arguments.size() != fields.size())
      {
        return toolError("search_assets exige exactement query, kind et limit.");
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!fields.count(name))
        {
          return toolError("Argument non autorisé : " + name);
        }
      }

      auto const query_field = arguments.find("query");
      auto const kind_field = arguments.find("kind");
      if (query_field == arguments.end() || !query_field->is_string()
          || kind_field == arguments.end() || !kind_field->is_string())
      {
        return toolError("query et kind doivent être des chaînes.");
      }
      auto query = query_field->get<std::string>();
      auto const kind = kind_field->get<std::string>();
      if (query.size() > 128
          || std::any_of(query.begin(), query.end(), [](unsigned char c)
          {
            return c < 32 || c > 126;
          })
          || (kind != "any" && kind != "m2" && kind != "wmo"))
      {
        return toolError("query doit être ASCII imprimable et kind doit valoir any, m2 ou wmo.");
      }

      double limit_value = 0.0;
      if (!readFiniteNumber(arguments, "limit", limit_value)
          || std::floor(limit_value) != limit_value
          || limit_value < 1.0 || limit_value > 100.0)
      {
        return toolError("limit doit être un entier compris entre 1 et 100.");
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData())
      {
        return toolError("Aucune donnée client n'est chargée.");
      }
      query = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(query));
      std::vector<std::string> matches;
      for (auto const& [path, file_data_id]
           : application->clientData()->listfile()->pathToFileDataIDMap())
      {
        static_cast<void>(file_data_id);
        auto const is_m2 = path.ends_with(".m2");
        auto const is_wmo = path.ends_with(".wmo");
        auto const is_wmo_group = is_wmo && path.size() >= 8
          && path[path.size() - 8] == '_'
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 7]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 6]))
          && std::isdigit(static_cast<unsigned char>(path[path.size() - 5]));
        if ((!is_m2 && !is_wmo) || is_wmo_group
            || (kind == "m2" && !is_m2) || (kind == "wmo" && !is_wmo)
            || (!query.empty() && path.find(query) == std::string::npos))
        {
          continue;
        }
        matches.push_back(path);
      }
      std::sort(matches.begin(), matches.end());
      auto const total_matches = matches.size();
      auto const limit = static_cast<std::size_t>(limit_value);
      if (matches.size() > limit)
      {
        matches.resize(limit);
      }

      return {
        {"ok", true},
        {"operation", "search_assets"},
        {"query", query},
        {"kind", kind},
        {"assets", matches},
        {"returned", matches.size()},
        {"total_matches", total_matches},
        {"truncated", total_matches > matches.size()},
        {"source", "client_listfile"}
      };
    }

    if (call.name == "paint_texture")
    {
      static auto const expected_fields = std::set<std::string>{
        "x", "z", "texture_path", "radius", "hardness", "opacity"
      };
      if (arguments.size() != expected_fields.size())
      {
        return toolError("paint_texture exige exactement x, z, texture_path, radius, hardness et opacity.");
      }
      for (auto const& [name, value] : arguments.items())
      {
        static_cast<void>(value);
        if (!expected_fields.count(name))
        {
          return toolError("Argument non autorisé : " + name);
        }
      }

      double x = 0.0;
      double z = 0.0;
      double radius = 0.0;
      double hardness = 0.0;
      double opacity = 0.0;
      if (!readFiniteNumber(arguments, "x", x)
          || !readFiniteNumber(arguments, "z", z)
          || !readFiniteNumber(arguments, "radius", radius)
          || !readFiniteNumber(arguments, "hardness", hardness)
          || !readFiniteNumber(arguments, "opacity", opacity))
      {
        return toolError("Toutes les valeurs numériques doivent être présentes et finies.");
      }

      auto const texture_field = arguments.find("texture_path");
      if (texture_field == arguments.end() || !texture_field->is_string())
      {
        return toolError("texture_path doit être une chaîne.");
      }
      auto texture_path = texture_field->get<std::string>();
      if (texture_path.empty() || texture_path.size() > 260
          || std::any_of(texture_path.begin(), texture_path.end(), [](unsigned char c)
          {
            return c < 32 || c > 126;
          }))
      {
        return toolError("texture_path doit être un chemin WoW ASCII valide.");
      }
      texture_path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(texture_path));
      if (!texture_path.starts_with("tileset/") || !texture_path.ends_with(".blp")
          || texture_path.find("..") != std::string::npos)
      {
        return toolError("texture_path doit désigner un fichier tileset/*.blp sans traversée de chemin.");
      }

      auto const world_size = static_cast<double>(TILESIZE) * 64.0;
      if (x < 0.0 || x >= world_size || z < 0.0 || z >= world_size)
      {
        return toolError("Le centre du pinceau est hors de la grille 64 x 64 de la carte.");
      }
      if (radius < 5.0 || radius > 200.0)
      {
        return toolError("radius doit être compris entre 5 et 200.");
      }
      if (hardness < 0.0 || hardness > 1.0 || opacity < 0.01 || opacity > 1.0)
      {
        return toolError("hardness doit être entre 0 et 1 et opacity entre 0.01 et 1.");
      }

      auto const position = glm::vec3{static_cast<float>(x), 0.0f, static_cast<float>(z)};
      auto const center_tile = TileIndex(position);
      if (!center_tile.is_valid() || !world->mapIndex.hasTile(center_tile))
      {
        return toolError("La tuile centrale n'existe pas dans cette carte.");
      }
      if (!world->mapIndex.tileLoaded(center_tile))
      {
        return toolError("La tuile centrale n'est pas encore chargée. Déplace la caméra vers cette zone puis réessaie.");
      }
      for (std::size_t tile_z = 0; tile_z < 64; ++tile_z)
      {
        for (std::size_t tile_x = 0; tile_x < 64; ++tile_x)
        {
          auto const tile = TileIndex(tile_x, tile_z);
          if (world->mapIndex.hasTile(tile)
              && circleTouchesTile(x, z, radius, tile_x, tile_z)
              && !world->mapIndex.tileLoaded(tile))
          {
            return toolError("Le pinceau touche la tuile " + std::to_string(tile_x) + ","
                             + std::to_string(tile_z) + " qui n'est pas chargée.");
          }
        }
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData() || !application->clientData()->exists(texture_path))
      {
        return toolError("La texture n'existe pas dans les données client chargées : " + texture_path);
      }
      if (NOGGIT_ACTION_MGR->getCurrentAction())
      {
        return toolError("Une action utilisateur est en cours. Termine le geste de pinceau puis réessaie.");
      }
      if (!_map_view->context())
      {
        return toolError("Le contexte OpenGL de la vue n'est pas disponible.");
      }

      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      Brush brush;
      brush.init();
      brush.setHardness(static_cast<float>(hardness));
      brush.setRadius(static_cast<float>(radius));
      auto texture = scoped_blp_texture_reference(texture_path, _map_view->getRenderContext());

      NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      bool changed = false;
      try
      {
        changed = world->paintTexture(position, &brush, static_cast<float>(opacity * 255.0), 1.0f,
                                      std::move(texture));
      }
      catch (std::exception const& exception)
      {
        NOGGIT_ACTION_MGR->endAction();
        return partialToolError(std::string{"La peinture de texture a échoué : "} + exception.what());
      }
      catch (...)
      {
        NOGGIT_ACTION_MGR->endAction();
        return partialToolError("La peinture de texture a échoué.");
      }
      NOGGIT_ACTION_MGR->endAction();
      _map_view->invalidate();

      if (!changed)
      {
        return toolError("Aucun pixel n'a été modifié, probablement parce que les chunks ont déjà quatre couches de texture.");
      }
      return {
        {"ok", true},
        {"operation", "paint_texture"},
        {"texture_path", texture_path},
        {"center", {{"x", x}, {"z", z}}},
        {"radius", radius},
        {"hardness", hardness},
        {"opacity", opacity},
        {"undoable", true},
        {"saved", false}
      };
    }

    if (call.name == "set_base_texture_on_loaded_tiles")
    {
      if (arguments.size() != 1 || !arguments.contains("texture_path"))
      {
        return toolError("set_base_texture_on_loaded_tiles exige exactement texture_path.");
      }

      auto const texture_field = arguments.find("texture_path");
      if (!texture_field->is_string())
      {
        return toolError("texture_path doit être une chaîne.");
      }
      auto texture_path = texture_field->get<std::string>();
      if (texture_path.empty() || texture_path.size() > 260
          || std::any_of(texture_path.begin(), texture_path.end(), [](unsigned char c)
          {
            return c < 32 || c > 126;
          }))
      {
        return toolError("texture_path doit être un chemin WoW ASCII valide.");
      }
      texture_path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(texture_path));
      if (!texture_path.starts_with("tileset/") || !texture_path.ends_with(".blp")
          || texture_path.find("..") != std::string::npos)
      {
        return toolError("texture_path doit désigner un fichier tileset/*.blp sans traversée de chemin.");
      }

      auto* application = Noggit::Application::NoggitApplication::instance();
      if (!application->hasClientData() || !application->clientData()->exists(texture_path))
      {
        return toolError("La texture n'existe pas dans les données client chargées : " + texture_path);
      }
      if (world->mapIndex.getNLoadedTiles() == 0)
      {
        return toolError("Aucune tuile entièrement chargée.");
      }
      if (NOGGIT_ACTION_MGR->getCurrentAction())
      {
        return toolError("Une action utilisateur est en cours. Termine-la puis réessaie.");
      }
      if (!_map_view->context())
      {
        return toolError("Le contexte OpenGL de la vue n'est pas disponible.");
      }

      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      auto texture = scoped_blp_texture_reference(texture_path, _map_view->getRenderContext());
      std::size_t tiles_changed = 0;
      std::size_t chunks_changed = 0;
      NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      try
      {
        for (MapTile* tile : world->mapIndex.loaded_tiles())
        {
          ++tiles_changed;
          world->mapIndex.setChanged(tile);
          for (unsigned z = 0; z < 16; ++z)
          {
            for (unsigned x = 0; x < 16; ++x)
            {
              auto* chunk = tile->getChunk(x, z);
              NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
              chunk->eraseTextures();
              chunk->addTexture(texture);
              ++chunks_changed;
            }
          }
        }
      }
      catch (std::exception const& exception)
      {
        NOGGIT_ACTION_MGR->endAction();
        _map_view->invalidate();
        return partialToolError(std::string{"Le remplacement global de texture a échoué : "} + exception.what());
      }
      catch (...)
      {
        NOGGIT_ACTION_MGR->endAction();
        _map_view->invalidate();
        return partialToolError("Le remplacement global de texture a échoué.");
      }
      NOGGIT_ACTION_MGR->endAction();
      _map_view->invalidate();

      auto const existing_tiles = world->mapIndex.getNumExistingTiles();
      return {
        {"ok", true},
        {"operation", "set_base_texture_on_loaded_tiles"},
        {"texture_path", texture_path},
        {"tiles_changed", tiles_changed},
        {"chunks_changed", chunks_changed},
        {"existing_tiles", existing_tiles},
        {"covers_entire_map", tiles_changed == existing_tiles},
        {"undoable", true},
        {"saved", false}
      };
    }

    if (call.name != "change_terrain_height")
    {
      return toolError("Outil inconnu : " + call.name);
    }

    static auto const expected_fields = std::set<std::string>{
      "x", "z", "delta", "radius", "falloff", "inner_radius"
    };
    if (arguments.size() != expected_fields.size())
    {
      return toolError("change_terrain_height exige exactement x, z, delta, radius, falloff et inner_radius.");
    }
    for (auto const& [name, value] : arguments.items())
    {
      static_cast<void>(value);
      if (!expected_fields.count(name))
      {
        return toolError("Argument non autorisé : " + name);
      }
    }

    double x = 0.0;
    double z = 0.0;
    double delta = 0.0;
    double radius = 0.0;
    double inner_radius = 0.0;
    if (!readFiniteNumber(arguments, "x", x)
        || !readFiniteNumber(arguments, "z", z)
        || !readFiniteNumber(arguments, "delta", delta)
        || !readFiniteNumber(arguments, "radius", radius)
        || !readFiniteNumber(arguments, "inner_radius", inner_radius))
    {
      return toolError("Toutes les valeurs numériques doivent être présentes et finies.");
    }

    auto const world_size = static_cast<double>(TILESIZE) * 64.0;
    if (x < 0.0 || x >= world_size || z < 0.0 || z >= world_size)
    {
      return toolError("Le centre du pinceau est hors de la grille 64 x 64 de la carte.");
    }
    if (delta == 0.0 || std::abs(delta) > 50.0)
    {
      return toolError("delta doit être non nul et compris entre -50 et 50.");
    }
    if (radius < 5.0 || radius > 200.0)
    {
      return toolError("radius doit être compris entre 5 et 200.");
    }
    if (inner_radius < 0.0 || inner_radius > 1.0)
    {
      return toolError("inner_radius doit être compris entre 0 et 1.");
    }

    auto const falloff_field = arguments.find("falloff");
    if (falloff_field == arguments.end() || !falloff_field->is_string())
    {
      return toolError("falloff doit être une chaîne.");
    }
    static auto const falloffs = std::map<std::string, int>{
      {"flat", eTerrainType_Flat},
      {"linear", eTerrainType_Linear},
      {"smooth", eTerrainType_Smooth},
      {"gaussian", eTerrainType_Gaussian}
    };
    auto const falloff = falloffs.find(falloff_field->get<std::string>());
    if (falloff == falloffs.end())
    {
      return toolError("falloff doit valoir flat, linear, smooth ou gaussian.");
    }

    auto const position = glm::vec3{static_cast<float>(x), 0.0f, static_cast<float>(z)};
    auto const center_tile = TileIndex(position);
    if (!center_tile.is_valid() || !world->mapIndex.hasTile(center_tile))
    {
      return toolError("La tuile centrale n'existe pas dans cette carte.");
    }
    if (!world->mapIndex.tileLoaded(center_tile))
    {
      return toolError("La tuile centrale n'est pas encore chargée. Déplace la caméra vers cette zone puis réessaie.");
    }

    for (std::size_t tile_z = 0; tile_z < 64; ++tile_z)
    {
      for (std::size_t tile_x = 0; tile_x < 64; ++tile_x)
      {
        auto const tile = TileIndex(tile_x, tile_z);
        if (world->mapIndex.hasTile(tile)
            && circleTouchesTile(x, z, radius, tile_x, tile_z)
            && !world->mapIndex.tileLoaded(tile))
        {
          return toolError("Le pinceau touche la tuile " + std::to_string(tile_x) + ","
                           + std::to_string(tile_z) + " qui n'est pas chargée.");
        }
      }
    }

    if (NOGGIT_ACTION_MGR->getCurrentAction())
    {
      return toolError("Une action utilisateur est en cours. Termine le geste de pinceau puis réessaie.");
    }
    if (!_map_view->context())
    {
      return toolError("Le contexte OpenGL de la vue n'est pas disponible.");
    }

    _map_view->makeCurrent();
    OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
    NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
    try
    {
      world->changeTerrain(position, static_cast<float>(delta), static_cast<float>(radius),
                           falloff->second, static_cast<float>(inner_radius));
    }
    catch (std::exception const& exception)
    {
      NOGGIT_ACTION_MGR->endAction();
      return partialToolError(std::string{"La modification du terrain a échoué : "} + exception.what());
    }
    catch (...)
    {
      NOGGIT_ACTION_MGR->endAction();
      return partialToolError("La modification du terrain a échoué.");
    }
    NOGGIT_ACTION_MGR->endAction();
    _map_view->invalidate();

    return {
      {"ok", true},
      {"operation", "change_terrain_height"},
      {"center", {{"x", x}, {"z", z}}},
      {"delta", delta},
      {"radius", radius},
      {"falloff", falloff_field->get<std::string>()},
      {"inner_radius", inner_radius},
      {"tile", {{"x", center_tile.x}, {"z", center_tile.z}}},
      {"undoable", true},
      {"saved", false}
    };
  }
}
