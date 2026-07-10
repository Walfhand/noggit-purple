// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ai/AssistantDock.hpp>

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
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>

#include <ClientData.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QUrl>
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

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Ai
{
  namespace
  {
    constexpr auto max_tool_rounds = std::size_t{8};
    constexpr auto default_model = "gpt-5.4-mini";

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
  }

  AssistantDock::AssistantDock(MapView* map_view, QWidget* parent)
    : QDockWidget(tr("AI Assistant"), parent)
    , _map_view(map_view)
    , _network(new QNetworkAccessManager(this))
    , _transcript(new QTextBrowser(this))
    , _api_key(new QLineEdit(this))
    , _prompt(new PromptEdit(this))
    , _send_button(new QPushButton(tr("Envoyer"), this))
    , _reset_button(new QPushButton(tr("Nouvelle conversation"), this))
    , _status(new QLabel(this))
    , _input(nlohmann::json::array())
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
    _prompt->setPlaceholderText(tr("Exemple : relève le terrain sous le curseur de 2 unités avec un rayon de 20."));
    _prompt->setMaximumHeight(110);
    _status->setWordWrap(true);

    buttons->addWidget(_reset_button);
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

    appendTranscript(tr("Noggit"), tr("Décris ce que tu veux faire. Cette première version peut lire le contexte de l'éditeur et relever ou abaisser le terrain ; chaque modification reste annulable."));
    if (qEnvironmentVariableIsEmpty("OPENAI_API_KEY"))
    {
      _status->setText(tr("Saisis une clé API OpenAI ci-dessus."));
    }
    else
    {
      _status->setText(tr("Prêt — Ctrl+Entrée pour envoyer."));
    }
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
    if (_active_reply)
    {
      _cancel_requested = true;
      _active_reply->abort();
    }
  }

  void AssistantDock::resetConversation()
  {
    if (_busy)
    {
      return;
    }

    _input = nlohmann::json::array();
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
      {"instructions", "Tu es l'assistant intégré à l'éditeur de cartes Noggit. Réponds en français et reste concis. Utilise uniquement les outils fournis pour agir sur la carte. Ne modifie la carte que sur demande explicite de l'utilisateur ; une question, une explication ou une demande de plan reste en lecture seule. Si une position est implicite (par exemple 'ici' ou 'sous le curseur'), appelle d'abord get_editor_context. Pour toute la carte, toutes les tuiles chargées ou tous les chunks, utilise set_base_texture_on_loaded_tiles et rapporte ses nombres exacts. Si l'utilisateur te laisse choisir une texture belle ou adaptée, cherche, choisis toi-même un résultat pertinent et exécute sans redemander. Ne prétends jamais qu'une modification a réussi si l'outil ne l'a pas confirmé. Si un outil signale une modification partielle possible, préviens l'utilisateur qu'il peut l'annuler avec Ctrl+Z. Demande une précision seulement si elle est indispensable pour agir."},
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
        failTurn(tr("La limite de huit appels d'outils a été atteinte."));
        return;
      }

      nlohmann::json result;
      try
      {
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

      _input.push_back({
        {"type", "function_call_output"},
        {"call_id", calls.front().call_id},
        {"output", result.dump()}
      });
      ++_tool_rounds;
      _status->setText(tr("Noggit exécute %1…").arg(QString::fromStdString(calls.front().name)));
      sendRequest();
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

  void AssistantDock::setBusy(bool busy)
  {
    _busy = busy;
    _send_button->setText(busy ? tr("Annuler") : tr("Envoyer"));
    _reset_button->setEnabled(!busy);
    _api_key->setEnabled(!busy);
    _prompt->setReadOnly(busy);
  }

  void AssistantDock::finishTurn(QString const& answer)
  {
    appendTranscript(tr("Assistant"), answer);
    _status->setText(tr("Prêt — Ctrl+Entrée pour envoyer."));
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

  nlohmann::json AssistantDock::executeTool(FunctionCall const& call) const
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
        changed = world->paintTexture(position, &brush, static_cast<float>(opacity), 1.0f,
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
