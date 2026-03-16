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
#include <QMap>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QWebSocketProtocol>
#include "project/types/projecttypes.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/part.h"
#include "notation/internal/igetscore.h"
#include "log.h"
#include "qml/Editude/editudeannotationmodel.h"
#include "qml/Editude/editudepresencemodel.h"

using namespace mu::editude::internal;

EditudeService::EditudeService(const muse::modularity::ContextPtr& iocCtx, QObject* parent)
    : QObject(parent)
    , Contextable(iocCtx)
{
}

void EditudeService::setPresenceModel(EditudePresenceModel* model)
{
    m_presenceModel = model;
}

void EditudeService::setAnnotationModel(EditudeAnnotationModel* model)
{
    m_annotationModel = model;
}

void EditudeService::start()
{
    m_snapshotPath = QString::fromUtf8(qgetenv("EDITUDE_SNAPSHOT_PATH"));
    m_sessionUrl = QString::fromUtf8(qgetenv("EDITUDE_SESSION_URL"));

    if (m_sessionUrl.isEmpty()) {
        LOGD() << "[editude] EDITUDE_SESSION_URL not set; collaboration disabled";
        return;
    }

    QNetworkRequest req{QUrl{m_sessionUrl}};
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

        // Defer openProject() to the next event-loop iteration so it runs outside
        // the QNetworkReply::finished callback stack.  openProject() triggers QML
        // page navigation (openPageIfNeed) which must not be called re-entrantly
        // from inside a network reply handler.
        QTimer::singleShot(0, this, [this]() {
            openScoreForSession();
        });
    });

    if (m_playbackController()) {
        m_playbackController()->isPlayingChanged().onNotify(
            this,
            [this]() {
                onPlaybackStateChanged();
            });
    }
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
        m_reconnectAttempt = 0;

        // Apply peer ops that arrived while we were disconnected.  These are
        // the ops since our last known revision; they bring the local score up
        // to date before we re-submit our buffered offline edits.
        //
        // Note: ops we sent and got acked before disconnect are excluded by the
        // server (since_revision filter), so no double-apply for the normal case.
        // The narrow window where we sent but didn't receive an ack is accepted
        // as a v1 limitation — the OT system corrects the server-side state.
        const QJsonArray syncOps = msg.value("ops").toArray();
        if (!syncOps.isEmpty() && m_score) {
            m_applyingRemote = true;
            for (const QJsonValue& v : syncOps) {
                m_applicator.apply(m_score, v.toObject().value("payload").toObject());
            }
            m_applyingRemote = false;
            LOGD() << "[editude] applied" << syncOps.size() << "sync ops from peers";

            // Sync part registrations from the applicator into the translator.
            // When applyAddPart adopts an existing Part* (bootstrap de-dup), only
            // the applicator's m_partUuidToPart is updated.  The translator's
            // m_knownPartUuids must also learn about these parts; otherwise the
            // next onScoreChanges() call (e.g. from MuseScore internal init)
            // would generate spurious lazy AddPart ops for "unknown" parts.
            for (auto it = m_applicator.partUuidToPart().cbegin();
                 it != m_applicator.partUuidToPart().cend(); ++it) {
                m_translator.registerKnownPart(it.value(), it.key());
            }
        }

        m_serverRevision = msg.value("server_revision").toInt(m_serverRevision);
        LOGD() << "[editude] joined/rejoined project" << m_projectId
               << "at revision" << m_serverRevision;

        // Bootstrap: send AddPart ops for any parts in the local score that are
        // not yet known to the server (e.g. parts loaded from the blank MSCX file
        // before any OT session was established). This lets the server's
        // ProjectSession discover pre-existing parts so element ops succeed.
        if (m_score && m_serverRevision == 0) {
            const QVector<QJsonObject> bootstrapOps = m_translator.bootstrapPartsFromScore(
                m_score,
                [this](mu::engraving::Part* part, const QString& uuid) {
                    m_applicator.registerPart(part, uuid);
                });
            if (!bootstrapOps.isEmpty() && m_socket) {
                QJsonArray opsArray;
                for (const QJsonObject& op : bootstrapOps) {
                    opsArray.append(op);
                }
                QJsonObject batch;
                batch["type"]          = QStringLiteral("op_batch");
                batch["batch_id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
                batch["base_revision"] = 0;
                batch["ops"]           = opsArray;
                m_socket->sendTextMessage(
                    QJsonDocument(batch).toJson(QJsonDocument::Compact));
                LOGD() << "[editude] bootstrapped" << bootstrapOps.size()
                       << "pre-existing parts from MSCX";
            }
        }

        fetchAnnotations();

        // Drain batches buffered during the offline window.  Each batch retains
        // its original base_revision; the server OT-transforms against any ops
        // committed since then, including the sync ops applied above.
        if (!m_bufferedOps.isEmpty()) {
            for (const QJsonValue& v : m_bufferedOps) {
                QJsonObject batch = v.toObject();
                batch["client_seq"] = ++m_clientSeq;
                m_socket->sendTextMessage(QJsonDocument(batch).toJson(QJsonDocument::Compact));
            }
            LOGD() << "[editude] flushed" << m_bufferedOps.size() << "buffered batches";
            m_bufferedOps = QJsonArray();
        }

    } else if (type == "op_ack") {
        // Legacy ack format — kept for compatibility with older server versions.
        const int revision = msg.value("revision").toInt();
        if (revision > m_serverRevision) {
            m_serverRevision = revision;
        }
        LOGD() << "[editude] op_ack revision=" << revision;

    } else if (type == "op_batch_ack") {
        const int revision = msg.value("revision").toInt();
        if (revision > m_serverRevision) {
            m_serverRevision = revision;
        }
        LOGD() << "[editude] op_batch_ack batch_id=" << msg.value("batch_id").toString()
               << "revision=" << revision;

    } else if (type == "op") {
        // Legacy single-op broadcast — wrap and apply as a batch of one.
        if (m_playbackActive) {
            return;
        }
        const int revision = msg.value("revision").toInt();
        if (revision > m_serverRevision) {
            m_serverRevision = revision;
        }
        if (!m_score) {
            LOGW() << "[editude] received remote op but score not ready";
            return;
        }
        // Defer the score mutation to the next event-loop iteration.  This
        // prevents structural applicator calls (e.g. applyInsertBeats →
        // MasterScore::insertMeasure) from being invoked while a nested
        // QEventLoop is spinning inside EditudeTestServer::handleWaitRevision.
        // m_serverRevision is already updated above so the wait-revision poll
        // can detect completion before the deferred apply runs.
        const QJsonObject payload = msg.value("payload").toObject();
        QTimer::singleShot(0, this, [this, payload]() {
            if (!m_score) return;
            m_applyingRemote = true;
            m_applicator.apply(m_score, payload);
            m_applyingRemote = false;
            // Sync any new part registrations from applicator to translator
            // so subsequent onScoreChanges() calls don't generate spurious
            // lazy AddPart ops for parts added by remote peers.
            for (auto it = m_applicator.partUuidToPart().cbegin();
                 it != m_applicator.partUuidToPart().cend(); ++it) {
                m_translator.registerKnownPart(it.value(), it.key());
            }
        });

    } else if (type == "op_batch") {
        // Batch of sub-ops from a peer — apply as a single undo transaction.
        if (m_playbackActive) {
            return;
        }
        const int revision = msg.value("revision").toInt();
        if (revision > m_serverRevision) {
            m_serverRevision = revision;
        }
        if (!m_score) {
            LOGW() << "[editude] received remote op_batch but score not ready";
            return;
        }
        // Pass the full op_batch message to apply(); ScoreApplicator handles
        // startCmd/endCmd wrapping so the whole batch is one undo entry.
        // Defer the score mutation to the next event-loop iteration (see "op"
        // handler comment above for rationale).
        QTimer::singleShot(0, this, [this, msg]() {
            if (!m_score) return;
            m_applyingRemote = true;
            m_applicator.apply(m_score, msg);
            m_applyingRemote = false;
            // Sync part registrations (see "op" handler comment above).
            for (auto it = m_applicator.partUuidToPart().cbegin();
                 it != m_applicator.partUuidToPart().cend(); ++it) {
                m_translator.registerKnownPart(it.value(), it.key());
            }
        });

    } else if (type == "presence") {
        const QString cid = msg.value("contributor_id").toString();
        const QJsonObject sel = msg.value("selection").toObject();
        if (!cid.isEmpty()) {
            m_presenceOverlay.updateCursor(cid, sel);
            refreshPresenceModel();
        }

    } else if (type == "annotation_created") {
        if (m_annotationModel) {
            m_annotationModel->addAnnotation(msg.value("annotation").toObject());
        }

    } else if (type == "annotation_reply_created") {
        if (m_annotationModel) {
            m_annotationModel->incrementReplyCount(msg.value("annotation_id").toString());
        }

    } else if (type == "op_error") {
        const QString code = msg.value("code").toString();
        LOGD() << "[editude] op_error code=" << code;
        if ((code == "op_superseded" || code == "batch_superseded") && m_presenceModel) {
            m_presenceModel->showToast(
                "Your edit conflicted with a concurrent change and was not applied.");
        }

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
    m_presenceOverlay.clear();
    if (m_presenceModel) {
        m_presenceModel->setCanvasData({});
    }

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

    // Reset the metaTags snapshot for the new score so we don't carry over
    // stale diffs from a previously-loaded score.
    m_lastKnownMetaTags.clear();
    if (m_score) {
        for (const auto& [k, v] : m_score->metaTags()) {
            m_lastKnownMetaTags[QString::fromStdU16String(k.toStdU16String())]
                = QString::fromStdU16String(v.toStdU16String());
        }
    }

    // Apply bootstrap ops before going live
    if (m_bootstrapReady && !m_pendingOps.isEmpty()) {
        m_applyingRemote = true;
        m_applicator.bootstrapPartMap(m_score);
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

    notation->interaction()->selectionChanged().onNotify(
        this,
        [this]() {
            onSelectionChanged();
        });
}

void EditudeService::onScoreChanges(const mu::engraving::ScoreChanges& changes)
{
    if (m_applyingRemote) {
        return;
    }

    LOGD() << "[editude] onScoreChanges: changedObjects=" << changes.changedObjects.size()
           << "state=" << static_cast<int>(m_state)
           << "socket=" << (m_socket != nullptr);

    // Diff the current metaTags against the last known snapshot to detect
    // changes made via score->setMetaTag(), which bypasses the undo system
    // and does not appear in changedObjects or changedPropertyIdSet.
    QMap<QString, QString> changedMetaTags;
    if (m_score) {
        for (const auto& [k, v] : m_score->metaTags()) {
            const QString qk = QString::fromStdU16String(k.toStdU16String());
            const QString qv = QString::fromStdU16String(v.toStdU16String());
            if (m_lastKnownMetaTags.value(qk) != qv) {
                changedMetaTags[qk] = qv;
            }
        }
        // Update the persistent snapshot so we don't re-emit unchanged tags.
        if (!changedMetaTags.isEmpty()) {
            for (auto it = changedMetaTags.begin(); it != changedMetaTags.end(); ++it) {
                m_lastKnownMetaTags[it.key()] = it.value();
            }
        }
    }

    const QVector<QJsonObject> ops = m_translator.translateAll(
        changes.changedObjects,
        changes.changedPropertyIdSet,
        m_applicator.elementToUuid(),
        changedMetaTags);

    QVector<QJsonObject> allOps = ops;

    if (allOps.isEmpty()) {
        LOGD() << "[editude] onScoreChanges: translateAll produced 0 ops, dropping";
        return;
    }

    // Pack all ops from this undo transaction into a single op_batch.
    // They share base_revision so the server can OT-transform them correctly.
    const int baseRevision = m_serverRevision;
    QJsonArray opsArray;
    for (const QJsonObject& payload : allOps) {
        opsArray.append(payload);
    }

    if (m_state == State::Reconnecting) {
        // Buffer the whole batch as one unit; cap at 100 batches.
        if (m_bufferedOps.size() < 100) {
            QJsonObject batch;
            batch["type"]          = "op_batch";
            batch["batch_id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
            batch["base_revision"] = baseRevision;
            batch["ops"]           = opsArray;
            m_bufferedOps.append(batch);
        }
        LOGD() << "[editude] onScoreChanges: buffered" << allOps.size() << "ops (reconnecting)";
        return;
    }

    if (m_state != State::Live || !m_socket) {
        LOGW() << "[editude] onScoreChanges: NOT sending" << allOps.size()
               << "ops — state=" << static_cast<int>(m_state)
               << "socket=" << (m_socket != nullptr);
        return;
    }

    QJsonObject msg;
    msg["type"]          = "op_batch";
    msg["batch_id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg["client_seq"]    = ++m_clientSeq;
    msg["base_revision"] = baseRevision;
    msg["ops"]           = opsArray;

    for (const QJsonObject& op : allOps) {
        LOGD() << "[editude] onScoreChanges: sending op type=" << op.value("type").toString()
               << "base_rev=" << baseRevision;
    }
    m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
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
    QNetworkRequest req{QUrl{m_sessionUrl}};
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
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    const int delay = m_immediateReconnect
        ? 0
        : std::min(1000 * (1 << std::min(m_reconnectAttempt, 6)), 60000);
    m_immediateReconnect = false;
    m_reconnectTimer->start(delay);
}

void EditudeService::onReconnectTimer()
{
    if (m_sessionUrl.isEmpty()) {
        return;
    }
    QNetworkRequest req{QUrl{m_sessionUrl}};
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

void EditudeService::onPlaybackStateChanged()
{
    if (!m_playbackController()) {
        return;
    }
    const bool playing = m_playbackController()->isPlaying();
    if (playing) {
        m_playbackActive = true;
        LOGD() << "[editude] playback started; pausing remote op application at revision"
               << m_serverRevision;
    } else {
        m_playbackActive = false;
        LOGD() << "[editude] playback stopped; reconnecting for catch-up from revision"
               << m_serverRevision;
        // Treat playback end as a soft disconnect: close the WS and let the reconnect
        // logic replay ops since m_serverRevision via the standard join/sync path.
        // m_immediateReconnect bypasses exponential backoff for this intentional reconnect.
        if (m_socket && m_state == State::Live) {
            m_immediateReconnect = true;
            m_socket->close();
        }
    }
}

void EditudeService::onSelectionChanged()
{
    if (m_applyingRemote || m_state != State::Live || !m_socket) {
        return;
    }
    if (!m_presenceThrottle) {
        m_presenceThrottle = new QTimer(this);
        m_presenceThrottle->setSingleShot(true);
        m_presenceThrottle->setInterval(80);
        connect(m_presenceThrottle, &QTimer::timeout, this, [this]() {
            if (!m_currentNotation || m_state != State::Live || !m_socket) {
                return;
            }
            const auto sel = m_currentNotation->interaction()->selection();
            QJsonObject msg;
            msg["type"]      = "presence";
            msg["selection"] = buildSelectionPayload(sel);
            m_socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
        });
    }
    m_presenceThrottle->start();
}

QJsonObject EditudeService::buildSelectionPayload(const mu::notation::INotationSelectionPtr& sel)
{
    QJsonObject obj;
    obj["element_ids"] = QJsonArray();
    obj["start_staff"] = 0;
    obj["end_staff"]   = 0;
    obj["start_tick"]  = 0;
    obj["end_tick"]    = 0;

    if (!sel || sel->isNone()) {
        obj["state"] = "none";
        return obj;
    }

    if (sel->isRange()) {
        obj["state"] = "range";
        const auto range = sel->range();
        if (range) {
            obj["start_staff"] = static_cast<int>(range->startStaffIndex());
            obj["end_staff"]   = static_cast<int>(range->endStaffIndex());
            obj["start_tick"]  = range->startTick().ticks();
            obj["end_tick"]    = range->endTick().ticks();
        }
        return obj;
    }

    obj["state"] = "single";
    const auto& uuidMap = m_applicator.elementToUuid();
    QJsonArray ids;
    for (const auto* elem : sel->elements()) {
        auto it = uuidMap.find(const_cast<mu::engraving::EngravingItem*>(elem));
        if (it != uuidMap.end()) {
            ids.append(it.value());
        }
    }
    obj["element_ids"] = ids;
    return obj;
}

void EditudeService::fetchAnnotations()
{
    if (m_sessionUrl.isEmpty() || m_projectId.isEmpty() || !m_annotationModel) {
        return;
    }

    // Derive the annotations REST URL from the session URL base.
    // EDITUDE_SESSION_URL is expected to be something like
    //   http://host:port/projects/{pid}/session-bootstrap?token=...
    // Strip query + trailing path segment to get the project base URL.
    QUrl sessionUrl(m_sessionUrl);
    // Build: http://host:port/projects/{pid}/annotations
    QUrl annotationsUrl;
    annotationsUrl.setScheme(sessionUrl.scheme());
    annotationsUrl.setHost(sessionUrl.host());
    annotationsUrl.setPort(sessionUrl.port());
    annotationsUrl.setPath(QString("/projects/%1/annotations").arg(m_projectId));

    QNetworkRequest req(annotationsUrl);
    req.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] annotations fetch failed:" << reply->errorString();
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).array();
        m_annotationModel->loadFromJson(arr);
        LOGD() << "[editude] loaded" << arr.size() << "annotations";
    });
}

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

void EditudeService::connectToSession(const QString& sessionUrl)
{
    // Abort any existing socket connection.
    // Null the member and sever signal connections BEFORE calling abort() — Qt may
    // emit disconnected() synchronously from abort(), which would re-enter
    // onDisconnected() and leave m_socket dangling if we haven't nulled it first.
    if (m_socket) {
        QWebSocket* oldSocket = m_socket;
        m_socket = nullptr;
        oldSocket->disconnect(this);  // prevent onDisconnected() re-entrancy
        oldSocket->abort();
        oldSocket->deleteLater();
    }

    // Stop any pending timers
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_tokenRefreshTimer) {
        m_tokenRefreshTimer->stop();
    }

    // Reset all state to match a fresh start()
    m_state = State::Disconnected;
    m_serverRevision = 0;
    m_clientSeq = 0;
    m_bootstrapReady = false;
    m_pendingOps = QJsonArray();
    m_bufferedOps = QJsonArray();
    m_reconnectAttempt = 0;
    m_wsEverConnected = false;
    m_immediateReconnect = false;
    m_token.clear();
    m_websocketUrl.clear();
    m_projectId.clear();
    m_snapshotRevision = 0;
    m_tokenExpiry = 0;

    // Reset OT subsystems so stale Part*/Element* ↔ UUID mappings from the
    // previous project don't leak into the new session.
    m_translator.reset();
    m_applicator.reset();
    m_lastKnownMetaTags.clear();

    // Undo ALL local changes to restore the score to its pristine template
    // state.  Each test session must start from a clean, known score —
    // accumulated edits from previous tests (time-signature changes, added
    // parts, etc.) would otherwise confound subsequent tests.
    //
    // Guard with m_applyingRemote: each undoRedo fires changesChannel →
    // onScoreChanges → translateAll.  Without the guard, translateAll would
    // lazily register the template part (resolvePartUuid), and bootstrap
    // would then skip it — leaving the server without an AddPart for the
    // template.  The guard causes onScoreChanges to return immediately.
    if (m_score) {
        m_applyingRemote = true;
        while (m_score->undoStack()->canUndo()) {
            m_score->undoRedo(true, nullptr);
        }
        m_applyingRemote = false;

        // Re-populate the metaTags snapshot from the pristine score so the
        // first onScoreChanges after this connectToSession doesn't see every
        // existing tag as "changed" and emit spurious SetScoreMetadata ops.
        // (onNotationChanged does this for first-time score load, but
        // connectToSession bypasses onNotationChanged on reconnects.)
        m_lastKnownMetaTags.clear();
        for (const auto& [k, v] : m_score->metaTags()) {
            m_lastKnownMetaTags[QString::fromStdU16String(k.toStdU16String())]
                = QString::fromStdU16String(v.toStdU16String());
        }
    }

    // Set new session URL and trigger the bootstrap fetch — same logic as start()
    m_sessionUrl = sessionUrl;

    QNetworkRequest req{QUrl{m_sessionUrl}};
    QNetworkReply* reply = m_nam.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] connectToSession fetch failed:" << reply->errorString();
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_token = obj.value("token").toString();
        m_websocketUrl = obj.value("websocket_url").toString();
        m_projectId = obj.value("project_id").toString();

        if (m_token.isEmpty() || m_websocketUrl.isEmpty() || m_projectId.isEmpty()) {
            LOGW() << "[editude] connectToSession response missing required fields";
            return;
        }

        m_snapshotRevision = obj.value("snapshot_revision").toInt(0);
        m_serverRevision   = obj.value("server_revision").toInt(0);
        m_pendingOps       = obj.value("snapshot_ops").toArray();
        m_bootstrapReady   = true;

        // Ensure timers are created (they may have been nil if start() was never called)
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

        // Score is already loaded (undo-all above restored it to pristine
        // state).  Connect the WebSocket immediately.
        if (m_score != nullptr) {
            m_wsEverConnected = true;
            scheduleTokenRefresh();
            _openWebSocket();
            return;
        }

        // No score loaded yet (first /connect after startup).  Defer the
        // openProject() call to the next event-loop iteration so it runs
        // outside the QNetworkReply::finished callback stack — openProject()
        // triggers QML navigation that must not be called re-entrantly.
        QTimer::singleShot(0, this, [this]() {
            if (m_score == nullptr) {
                openScoreForSession();
            }
        });
    });
}

QString EditudeService::stateForTest() const
{
    switch (m_state) {
    case State::Disconnected:   return QStringLiteral("disconnected");
    case State::Authenticating: return QStringLiteral("authenticating");
    case State::Joining:        return QStringLiteral("joining");
    case State::Live:           return QStringLiteral("live");
    case State::Reconnecting:   return QStringLiteral("reconnecting");
    default:                    return QStringLiteral("unknown");
    }
}

#endif // MUE_BUILD_EDITUDE_TEST_SERVER

void EditudeService::refreshPresenceModel()
{
    if (!m_presenceModel || !m_score) {
        return;
    }

    QVector<QPair<QColor, QVector<muse::RectF>>> data;

    for (const auto& cursor : m_presenceOverlay.cursors()) {
        if (cursor.state == "none" || cursor.state.isEmpty()) {
            continue;
        }

        QVector<muse::RectF> rects;

        if (cursor.state == "range") {
            rects = PresenceOverlay::reprojectRange(m_score, cursor);
        } else if (cursor.state == "single") {
            const auto& uuidMap = m_applicator.elementToUuid();
            for (auto it = uuidMap.begin(); it != uuidMap.end(); ++it) {
                if (cursor.elementIds.contains(it.value())) {
                    const auto* engItem = dynamic_cast<const mu::engraving::EngravingItem*>(it.key());
                    if (engItem) {
                        rects.append(engItem->canvasBoundingRect());
                    }
                }
            }
        }

        if (!rects.isEmpty()) {
            data.append({ cursor.color, rects });
        }
    }

    m_presenceModel->setCanvasData(data);
}
