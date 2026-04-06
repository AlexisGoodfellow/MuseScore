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
#pragma once

#include <memory>

#include <QJsonArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QUuid>
#include <QEvent>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWebSocket>

#include "global/modularity/ioc.h"
#include "global/async/asyncable.h"
#include "actions/iactionsdispatcher.h"
#include "interactive/iinteractive.h"
#include "engraving/dom/score.h"
#include "context/iglobalcontext.h"
#include "notation/inotation.h"
#include "project/iprojectfilescontroller.h"
#include "audio/main/iplayback.h"
#include "playback/iplaybackcontroller.h"
#include "ui/imainwindow.h"
#include "ui/iuiconfiguration.h"
#include "ui/inavigationcontroller.h"

#include "operationtranslator.h"
#include "scoreapplicator.h"
#include "presenceoverlay.h"

namespace mu::editude::internal {

class EditudeAnnotationModel;
class EditudeAnnotationOverlayModel;
class EditudePresenceModel;

class EditudeService : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

public:
    explicit EditudeService(const muse::modularity::ContextPtr& iocCtx, QObject* parent = nullptr);

    void start();

#if defined(MUE_BUILD_EDITUDE_TEST_DRIVER) || defined(Q_OS_WASM)
    mu::engraving::Score* scoreForTest() const { return m_score; }
    int serverRevisionForTest() const { return m_serverRevision; }
    mu::notation::INotationPtr currentNotationForTest() const { return m_currentNotation; }
    const QHash<mu::engraving::Part*, QString>& translatorKnownPartUuids() const
    {
        return m_translator.knownPartUuids();
    }
    const QHash<QString, mu::engraving::Part*>& applicatorPartUuidToPart() const
    {
        return m_applicator.partUuidToPart();
    }
    void connectToSession(const QString& sessionUrl);
    QString stateForTest() const;
    void sendUndoRequest();
    bool hasUndoableOps() const { return !m_ownOpIds.isEmpty(); }
#endif
    void onNotationChanged(mu::notation::INotationPtr notation);
    void setPresenceModel(EditudePresenceModel* model);
    void setAnnotationModel(EditudeAnnotationModel* model);
    void setAnnotationOverlayModel(EditudeAnnotationOverlayModel* model);
    void setBootstrapScorePath(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    class RemoteApplyGuard;  // RAII guard for m_applyingRemote (defined in .cpp)
    enum class State { Disconnected, Authenticating, Joining, Live, Reconnecting };

    void onConnected();
    void onServerMessage(const QString& msg);
    void onScoreChanges(const mu::engraving::ScoreChanges& changes);
    void openScoreForSession();
    void bootstrapAndConnect();
    void applyPendingOps();
    void scheduleTokenRefresh();
    void onTokenRefreshTimer();
    void onDisconnected();
    void onReconnectTimer();
    void _openWebSocket();
    void onPlaybackStateChanged();
    void onSelectionChanged();
    QJsonObject buildSelectionPayload(const mu::notation::INotationSelectionPtr& sel);
    void refreshPresenceModel();
    void refreshAnnotationOverlay();
    void fetchAnnotations();
    void createAnnotation(const QString& partId, const QJsonArray& partIds,
                          qint64 startNum, qint64 startDen,
                          qint64 endNum, qint64 endDen, const QString& body);
    void resolveAnnotation(const QString& annotationId, bool resolved);
    void createReply(const QString& annotationId, const QString& body);
    void deleteAnnotation(const QString& annotationId);
    QJsonObject getSelectionAnchor();
    void uploadInitialSnapshot();
    void requestNotationFocus();
    void markScoreSaved();
    QUrl deriveServerBaseUrl() const;

    muse::ContextInject<mu::context::IGlobalContext> m_globalContext{ iocContext() };
    muse::ContextInject<mu::project::IProjectFilesController> m_projectFiles{ iocContext() };
    muse::ContextInject<muse::actions::IActionsDispatcher> m_dispatcher{ iocContext() };
    muse::ContextInject<muse::IInteractive> m_interactive{ iocContext() };
    muse::ContextInject<mu::playback::IPlaybackController> m_playbackController{ iocContext() };
    muse::ContextInject<muse::audio::IPlayback> m_audioPlayback{ iocContext() };
    muse::ContextInject<muse::ui::INavigationController> m_navigationController{ iocContext() };
    muse::GlobalInject<muse::ui::IUiConfiguration> m_uiConfiguration;
    muse::ContextInject<muse::ui::IMainWindow> m_mainWindow{ iocContext() };

    QWebSocket* m_socket = nullptr;
    QNetworkAccessManager m_nam;
    State m_state = State::Disconnected;
    QString m_token;
    QString m_sessionUrl;
    QString m_websocketUrl;
    QString m_projectId;
    QString m_bootstrapScorePath;
    int m_clientSeq = 0;
    int m_bootstrapRevision = 0;
    int m_serverRevision = 0;
    QJsonArray m_pendingOps;
    QJsonArray m_serverParts;  // part UUID map from session-bootstrap (survives snapshot compaction)
    bool m_bootstrapReady = false;
    QTimer* m_tokenRefreshTimer = nullptr;
    qint64 m_tokenExpiry = 0;
    QTimer* m_reconnectTimer = nullptr;
    int m_reconnectAttempt = 0;
    bool m_wsEverConnected = false;
    QJsonArray m_bufferedOps;

    mu::notation::INotationPtr m_currentNotation;
    mu::engraving::Score* m_score = nullptr;
    bool m_applyingRemote = false;
    bool m_playbackActive = false;
    bool m_immediateReconnect = false;
    bool m_needsInitialSnapshot = false;
    bool m_fileNewPending = false;
    bool m_fileOpenPending = false;
    bool m_reclaimNotation = false;
    bool m_wasmReadySent = false;
    bool m_wasmBootstrapOpened = false;
    QString m_bootstrapBatchId;
    OperationTranslator m_translator;
    ScoreApplicator m_applicator;
    EditudePresenceModel* m_presenceModel = nullptr;
    EditudeAnnotationModel* m_annotationModel = nullptr;
    EditudeAnnotationOverlayModel* m_annotationOverlayModel = nullptr;
    PresenceOverlay m_presenceOverlay;
    QTimer* m_presenceThrottle = nullptr;

    // Stack of op_ids generated by *this* client's forward edits, most recent
    // last.  Used by sendUndoRequest() to pop the last op and ask the server to
    // compute and broadcast the inverse.
    QStringList m_ownOpIds;

    // Last known metaTags snapshot: used to diff against score->metaTags() on
    // every onScoreChanges() call to detect setMetaTag() mutations that bypass
    // the undo system and don't appear in changedObjects.
    QMap<QString, QString> m_lastKnownMetaTags;
};
}
