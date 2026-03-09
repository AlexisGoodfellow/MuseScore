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

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QWebSocket>

#include "global/modularity/ioc.h"
#include "global/async/asyncable.h"
#include "engraving/dom/score.h"
#include "notation/inotation.h"

#include "operationtranslator.h"
#include "scoreapplicator.h"

namespace mu::editude::internal {
class EditudeService : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

public:
    explicit EditudeService(const muse::modularity::ContextPtr& iocCtx, QObject* parent = nullptr);

    void start();
    void onNotationChanged(mu::notation::INotationPtr notation);

private:
    enum class State { Disconnected, Authenticating, Joining, Live };

    void onConnected();
    void onServerMessage(const QString& msg);
    void onScoreChanges(const mu::engraving::ScoreChanges& changes);

    QWebSocket* m_socket = nullptr;
    QNetworkAccessManager m_nam;
    State m_state = State::Disconnected;
    QString m_token;
    QString m_websocketUrl;
    QString m_projectId;
    int m_clientSeq = 0;

    mu::notation::INotationPtr m_currentNotation;
    mu::engraving::Score* m_score = nullptr;
    bool m_applyingRemote = false;
    OperationTranslator m_translator;
    ScoreApplicator m_applicator;
};
}
