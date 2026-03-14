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
#pragma once

#include <memory>

#include <QJsonArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QUuid>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWebSocket>

#include "global/modularity/ioc.h"
#include "global/async/asyncable.h"
#include "engraving/dom/score.h"
#include "notation/inotation.h"
#include "project/iprojectfilescontroller.h"
#include "playback/iplaybackcontroller.h"

#include "operationtranslator.h"
#include "scoreapplicator.h"
#include "presenceoverlay.h"

namespace mu::editude::internal {

class EditudeAnnotationModel;
class EditudePresenceModel;

class EditudeService : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

public:
    explicit EditudeService(const muse::modularity::ContextPtr& iocCtx, QObject* parent = nullptr);

    void start();

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER
    mu::engraving::Score* scoreForTest() const { return m_score; }
    int serverRevisionForTest() const { return m_serverRevision; }
    mu::notation::INotationPtr currentNotationForTest() const { return m_currentNotation; }
    const QHash<mu::engraving::EngravingObject*, QString>& applicatorElementToUuid() const
    {
        return m_applicator.elementToUuid();
    }
    const QHash<mu::engraving::EngravingObject*, QString>& translatorLocalElementToUuid() const
    {
        return m_translator.localElementToUuid();
    }
    const QHash<mu::engraving::EngravingObject*, QString>& applicatorTier3ElementToUuid() const
    {
        return m_applicator.tier3ElementToUuid();
    }
    void connectToSession(const QString& sessionUrl);
    QString stateForTest() const;
#endif
    void onNotationChanged(mu::notation::INotationPtr notation);
    void setPresenceModel(EditudePresenceModel* model);
    void setAnnotationModel(EditudeAnnotationModel* model);

private:
    enum class State { Disconnected, Authenticating, Joining, Live, Reconnecting };

    void onConnected();
    void onServerMessage(const QString& msg);
    void onScoreChanges(const mu::engraving::ScoreChanges& changes);
    void openScoreForSession();
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
    void fetchAnnotations();

    muse::ContextInject<mu::project::IProjectFilesController> m_projectFiles{ iocContext() };
    muse::ContextInject<mu::playback::IPlaybackController> m_playbackController{ iocContext() };

    QWebSocket* m_socket = nullptr;
    QNetworkAccessManager m_nam;
    State m_state = State::Disconnected;
    QString m_token;
    QString m_sessionUrl;
    QString m_websocketUrl;
    QString m_projectId;
    QString m_snapshotPath;
    int m_clientSeq = 0;
    int m_snapshotRevision = 0;
    int m_serverRevision = 0;
    QJsonArray m_pendingOps;
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
    OperationTranslator m_translator;
    ScoreApplicator m_applicator;
    EditudePresenceModel* m_presenceModel = nullptr;
    EditudeAnnotationModel* m_annotationModel = nullptr;
    PresenceOverlay m_presenceOverlay;
    QTimer* m_presenceThrottle = nullptr;

    // Last known metaTags snapshot: used to diff against score->metaTags() on
    // every onScoreChanges() call to detect setMetaTag() mutations that bypass
    // the undo system and don't appear in changedObjects.
    QMap<QString, QString> m_lastKnownMetaTags;
};
}
