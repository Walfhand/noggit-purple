// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ai/AiProtocol.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QPointer>
#include <QtWidgets/QDockWidget>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class MapView;

namespace Noggit::Ai
{
  struct MapBatchState;
  struct MobaTransactionState;

  class AssistantDock final : public QDockWidget
  {
  public:
    explicit AssistantDock(MapView* map_view, QWidget* parent = nullptr);
    ~AssistantDock() override;
    void cancelPending();

  private:
    void submitPrompt();
    void openMobaBlueprintLab();
    void startNextDirectBlueprintCall();
    void finishDirectBlueprintCall(FunctionCall const& call, nlohmann::json const& result);
    void approvePlan();
    void resetConversation();
    void sendRequest();
    void sendRequest(nlohmann::json const& extra_input);
    void postRequest(QByteArray const& body, int attempt);
    void handleReply(QNetworkReply* reply);
    void continueAfterTool(FunctionCall const& call, nlohmann::json const& result);
    bool startMapBatch(FunctionCall const& call);
    void processMapBatch();
    void finishMapBatch(nlohmann::json result);
    void setBusy(bool busy);
    void finishTurn(QString const& answer);
    void failTurn(QString const& message);
    std::optional<std::string> beginMobaTransaction();
    std::optional<std::string> rollbackMobaTransaction();
    void commitMobaTransaction();
    void appendTranscript(QString const& speaker, QString const& text);
    nlohmann::json executeTool(FunctionCall const& call);

    QPointer<MapView> _map_view;
    QPointer<QNetworkReply> _active_reply;
    QNetworkAccessManager* _network;
    QTextBrowser* _transcript;
    QLineEdit* _api_key;
    QPlainTextEdit* _prompt;
    QPushButton* _send_button;
    QPushButton* _reset_button;
    QPushButton* _approve_button;
    QPushButton* _blueprint_lab_button;
    QLabel* _status;
    nlohmann::json _input;
    nlohmann::json _pending_plan;
    nlohmann::json _approved_blueprint_calls = nlohmann::json::array();
    nlohmann::json _direct_blueprint_calls = nlohmann::json::array();
    nlohmann::json _direct_blueprint_results = nlohmann::json::array();
    std::unique_ptr<MapBatchState> _map_batch;
    std::unique_ptr<MobaTransactionState> _moba_transaction;
    std::size_t _tool_rounds = 0;
    bool _busy = false;
    bool _cancel_requested = false;
    bool _plan_approved = false;
    bool _plan_checkpoint_saved = false;
    bool _direct_blueprint_running = false;
  };
}
