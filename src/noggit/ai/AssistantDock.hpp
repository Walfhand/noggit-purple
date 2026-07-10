// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ai/AiProtocol.hpp>

#include <QtCore/QPointer>
#include <QtWidgets/QDockWidget>

#include <cstddef>

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
  class AssistantDock final : public QDockWidget
  {
  public:
    explicit AssistantDock(MapView* map_view, QWidget* parent = nullptr);
    void cancelPending();

  private:
    void submitPrompt();
    void resetConversation();
    void sendRequest();
    void handleReply(QNetworkReply* reply);
    void setBusy(bool busy);
    void finishTurn(QString const& answer);
    void failTurn(QString const& message);
    void appendTranscript(QString const& speaker, QString const& text);
    nlohmann::json executeTool(FunctionCall const& call) const;

    QPointer<MapView> _map_view;
    QPointer<QNetworkReply> _active_reply;
    QNetworkAccessManager* _network;
    QTextBrowser* _transcript;
    QLineEdit* _api_key;
    QPlainTextEdit* _prompt;
    QPushButton* _send_button;
    QPushButton* _reset_button;
    QLabel* _status;
    nlohmann::json _input;
    std::size_t _tool_rounds = 0;
    bool _busy = false;
    bool _cancel_requested = false;
  };
}
