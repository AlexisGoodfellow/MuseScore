/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "editudeservice.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QWebSocketProtocol>

#include "notation/internal/igetscore.h"
#include "log.h"

using namespace mu::editude::internal;

EditudeService::EditudeService(const muse::modularity::ContextPtr& iocCtx, QObject* parent)
    : QObject(parent)
    , Contextable(iocCtx)
{
}

void EditudeService::start()
{
    const QByteArray envUrl = qgetenv("EDITUDE_SESSION_URL");
    if (envUrl.isEmpty()) {
        LOGD() << "[editude] EDITUDE_SESSION_URL not set; collaboration disabled";
        return;
    }

    QNetworkRequest req(QUrl(QString::fromUtf8(envUrl)));
    QNetworkReply* reply = m_nam.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] session fetch failed:" << reply->errorString();
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_token = obj.value("token").toString();
        m_websocketUrl = obj.value("websocket_url").toString();
        m_projectId = obj.value("project_id").toString();

        if (m_token.isEmpty() || m_websocketUrl.isEmpty() || m_projectId.isEmpty()) {
            LOGW() << "[editude] session response missing required fields";
            return;
        }

        m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(m_socket, &QWebSocket::connected, this, &EditudeService::onConnected);
        connect(m_socket, &QWebSocket::textMessageReceived, this, &EditudeService::onServerMessage);

        m_state = State::Authenticating;
        m_socket->open(QUrl(m_websocketUrl));
    });
}

void EditudeService::onConnected()
{
    QJsonObject msg;
    msg["type"] = "auth";
    msg["token"] = m_token;
    m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void EditudeService::onServerMessage(const QString& text)
{
    const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = msg.value("type").toString();

    if (type == "auth_ok") {
        QJsonObject join;
        join["type"] = "join";
        join["project_id"] = m_projectId;
        join["since_revision"] = 0;
        m_socket->sendTextMessage(QJsonDocument(join).toJson(QJsonDocument::Compact));
        m_state = State::Joining;

    } else if (type == "sync") {
        m_state = State::Live;
        LOGD() << "[editude] joined project" << m_projectId
               << "at revision" << msg.value("server_revision").toInt();

    } else if (type == "op_ack") {
        LOGD() << "[editude] op_ack op_id=" << msg.value("op_id").toString()
               << "revision=" << msg.value("revision").toInt();

    } else if (type == "op") {
        if (!m_score) {
            LOGW() << "[editude] received remote op but score not ready";
            return;
        }
        m_applyingRemote = true;
        m_applicator.apply(m_score, msg.value("payload").toObject());
        m_applyingRemote = false;

    } else if (type == "auth_error" || type == "error") {
        LOGW() << "[editude] server error:" << msg.value("detail").toString();
    }
}

void EditudeService::onNotationChanged(mu::notation::INotationPtr notation)
{
    if (m_currentNotation) {
        m_currentNotation->undoStack()->changesChannel().disconnect(this);
    }

    m_score = nullptr;
    m_currentNotation = notation;

    if (notation) {
        auto* gs = dynamic_cast<mu::notation::IGetScore*>(notation.get());
        m_score = gs ? gs->score() : nullptr;

        notation->undoStack()->changesChannel().onReceive(
            this,
            [this](const mu::engraving::ScoreChanges& changes) {
                onScoreChanges(changes);
            });
    }
}

void EditudeService::onScoreChanges(const mu::engraving::ScoreChanges& changes)
{
    if (m_applyingRemote) {
        return;
    }
    if (m_state != State::Live || !m_socket) {
        return;
    }

    for (const auto& [obj, cmds] : changes.changedObjects) {
        std::optional<QJsonObject> payload = m_translator.translate(obj, cmds, m_projectId);
        if (!payload.has_value()) {
            continue;
        }

        QJsonObject msg;
        msg["type"] = "op";
        msg["client_seq"] = ++m_clientSeq;
        msg["payload"] = payload.value();
        m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    }
}
