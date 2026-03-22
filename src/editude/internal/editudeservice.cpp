/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 Alexis Goodfellow
 *
 * CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
 * EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
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
#include <QApplication>
#include <QDir>
#include <QHttpMultiPart>
#include <QWindow>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QUrl>
#include <QWebSocketProtocol>
#include "project/types/projecttypes.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/engravingproject.h"
#include "engraving/infrastructure/mscwriter.h"
#include "global/io/buffer.h"
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
    if (model) {
        model->setService(this);
    }
}

void EditudeService::setSnapshotPath(const QString& path)
{
    m_snapshotPath = path;
}

void EditudeService::start()
{
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

        // Defer openProject() to the next event-loop iteration so it runs
        // outside the QNetworkReply::finished callback stack.  openProject()
        // triggers QML page navigation (openPageIfNeed) which must not be
        // called re-entrantly from inside a network reply handler.
        QTimer::singleShot(0, this, [this]() {
            openScoreForSession();
        });

        // Listen for the startup scenario opening a page (typically HOME).
        // If we loaded a score above, the startup scenario's HOME page will
        // clobber our NOTATION page.  When opened() fires for HOME, we
        // navigate back to NOTATION.  For the new-project template flow,
        // we dispatch "file-new" instead.  Both use this single handler.
        m_interactive()->opened().onReceive(this, [this](const muse::Uri&) {
            if (m_fileNewPending) {
                m_fileNewPending = false;
                m_dispatcher()->dispatch("file-new");
                return;
            }
            if (m_reclaimNotation) {
                m_reclaimNotation = false;
                m_interactive()->open(muse::Uri("musescore://notation"));
                requestNotationFocus();
            }
        });
    });

    if (m_playbackController()) {
        m_playbackController()->isPlayingChanged().onNotify(
            this,
            [this]() {
                onPlaybackStateChanged();
            });
    }

    // Install an event filter on QApplication to intercept Close/Quit events.
    // When the user closes MuseScore, this marks the score as saved BEFORE
    // ApplicationActionController's filter runs, suppressing the "Save
    // changes?" dialog.  Editude projects are persisted via OT ops — there's
    // nothing to "save" locally.  Qt event filters fire in reverse install
    // order, and editude initializes after appshell, so ours runs first.
    qApp->installEventFilter(this);
}

bool EditudeService::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Close || event->type() == QEvent::Quit) {
        // Mark the score as saved so the "Save changes?" dialog is suppressed.
        // This runs before ApplicationActionController's event filter (which
        // calls closeOpenedProject → needSave check) because Qt invokes
        // filters in reverse installation order and editude installs after
        // appshell.
        markScoreSaved();
    }
    return QObject::eventFilter(watched, event);
}

void EditudeService::openScoreForSession()
{
    if (!m_snapshotPath.isEmpty()) {
        if (!m_score) {
            // Score not loaded yet.  Open the snapshot file via MuseScore's
            // own openProject().  It internally opens the notation page after
            // loading the score, which correctly wires up the navigation
            // context (ScoreView panel active → UiCtxProjectFocused → actions
            // enabled).  openProject() fires onNotationChanged() synchronously,
            // which sets m_score and calls bootstrapAndConnect().
            //
            // If the startup scenario already loaded the file (via
            // setStartupScoreFile in registerExports), m_score is set and we
            // skip this — no double-open.
            m_projectFiles()->openProject(
                mu::project::ProjectFile(muse::io::path_t(m_snapshotPath)));
        }
        bootstrapAndConnect();
        // The startup scenario will open HOME after our NOTATION page,
        // clobbering it.  Set the flag so the opened() handler in start()
        // navigates back to NOTATION when HOME fires.
        m_reclaimNotation = true;
    } else if (m_snapshotRevision == 0) {
        // m_snapshotRevision == 0 — brand-new project.  Open MuseScore's
        // "New Score" wizard so the user can pick a template/instruments.
        // onNotationChanged() fires when they finish the wizard.
        //
        // The "file-new" action opens a QML dialog that requires the
        // InteractiveProvider to be fully initialized.  At this point in
        // startup, the main window has not opened yet — dispatching now
        // would crash (assertion in Interactive::onClose because the dialog
        // was never registered in m_stack).  The opened() handler in
        // start() watches m_fileNewPending and dispatches "file-new" when
        // the startup scenario shows the HOME page.
        m_needsInitialSnapshot = true;
        m_fileNewPending = true;
    }
}

void EditudeService::bootstrapAndConnect()
{
    if (!m_bootstrapReady || !m_score) {
        return;
    }

    if (!m_pendingOps.isEmpty()) {
        m_applyingRemote = true;
        m_applicator.bootstrapPartMap(m_score);
        applyPendingOps();
        m_applyingRemote = false;
        m_pendingOps = QJsonArray();

        // Sync part registrations from applicator → translator.  When
        // applyAddPart adopts an existing Part* (bootstrap de-dup), only the
        // applicator's m_partUuidToPart is updated.  The translator must also
        // learn about these parts; otherwise the next onScoreChanges() would
        // generate spurious AddPart ops for "unknown" parts.
        for (auto it = m_applicator.partUuidToPart().cbegin();
             it != m_applicator.partUuidToPart().cend(); ++it) {
            m_translator.registerKnownPart(it.value(), it.key());
        }
    }

    // Clear any stale editing state left over from the bootstrap op replay.
    // NotationActionController::isEditingElement() gates ALL action
    // enablement (note-input, arrow keys, toolbar buttons) — if the
    // interaction reports editing or dragging in progress, actions are
    // unconditionally disabled.  Applying bootstrap ops via startCmd/endCmd
    // can leave m_editData.element set as a side effect, so we explicitly
    // end editing here.
    if (m_currentNotation) {
        m_currentNotation->interaction()->endEditElement();
    }

    if (!m_websocketUrl.isEmpty() && !m_wsEverConnected) {
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

    if (m_presenceModel) {
        m_presenceModel->notifyScoreReady();
    }

    // Mark score as saved after bootstrap — editude projects are persisted
    // via OT ops to the server, not local files.
    markScoreSaved();
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
            markScoreSaved();
            LOGD() << "[editude] applied" << syncOps.size() << "sync ops from peers";
        }

        // Sync part registrations from the applicator into the translator.
        // This must run even when syncOps is empty: bootstrapAndConnect()
        // may have applied AddPart ops (from the snapshot_ops bootstrap)
        // that updated the applicator's part map but didn't sync to the
        // translator.  Without this, the next onScoreChanges() would
        // generate spurious AddPart ops for "unknown" parts.
        if (m_score) {
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
                QString batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                QJsonObject batch;
                batch["type"]          = QStringLiteral("op_batch");
                batch["batch_id"]      = batchId;
                batch["base_revision"] = 0;
                batch["ops"]           = opsArray;
                m_socket->sendTextMessage(
                    QJsonDocument(batch).toJson(QJsonDocument::Compact));
                LOGD() << "[editude] bootstrapped" << bootstrapOps.size()
                       << "pre-existing parts from MSCX";

                // Defer the initial snapshot upload until this batch is acked.
                // The ack updates m_serverRevision to include the AddPart ops,
                // so the snapshot is stored at the correct revision and won't
                // cause duplicate AddPart ops on the next session_bootstrap.
                if (m_needsInitialSnapshot) {
                    m_bootstrapBatchId = batchId;
                }
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

        // Upload the initial snapshot for brand-new projects (wizard flow).
        // If bootstrap AddPart ops were sent above, the upload is deferred
        // until their op_batch_ack arrives (so m_serverRevision is correct).
        // If no bootstrap ops were needed, upload immediately.
        if (m_needsInitialSnapshot && m_bootstrapBatchId.isEmpty() && m_score) {
            m_needsInitialSnapshot = false;
            uploadInitialSnapshot();
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

        // Deferred initial snapshot upload: the bootstrap AddPart batch has
        // been acked, so m_serverRevision now includes the parts.  Upload
        // the snapshot at this revision so session_bootstrap won't return
        // the AddPart ops (they precede the snapshot revision).
        if (m_needsInitialSnapshot
            && !m_bootstrapBatchId.isEmpty()
            && msg.value("batch_id").toString() == m_bootstrapBatchId) {
            m_needsInitialSnapshot = false;
            m_bootstrapBatchId.clear();
            if (m_score) {
                uploadInitialSnapshot();
            }
        }

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
            markScoreSaved();
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
            markScoreSaved();
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
            m_annotationModel->addReply(
                msg.value("annotation_id").toString(),
                msg.value("reply").toObject()
            );
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

    // Apply bootstrap ops and open the WebSocket if the session info
    // has already arrived from the network.  When the snapshot path is
    // passed as a command-line argument, StartupScenario loads the file
    // before the network reply — in that case bootstrapAndConnect() is
    // a no-op here and runs later from openScoreForSession().
    bootstrapAndConnect();

    // Subscribe to the project-level needSave notification.  MuseScore marks
    // the project dirty on undo-stack changes, view-state changes, solo/mute
    // changes, and audio-settings changes.  In an editude session, the score
    // is persisted via OT ops — there is nothing to "save" locally.  When
    // needSave fires as true, we defer a markScoreSaved() to clear it.
    // The deferred call avoids infinite recursion: markScoreSaved() fires the
    // same notification, but by then needSave().val is false so we skip.
    if (auto project = m_globalContext()->currentProject()) {
        project->needSave().notification.onNotify(this, [this]() {
            if (auto proj = m_globalContext()->currentProject()) {
                if (proj->needSave().val) {
                    QTimer::singleShot(0, this, [this]() {
                        markScoreSaved();
                    });
                }
            }
        });
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

void EditudeService::markScoreSaved()
{
    if (!m_score || !m_score->masterScore()) {
        return;
    }
    m_score->masterScore()->setSaved(true);

    // Fire the project-level needSave notification so the title bar
    // re-reads needSave().val (now false) and clears the asterisk.
    // setSaved(true) alone doesn't trigger this notification.
    if (auto project = m_globalContext()->currentProject()) {
        project->needSave().notification.notify();
    }
}

QUrl EditudeService::deriveServerBaseUrl() const
{
    QUrl wsUrl(m_websocketUrl);
    QUrl base;
    base.setScheme(wsUrl.scheme() == QStringLiteral("wss") ? QStringLiteral("https")
                                                           : QStringLiteral("http"));
    base.setHost(wsUrl.host());
    base.setPort(wsUrl.port());
    return base;
}

void EditudeService::uploadInitialSnapshot()
{
    if (!m_score || !m_score->masterScore()) {
        LOGW() << "[editude] uploadInitialSnapshot: no score available";
        return;
    }

    // Serialize the score to MSCZ bytes in memory.
    muse::ByteArray msczData;
    {
        muse::io::Buffer buf(&msczData);

        mu::engraving::MscWriter::Params params;
        params.device = &buf;
        params.filePath = muse::io::path_t("snapshot.mscz");
        params.mode = mu::engraving::MscIoMode::Zip;

        mu::engraving::MscWriter writer(params);
        muse::Ret ret = writer.open();
        if (!ret) {
            LOGW() << "[editude] uploadInitialSnapshot: MscWriter::open() failed";
            return;
        }

        auto engProject = m_score->masterScore()->project().lock();
        if (!engProject) {
            LOGW() << "[editude] uploadInitialSnapshot: no EngravingProject";
            return;
        }

        bool ok = engProject->writeMscz(writer, false);
        writer.close();

        if (!ok || writer.hasError()) {
            LOGW() << "[editude] uploadInitialSnapshot: serialization failed";
            return;
        }
    }

    LOGD() << "[editude] uploadInitialSnapshot: serialized"
           << msczData.size() << "bytes at revision" << m_serverRevision;

    // Build the upload URL from the WebSocket URL.
    QUrl uploadUrl = deriveServerBaseUrl();
    uploadUrl.setPath(QString("/projects/%1/snapshots").arg(m_projectId));

    // Multipart form: revision (int) + file (binary).
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart revisionPart;
    revisionPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"revision\"")));
    revisionPart.setBody(QByteArray::number(m_serverRevision));
    multiPart->append(revisionPart);

    QHttpPart filePart;
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"snapshot.mscz\"")));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                       QVariant(QStringLiteral("application/octet-stream")));
    filePart.setBody(QByteArray(reinterpret_cast<const char*>(msczData.constData()),
                                static_cast<qsizetype>(msczData.size())));
    multiPart->append(filePart);

    QNetworkRequest req(uploadUrl);
    req.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());

    QNetworkReply* reply = m_nam.post(req, multiPart);
    multiPart->setParent(reply);

    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] initial snapshot upload failed:" << reply->errorString();
        } else {
            LOGD() << "[editude] initial snapshot uploaded successfully";
        }
    });
}

void EditudeService::requestNotationFocus()
{
    // Three things must happen for keyboard shortcuts and toolbar to work:
    //
    // 1. The OS must recognize this window as the active application
    //    (→ keyboard events are delivered by the OS).
    // 2. The ScoreView navigation panel must be active (→ UiCtxProjectFocused
    //    → actions like note-input are enabled).
    // 3. The NotationPaintView must have QML active focus (→ ShortcutOverride
    //    events are delivered to it → keyboard shortcuts fire).
    //
    // On macOS, when launched via subprocess.Popen, the MuseScore window
    // appears but the APPLICATION may not be activated — the parent process
    // retains active-app status.  requestActivate() calls
    // [NSApp activateIgnoringOtherApps:YES] under the hood.
    //
    // requestActivateByName activates the navigation hierarchy; the explicit
    // navigationChanged().notify() forces UiContextResolver to re-evaluate
    // even if the controls were already active from a prior attempt.
    //
    // Three timed attempts cover sync, near-sync, and async page creation.
    // These only handle OS-level window activation and MuseScore's internal
    // navigation panel hierarchy.  Page navigation (HOME → NOTATION) is
    // handled signal-based via the opened() handler in start().
    for (int delay : {0, 200, 500}) {
        QTimer::singleShot(delay, this, [this, delay]() {
            LOGD() << "[editude] requestNotationFocus @" << delay
                   << "ms: appState=" << qApp->applicationState();

            if (QWindow* w = m_mainWindow()->qWindow()) {
                w->requestActivate();
            }

            bool ok = m_navigationController()->requestActivateByName(
                "NotationView", "ScoreView", "Score");
            LOGD() << "[editude] requestActivateByName returned " << ok;

            m_navigationController()->navigationChanged().notify();
        });
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
    if (m_websocketUrl.isEmpty() || m_projectId.isEmpty() || !m_annotationModel) {
        return;
    }

    QUrl annotationsUrl = deriveServerBaseUrl();
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

void EditudeService::createAnnotation(const QString& partId, qint64 startNum, qint64 startDen,
                                      qint64 endNum, qint64 endDen, const QString& body)
{
    if (m_token.isEmpty() || m_projectId.isEmpty()) {
        return;
    }

    QUrl url = deriveServerBaseUrl();
    url.setPath(QString("/projects/%1/annotations").arg(m_projectId));

    QJsonObject payload;
    payload["part_id"]        = partId;
    payload["start_beat_num"] = startNum;
    payload["start_beat_den"] = startDen;
    payload["end_beat_num"]   = endNum;
    payload["end_beat_den"]   = endDen;
    payload["body"]           = body;

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] create annotation failed:" << reply->errorString();
        }
        // The server broadcasts annotation_created via WS; the model handles it there.
    });
}

void EditudeService::resolveAnnotation(const QString& annotationId, bool resolved)
{
    if (m_token.isEmpty() || m_projectId.isEmpty()) {
        return;
    }

    QUrl url = deriveServerBaseUrl();
    url.setPath(QString("/projects/%1/annotations/%2").arg(m_projectId, annotationId));

    QJsonObject payload;
    payload["resolved"] = resolved;

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // PATCH via custom verb
    QNetworkReply* reply = m_nam.sendCustomRequest(req, "PATCH",
        QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, annotationId, resolved]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] resolve annotation failed:" << reply->errorString();
            return;
        }
        if (m_annotationModel) {
            QJsonObject fields;
            fields["resolved"] = resolved;
            m_annotationModel->updateAnnotation(annotationId, fields);
        }
    });
}

void EditudeService::createReply(const QString& annotationId, const QString& body)
{
    if (m_token.isEmpty() || m_projectId.isEmpty()) {
        return;
    }

    QUrl url = deriveServerBaseUrl();
    url.setPath(QString("/projects/%1/annotations/%2/replies").arg(m_projectId, annotationId));

    QJsonObject payload;
    payload["body"] = body;

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[editude] create reply failed:" << reply->errorString();
        }
        // The server broadcasts annotation_reply_created via WS; the model handles it there.
    });
}

QJsonObject EditudeService::getSelectionAnchor()
{
    QJsonObject anchor;
    static const QString SCORE_LEVEL_PART_ID = QStringLiteral("_score");

    if (!m_currentNotation) {
        anchor["part_id"]        = SCORE_LEVEL_PART_ID;
        anchor["start_beat_num"] = 0;
        anchor["start_beat_den"] = 1;
        anchor["end_beat_num"]   = 0;
        anchor["end_beat_den"]   = 1;
        return anchor;
    }

    const auto sel = m_currentNotation->interaction()->selection();
    if (!sel || sel->isNone()) {
        anchor["part_id"]        = SCORE_LEVEL_PART_ID;
        anchor["start_beat_num"] = 0;
        anchor["start_beat_den"] = 1;
        anchor["end_beat_num"]   = 0;
        anchor["end_beat_den"]   = 1;
        return anchor;
    }

    if (sel->isRange()) {
        const auto range = sel->range();
        if (range) {
            const auto startFrac = range->startTick().reduced();
            const auto endFrac   = range->endTick().reduced();
            anchor["start_beat_num"] = startFrac.numerator();
            anchor["start_beat_den"] = startFrac.denominator();
            anchor["end_beat_num"]   = endFrac.numerator();
            anchor["end_beat_den"]   = endFrac.denominator();

            // Resolve part from the range's start staff
            const auto startStaff = range->startStaffIndex();
            if (m_score) {
                const auto& partUuids = m_translator.knownPartUuids();
                for (auto* part : m_score->parts()) {
                    staff_idx_t firstStaff = m_score->staffIdx(part);
                    staff_idx_t lastStaff  = firstStaff + part->nstaves();
                    if (startStaff >= firstStaff && startStaff < lastStaff) {
                        auto it = partUuids.find(part);
                        if (it != partUuids.end()) {
                            anchor["part_id"] = it.value();
                        }
                        break;
                    }
                }
            }
            if (!anchor.contains("part_id")) {
                anchor["part_id"] = SCORE_LEVEL_PART_ID;
            }
            return anchor;
        }
    }

    // Single element selection
    const auto& elements = sel->elements();
    if (!elements.empty()) {
        const auto* elem = elements.first();
        const auto tick = elem->tick().reduced();
        anchor["start_beat_num"] = tick.numerator();
        anchor["start_beat_den"] = tick.denominator();
        anchor["end_beat_num"]   = tick.numerator();
        anchor["end_beat_den"]   = tick.denominator();

        if (auto* part = elem->part()) {
            const auto& partUuids = m_translator.knownPartUuids();
            auto it = partUuids.find(part);
            if (it != partUuids.end()) {
                anchor["part_id"] = it.value();
            }
        }
        if (!anchor.contains("part_id")) {
            anchor["part_id"] = SCORE_LEVEL_PART_ID;
        }
        return anchor;
    }

    // Fallback: score-level
    anchor["part_id"]        = SCORE_LEVEL_PART_ID;
    anchor["start_beat_num"] = 0;
    anchor["start_beat_den"] = 1;
    anchor["end_beat_num"]   = 0;
    anchor["end_beat_den"]   = 1;
    return anchor;
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

    // Cancel any pending sequenceRemoved wait from a prior connectToSession call.
    m_audioPlayback()->sequenceRemoved().disconnect(this);

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
    m_needsInitialSnapshot = false;
    m_fileNewPending = false;
    m_fileOpenPending = false;
    m_reclaimNotation = false;
    m_bootstrapBatchId.clear();
    m_token.clear();
    m_websocketUrl.clear();
    m_projectId.clear();
    m_snapshotPath.clear();
    m_snapshotRevision = 0;
    m_tokenExpiry = 0;

    // Reset OT subsystems so stale Part*/Element* ↔ UUID mappings from the
    // previous project don't leak into the new session.
    m_translator.reset();
    m_applicator.reset();
    m_lastKnownMetaTags.clear();

    // Disconnect from the current score's changes channel before closing
    // to prevent spurious onScoreChanges callbacks during teardown.
    if (m_currentNotation) {
        m_currentNotation->undoStack()->changesChannel().disconnect(this);
    }

    // Save the raw score pointer before clearing it — we need it later in the
    // deferred callback to mark the score as saved (suppressing the "Save
    // changes?" dialog) immediately before closeOpenedProject().  The Score*
    // remains valid until closeOpenedProject() actually tears it down.
    mu::engraving::Score* scoreToClose = m_score;

    m_score = nullptr;
    m_currentNotation = nullptr;

    // Set new session URL and trigger the bootstrap fetch — same logic as start()
    m_sessionUrl = sessionUrl;

    QNetworkRequest req{QUrl{m_sessionUrl}};
    QNetworkReply* reply = m_nam.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, scoreToClose]() {
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

        // Helper: close the current project, wait for audio teardown, then
        // open the new score via openScoreForSession().
        auto closeAndReopen = [this, scoreToClose]() {
            QTimer::singleShot(0, this, [this, scoreToClose]() {
                if (scoreToClose) {
                    scoreToClose->masterScore()->setSaved(true);
                }

                auto oldSeqId = m_playbackController()->currentTrackSequenceId();
                m_projectFiles()->closeOpenedProject(false);

                if (oldSeqId >= 0) {
                    m_audioPlayback()->sequenceRemoved().onReceive(
                        this,
                        [this, oldSeqId](muse::audio::TrackSequenceId removedId) {
                            if (removedId != oldSeqId) {
                                return;
                            }
                            m_audioPlayback()->sequenceRemoved().disconnect(this);
                            openScoreForSession();
                        },
                        muse::async::Asyncable::Mode::SetReplace);
                } else {
                    openScoreForSession();
                }
            });
        };

        // If the project has a snapshot on the server, fetch it before
        // closing the current project so openScoreForSession() can load it.
        if (m_snapshotRevision > 0) {
            QUrl snapshotUrl = deriveServerBaseUrl();
            snapshotUrl.setPath(QString("/projects/%1/latest-snapshot").arg(m_projectId));

            QNetworkRequest snapReq(snapshotUrl);
            snapReq.setRawHeader("Authorization",
                                 QString("Bearer %1").arg(m_token).toUtf8());

            QNetworkReply* snapReply = m_nam.get(snapReq);
            connect(snapReply, &QNetworkReply::finished, this,
                    [this, snapReply, closeAndReopen]() {
                snapReply->deleteLater();
                if (snapReply->error() == QNetworkReply::NoError) {
                    const QByteArray msczData = snapReply->readAll();
                    QTemporaryFile tmpFile;
                    tmpFile.setFileTemplate(
                        QDir::tempPath() + "/editude_XXXXXX.mscz");
                    tmpFile.setAutoRemove(false);
                    if (tmpFile.open()) {
                        tmpFile.write(msczData);
                        tmpFile.close();
                        m_snapshotPath = tmpFile.fileName();
                        LOGD() << "[editude] connectToSession: snapshot written to"
                               << m_snapshotPath << "(" << msczData.size() << "bytes)";
                    }
                } else {
                    LOGW() << "[editude] connectToSession: snapshot fetch failed:"
                           << snapReply->errorString();
                }
                closeAndReopen();
            });
        } else {
            // No snapshot — new project; proceed directly.
            closeAndReopen();
        }
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
