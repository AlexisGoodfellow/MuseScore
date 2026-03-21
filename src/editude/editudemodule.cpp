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
#include "editudemodule.h"

#include "internal/editudeservice.h"
#include "internal/editudeuiactions.h"
#include "qml/Editude/editudeannotationmodel.h"
#include "qml/Editude/editudepresencemodel.h"
#include "log.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QTimer>
#include <QtQml/qqml.h>

#include "project/types/projecttypes.h"

using namespace mu::editude;
using namespace muse::modularity;

static const std::string mname("editude");

std::string EditudeModule::moduleName() const
{
    return mname;
}

IContextSetup* EditudeModule::newContext(const ContextPtr& ctx) const
{
    return new EditudeModuleContext(ctx);
}

void EditudeModuleContext::registerExports()
{
    m_service = std::make_shared<internal::EditudeService>(iocContext());

    m_presenceModel = internal::EditudePresenceModel::instance();
    m_annotationModel = internal::EditudeAnnotationModel::instance();

    qmlRegisterSingletonType<internal::EditudeAnnotationModel>(
        "Editude", 1, 0, "EditudeAnnotationModel",
        [](QQmlEngine*, QJSEngine*) -> QObject* {
            auto* inst = internal::EditudeAnnotationModel::instance();
            QJSEngine::setObjectOwnership(inst, QJSEngine::CppOwnership);
            return inst;
        });

    qmlRegisterSingletonType<internal::EditudePresenceModel>(
        "Editude", 1, 0, "EditudePresenceModel",
        [](QQmlEngine*, QJSEngine*) -> QObject* {
            auto* inst = internal::EditudePresenceModel::instance();
            QJSEngine::setObjectOwnership(inst, QJSEngine::CppOwnership);
            return inst;
        });

    // NOTE: session recovery suppression moved to onInit() — see comment there.

    // ── Snapshot fetch + startup scenario steering ──────────────────────
    //
    // If EDITUDE_SESSION_URL is set, fetch session metadata and the latest
    // snapshot synchronously so we can tell the startup scenario to open
    // the NOTATION page directly (no HOME page flash).

    const QString sessionUrl = QString::fromUtf8(qgetenv("EDITUDE_SESSION_URL"));
    if (sessionUrl.isEmpty()) {
        return; // standalone mode — no editude session
    }

    // 1. Sync GET /session → local session server (~0.1 ms, localhost)
    QNetworkAccessManager nam;
    {
        QNetworkReply* reply = nam.get(QNetworkRequest(QUrl(sessionUrl)));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "[EditudeModule] session fetch failed:" << reply->errorString();
            reply->deleteLater();
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();

        const QString token     = obj.value("token").toString();
        const QString projectId = obj.value("project_id").toString();
        const QString serverUrl = obj.value("server_url").toString();

        if (token.isEmpty() || projectId.isEmpty() || serverUrl.isEmpty()) {
            LOGW() << "[EditudeModule] session response missing required fields";
            return;
        }

        // 2. Sync GET /projects/{pid}/latest-snapshot → editude server
        QUrl snapshotUrl(serverUrl + "/projects/" + projectId + "/latest-snapshot");
        QNetworkRequest snapReq(snapshotUrl);
        snapReq.setRawHeader("Authorization", QString("Bearer %1").arg(token).toUtf8());

        QNetworkReply* snapReply = nam.get(snapReq);
        QEventLoop snapLoop;
        QObject::connect(snapReply, &QNetworkReply::finished, &snapLoop, &QEventLoop::quit);
        QTimer::singleShot(10000, &snapLoop, &QEventLoop::quit);
        snapLoop.exec();

        if (snapReply->error() == QNetworkReply::NoError
            && snapReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            // Write MSCZ bytes to a temp file for MuseScore's openProject().
            const QByteArray msczData = snapReply->readAll();
            QTemporaryFile tmpFile;
            tmpFile.setFileTemplate(QDir::tempPath() + "/editude_XXXXXX.mscz");
            tmpFile.setAutoRemove(false);
            if (tmpFile.open()) {
                tmpFile.write(msczData);
                tmpFile.close();
                m_snapshotTmpPath = tmpFile.fileName();
                m_service->setSnapshotPath(m_snapshotTmpPath);

                // Tell the startup scenario to open this file directly → NOTATION page.
                if (m_startupScenario()) {
                    m_startupScenario()->setStartupScoreFile(
                        mu::project::ProjectFile(muse::io::path_t(m_snapshotTmpPath)));
                }
                LOGI() << "[EditudeModule] snapshot written to" << m_snapshotTmpPath
                       << "(" << msczData.size() << "bytes)";
            } else {
                LOGW() << "[EditudeModule] failed to create temp file for snapshot";
            }
        } else {
            LOGD() << "[EditudeModule] no snapshot available (new project or 404)";
        }
        snapReply->deleteLater();
    }
}

void EditudeModuleContext::resolveImports()
{
    m_uiActions = std::make_shared<internal::EditudeUiActions>();
    if (auto ar = ioc()->resolve<muse::ui::IUiActionsRegister>(mname)) {
        ar->reg(m_uiActions);
    }
}

void EditudeModuleContext::onInit(const muse::IApplication::RunMode& mode)
{
    if (mode != muse::IApplication::RunMode::GuiApp) {
        LOGI() << "[EditudeModule] skipping — not GuiApp";
        return;
    }

    // Suppress MuseScore's "previous session quit unexpectedly" recovery dialog.
    // This clears the persisted session project list via configuration().
    // Must run in onInit() (not registerExports) because the configuration
    // system hasn't loaded persisted settings yet during registerExports().
    // Still well before the startup scenario, which runs via QueuedConnection
    // after all modules' onInit() completes.
    if (m_sessionsManager()) {
        m_sessionsManager()->reset();
    } else {
        LOGW() << "[EditudeModule] ISessionsManager not available — cannot suppress recovery dialog";
    }

    m_service->setPresenceModel(m_presenceModel);
    m_service->setAnnotationModel(m_annotationModel);
    m_service->start();

    // Wire the annotation model to the UI actions so actionChecked() works.
    m_uiActions->setAnnotationModel(m_annotationModel);

    // Register dispatch handler for the toggle-annotations toolbar action.
    m_dispatcher()->reg(this, "toggle-annotations", [this]() {
        m_annotationModel->setPanelVisible(!m_annotationModel->panelVisible());
        m_uiActions->notifyAnnotationToggleChanged();
    });

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER
    LOGI() << "[EditudeModule] test server enabled, EDITUDE_TEST_PORT=" << qgetenv("EDITUDE_TEST_PORT");
    {
        const QByteArray portEnv = qgetenv("EDITUDE_TEST_PORT");
        if (!portEnv.isEmpty()) {
            bool ok = false;
            const quint16 port = static_cast<quint16>(portEnv.toUShort(&ok));
            if (ok) {
                m_testServer = std::make_unique<internal::EditudeTestServer>(
                    m_service.get(), port);
                m_testServer->start();
            }
        }
    }
#endif

    m_globalContext()->currentNotationChanged().onNotify(this, [this]() {
        m_service->onNotationChanged(m_globalContext()->currentNotation());

        // Re-clear session recovery data.  openProject() fires
        // currentProjectChanged() which triggers the session manager's
        // update() to re-add the project path — undoing the reset() we
        // did above.  AppShell's onInit() registered update() before
        // ours, so by this point the path has already been re-added.
        // Clearing again here ensures hasProjectsForRestore() returns
        // false when the startup scenario checks moments later.
        if (m_sessionsManager()) {
            m_sessionsManager()->reset();
        }
    });
}

void EditudeModuleContext::onDeinit()
{
    m_globalContext()->currentNotationChanged().disconnect(this);
    m_service.reset();

    if (!m_snapshotTmpPath.isEmpty()) {
        QFile::remove(m_snapshotTmpPath);
    }
}
