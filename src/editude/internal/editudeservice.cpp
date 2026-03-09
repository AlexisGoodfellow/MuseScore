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

#include <algorithm>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QWebSocketProtocol>
#include "project/types/projecttypes.h"

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
    m_snapshotPath = QString::fromUtf8(qgetenv("EDITUDE_SNAPSHOT_PATH"));
    m_sessionUrl = QString::fromUtf8(qgetenv("EDITUDE_SESSION_URL"));

    if (m_sessionUrl.isEmpty()) {
        LOGD() << "[editude] EDITUDE_SESSION_URL not set; collaboration disabled";
        return;
    }

    QNetworkRequest req(QUrl(m_sessionUrl));
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

        m_snapshotRevision = obj.value("snapshot_revision").toInt(0);
        m_serverRevision   = obj.value("server_revision").toInt(0);
        m_pendingOps       = obj.value("snapshot_ops").toArray();
        m_bootstrapReady   = true;

        // WebSocket open is deferred until the score finishes loading
        openScoreForSession();
    });
}

void EditudeService::openScoreForSession()
{
    if (!m_snapshotPath.isEmpty()) {
        m_projectFiles()->openProject(
            mu::project::ProjectFile(muse::io::path_t(m_snapshotPath)));
        // onNotationChanged() fires when MuseScore finishes loading the file
    }
    // No snapshot → MuseScore shows its normal start screen;
    // onNotationChanged() fires when user opens/creates any score.
}

void EditudeService::applyPendingOps()
{
    for (const QJsonValue& v : m_pendingOps) {
        m_applicator.apply(m_score, v.toObject().value("payload").toObject());
    }
    LOGD() << "[editude] applied" << m_pendingOps.size()
           << "bootstrap ops; at server_revision" << m_serverRevision;
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
        if (m_state == State::Live) {
            // Mid-session token refresh confirmation — no state change needed
            LOGD() << "[editude] token refreshed successfully";
            return;
        }
        QJsonObject join;
        join["type"] = "join";
        join["project_id"] = m_projectId;
        join["since_revision"] = m_serverRevision;
        m_socket->sendTextMessage(QJsonDocument(join).toJson(QJsonDocument::Compact));
        m_state = State::Joining;

    } else if (type == "sync") {
        m_state = State::Live;
        m_serverRevision = msg.value("server_revision").toInt(m_serverRevision);
        m_reconnectAttempt = 0;
        LOGD() << "[editude] joined/rejoined project" << m_projectId
               << "at revision" << m_serverRevision;

        // Flush ops buffered during reconnect
        if (!m_bufferedOps.isEmpty()) {
            for (const QJsonValue& v : m_bufferedOps) {
                QJsonObject out;
                out["type"]       = "op";
                out["client_seq"] = ++m_clientSeq;
                out["payload"]    = v.toObject().value("payload").toObject();
                m_socket->sendTextMessage(QJsonDocument(out).toJson(QJsonDocument::Compact));
            }
            LOGD() << "[editude] flushed" << m_bufferedOps.size() << "buffered ops";
            m_bufferedOps = QJsonArray();
        }

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
        if (m_state == State::Authenticating) {
            // Auth failed on reconnect attempt — close to trigger next backoff
            m_socket->close();
        }
    }
}

void EditudeService::onNotationChanged(mu::notation::INotationPtr notation)
{
    if (m_currentNotation) {
        m_currentNotation->undoStack()->changesChannel().disconnect(this);
    }

    m_score = nullptr;
    m_currentNotation = notation;

    if (!notation) {
        return;
    }

    auto* gs = dynamic_cast<mu::notation::IGetScore*>(notation.get());
    m_score = gs ? gs->score() : nullptr;

    // Apply bootstrap ops before going live
    if (m_bootstrapReady && !m_pendingOps.isEmpty()) {
        m_applyingRemote = true;
        applyPendingOps();
        m_applyingRemote = false;
        m_pendingOps = QJsonArray();
    }

    // Open WebSocket once (guard with m_wsEverConnected)
    if (m_bootstrapReady && !m_websocketUrl.isEmpty() && !m_wsEverConnected) {
        m_wsEverConnected = true;

        if (!m_tokenRefreshTimer) {
            m_tokenRefreshTimer = new QTimer(this);
            m_tokenRefreshTimer->setSingleShot(true);
            connect(m_tokenRefreshTimer, &QTimer::timeout,
                    this, &EditudeService::onTokenRefreshTimer);
        }
        if (!m_reconnectTimer) {
            m_reconnectTimer = new QTimer(this);
            m_reconnectTimer->setSingleShot(true);
            connect(m_reconnectTimer, &QTimer::timeout,
                    this, &EditudeService::onReconnectTimer);
        }
        scheduleTokenRefresh();
        _openWebSocket();
    }

    notation->undoStack()->changesChannel().onReceive(
        this,
        [this](const mu::engraving::ScoreChanges& changes) {
            onScoreChanges(changes);
        });
}

void EditudeService::onScoreChanges(const mu::engraving::ScoreChanges& changes)
{
    if (m_applyingRemote) {
        return;
    }

    if (m_state == State::Reconnecting) {
        const QVector<QJsonObject> ops = m_translator.translateAll(
            changes.changedObjects,
            changes.changedPropertyIdSet,
            m_projectId,
            m_applicator.elementToUuid());
        for (const QJsonObject& payload : ops) {
            if (m_bufferedOps.size() < 100) {
                QJsonObject entry;
                entry["payload"] = payload;
                m_bufferedOps.append(entry);
            }
        }
        return;
    }

    if (m_state != State::Live || !m_socket) {
        return;
    }

    const QVector<QJsonObject> ops = m_translator.translateAll(
        changes.changedObjects,
        changes.changedPropertyIdSet,
        m_projectId,
        m_applicator.elementToUuid());

    for (const QJsonObject& payload : ops) {
        QJsonObject msg;
        msg["type"]       = "op";
        msg["client_seq"] = ++m_clientSeq;
        msg["payload"]    = payload;
        m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    }
}

void EditudeService::_openWebSocket()
{
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected,          this, &EditudeService::onConnected);
    connect(m_socket, &QWebSocket::disconnected,       this, &EditudeService::onDisconnected);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &EditudeService::onServerMessage);
    m_state = State::Authenticating;
    m_socket->open(QUrl(m_websocketUrl));
}

void EditudeService::scheduleTokenRefresh()
{
    // Decode the middle segment of the JWT (no signature verification)
    const QStringList parts = m_token.split('.');
    if (parts.size() < 2) {
        return;
    }
    QString segment = parts[1];
    // Re-pad to a multiple of 4
    while (segment.size() % 4 != 0) {
        segment += '=';
    }
    const QByteArray decoded = QByteArray::fromBase64(
        segment.toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject payload = QJsonDocument::fromJson(decoded).object();
    if (!payload.contains("exp")) {
        return;
    }
    const qint64 exp = static_cast<qint64>(payload.value("exp").toDouble());
    m_tokenExpiry = exp;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 fireInSecs = std::max(exp - now - 300, static_cast<qint64>(1));
    m_tokenRefreshTimer->start(static_cast<int>(std::min(fireInSecs * 1000,
                                                          static_cast<qint64>(INT_MAX))));
}

void EditudeService::onTokenRefreshTimer()
{
    if (m_sessionUrl.isEmpty()) {
        return;
    }
    QNetworkRequest req(QUrl(m_sessionUrl));
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] token refresh fetch failed; retrying in 30 s";
            m_tokenRefreshTimer->start(30000);
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_token = obj.value("token").toString();
        scheduleTokenRefresh();
        if (m_state == State::Live || m_state == State::Joining) {
            if (m_socket) {
                QJsonObject msg;
                msg["type"]  = "auth";
                msg["token"] = m_token;
                m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
            }
        }
    });
}

void EditudeService::onDisconnected()
{
    if (m_state == State::Disconnected) {
        return;
    }
    LOGD() << "[editude] WS disconnected; attempt" << m_reconnectAttempt;
    m_state = State::Reconnecting;
    m_socket->deleteLater();
    m_socket = nullptr;
    const int delay = std::min(1000 * (1 << std::min(m_reconnectAttempt, 6)), 60000);
    m_reconnectTimer->start(delay);
}

void EditudeService::onReconnectTimer()
{
    if (m_sessionUrl.isEmpty()) {
        return;
    }
    QNetworkRequest req(QUrl(m_sessionUrl));
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_reconnectAttempt++;
            const int delay = std::min(1000 * (1 << std::min(m_reconnectAttempt, 6)), 60000);
            m_reconnectTimer->start(delay);
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_token = obj.value("token").toString();
        scheduleTokenRefresh();
        _openWebSocket();
    });
}
