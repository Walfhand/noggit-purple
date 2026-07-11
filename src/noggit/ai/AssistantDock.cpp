// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AssistantDock.hpp>
#include <noggit/ai/ProceduralLayout.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Brush.h>
#include <noggit/Camera.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>

#include <ClientData.hpp>
#include <FastNoise/FastNoise.h>

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Noggit::Ai
{
  namespace
  {
    constexpr auto max_tool_rounds = std::size_t{32};
    constexpr auto default_model = "gpt-5.6";
    constexpr auto system_instructions = R"(Tu es l'agent de création intégré à Noggit. Réponds en français.
Pour une petite retouche locale explicite, utilise directement les outils locaux.
Pour une création, une refonte ou une opération portant sur toute la carte :
1. inspecte d'abord la carte et recherche les textures/assets nécessaires ;
2. choisis toi-même quand l'utilisateur te délègue les choix esthétiques ;
3. appelle submit_map_plan et arrête toute mutation jusqu'au message [APPROBATION HOTE] ;
4. après approbation, exécute toutes les étapes disponibles sans demander de confirmation intermédiaire ;
5. appelle validate_map après les opérations globales avant d'annoncer leur réussite.
Après un relief global naturel, utilise blend_terrain_textures_on_map pour répartir les textures selon la hauteur et la pente. Réserve paint_texture aux retouches locales et ne remplace pas ensuite ce mélange par set_base_texture_on_map.
Pour créer des routes, rivières, voies, plateformes ou autres formes continues, utilise apply_terrain_layout_on_map afin d'appliquer ensemble leur hauteur et leurs textures sémantiques.
Regroupe toutes les formes globales dans un seul appel à cet outil ; n'enchaîne jamais des change_terrain_height pour construire un layout complet et ne rappelle pas le layout uniquement pour retoucher ses textures.
Les outils *_on_map enregistrent les tuiles une par une et ne sont pas annulables avec Ctrl+Z. Rapporte uniquement leurs compteurs réels. Ne prétends jamais qu'une action a réussi sans résultat ok=true. Si un outil échoue partiellement, indique précisément ce qui a été fait et ce qui reste. Une question n'est permise que si une information indispensable ne peut pas être choisie raisonnablement.)";

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
      BlendTerrainTextures,
      SetBaseTexture,
      Validate
    };

    std::optional<MapBatchOperation> mapBatchOperation(std::string_view name)
    {
      if (name == "generate_terrain_on_map") return MapBatchOperation::GenerateTerrain;
      if (name == "apply_terrain_layout_on_map") return MapBatchOperation::ApplyTerrainLayout;
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
    std::array<std::size_t, 4> visible_texture_pixels{};
    std::map<std::string, std::size_t> texture_chunks_by_path;
    std::map<std::string, std::size_t> visible_texture_pixels_by_path;
    std::map<std::string, std::size_t> strong_texture_pixels_by_path;
    std::map<std::string, std::uint8_t> max_texture_alpha_by_path;
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
    std::vector<scoped_blp_texture_reference> procedural_textures;
    std::vector<std::size_t> feature_core_pixels;
    std::vector<std::size_t> feature_transition_pixels;
    std::array<std::size_t, 4> strong_texture_pixels{};
    std::array<std::uint8_t, 4> max_texture_alpha{};
    std::vector<TileIndex> height_changed_tiles;
    std::vector<TileIndex> normal_tiles;
    std::vector<TileIndex> normal_neighborhood;
    std::vector<bool> normal_keep_loaded;
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

    auto request_body = nlohmann::json{
      {"model", model.toStdString()},
      {"instructions", system_instructions},
      {"input", _input},
      {"tools", toolDefinitions()},
      {"parallel_tool_calls", false},
      {"max_output_tokens", 4096},
      {"store", false},
      {"include", {"reasoning.encrypted_content"}}
    };

    QNetworkRequest request(QUrl("https://api.openai.com/v1/responses"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key);
    request.setRawHeader("User-Agent", "Noggit-Purple/AI");

    auto* reply = _network->post(request, QByteArray::fromStdString(request_body.dump()));
    _active_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleReply(reply); });
    _status->setText(tr("OpenAI réfléchit…"));
  }

  void AssistantDock::handleReply(QNetworkReply* reply)
  {
    auto const body = reply->readAll();
    auto const status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto const network_error = reply->error();
    auto const network_error_text = reply->errorString();
    auto const request_id = QString::fromUtf8(reply->rawHeader("x-request-id"));
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

    nlohmann::json response;
    try
    {
      response = nlohmann::json::parse(body.constData(), body.constData() + body.size());
    }
    catch (std::exception const&)
    {
      if (network_error != QNetworkReply::NoError)
      {
        failTurn(tr("Erreur réseau : %1").arg(network_error_text));
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
      failTurn(reason.empty()
        ? tr("OpenAI n'a pas pu terminer la réponse.")
        : tr("OpenAI n'a pas pu terminer la réponse : %1").arg(QString::fromStdString(reason)));
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
    _input.push_back({
      {"type", "function_call_output"},
      {"call_id", call.call_id},
      {"output", result.dump()}
    });
    ++_tool_rounds;
    _status->setText(tr("Noggit a terminé %1, poursuite du plan…")
                       .arg(QString::fromStdString(call.name)));
    sendRequest();
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
      continueAfterTool(call, result);
      return true;
    };

    if (!_map_view || !_map_view->getWorld())
    {
      return completeWith(toolError("Aucune carte n'est ouverte."));
    }
    auto const mutates_map = !isValidation(*operation);
    if (mutates_map && !_plan_approved)
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
        auto const radius = static_cast<double>(
          feature.half_width_ratio) * short_side;
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
    try
    {
      _map_view->makeCurrent();
      OpenGL::context::scoped_setter const current_context(::gl, _map_view->context());
      if (!isValidation(_map_batch->operation))
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
                ++_map_batch->texture_chunks_by_path[texture_set->filename(layer)];
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
              auto const sample = sampleProceduralLayout(
                layout, u, v, vertex.y, 0.0f, map_width, map_height);
              if (!std::isfinite(sample.height))
              {
                throw std::runtime_error("Le layout a produit une hauteur non finie.");
              }
              if (sample.height != vertex.y)
              {
                vertex.y = sample.height;
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
            auto& texture_set = ensureTextureSet(
              *chunk, _map_view->getRenderContext());
            texture_set.eraseTextures();
            for (std::size_t layer = 0; layer < textures.size(); ++layer)
            {
              if (texture_set.addTexture(textures[layer]) != static_cast<int>(layer))
              {
                throw std::runtime_error("Impossible d'ajouter les couches du layout.");
              }
            }
            ++_map_batch->chunks_with_multiple_texture_layers;
            _map_batch->max_texture_layers = std::max(
              _map_batch->max_texture_layers, layout.texture_paths.size());

            texture_set.create_temporary_alphamaps_if_needed();
            auto& alpha = *texture_set.getTempAlphamaps();
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
                  terrain.height, terrain.slope_degrees, map_width, map_height);
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

                auto visible_layers = 0;
                for (std::size_t layer = 0; layer < 4; ++layer)
                {
                  auto const value = sample.quantized_weights[layer];
                  alpha[layer][pixel] = value;
                  if (layer >= layout.texture_paths.size())
                  {
                    continue;
                  }
                  _map_batch->max_texture_alpha[layer] = std::max(
                    _map_batch->max_texture_alpha[layer], value);
                  if (value >= visible_alpha)
                  {
                    ++visible_layers;
                    ++_map_batch->visible_texture_pixels[layer];
                  }
                  if (value >= strong_alpha)
                  {
                    ++_map_batch->strong_texture_pixels[layer];
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
            if (!texture_set.apply_alpha_changes())
            {
              throw std::runtime_error("Impossible d'appliquer les alphamaps du layout.");
            }
          }
        }
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

      if (!isValidation(_map_batch->operation))
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
      ++_map_batch->tiles_changed;
      _map_batch->chunks_changed += 256;
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
      auto* world = _map_view->getWorld();
      if (!isValidation(batch.operation))
      {
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
        auto const normals_are_current = !changesHeight(batch.operation)
          || batch.normals_recalculated == batch.normal_tiles.size();
        result = {
          {"ok", batch.failures == 0
            && normals_are_current && blend_is_visible && layout_is_valid},
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
          {"texture_layer", definition.texture_layer},
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
      result["textures"] = std::move(texture_stats);
      result["layers_requested"] = batch.procedural_layout->texture_paths.size();
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
      result["visible_alpha_threshold"] = 8;
      result["strong_alpha_threshold"] = 64;
    }

    _cancel_requested = false;
    continueAfterTool(batch.call, result);
  }

  void AssistantDock::setBusy(bool busy)
  {
    _busy = busy;
    _send_button->setText(busy ? tr("Annuler") : tr("Envoyer"));
    _reset_button->setEnabled(!busy);
    _approve_button->setEnabled(!busy && !_pending_plan.empty() && !_plan_approved);
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
        {"global_grid", {{"width", 64}, {"height", 64}}}
      };
    }

    if (call.name == "search_textures")
    {
      static auto const expected_fields = std::set<std::string>{"query", "limit"};
      if (arguments.size() != expected_fields.size())
      {
        return toolError("search_textures exige exactement query et limit.");
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
        if (path.starts_with("tileset/") && path.ends_with(".blp")
            && (query.empty() || path.find(query) != std::string::npos))
        {
          matches.push_back(path);
        }
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
        {"operation", "search_textures"},
        {"query", query},
        {"textures", matches},
        {"returned", matches.size()},
        {"total_matches", total_matches},
        {"truncated", total_matches > matches.size()},
        {"source", "client_listfile"}
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
