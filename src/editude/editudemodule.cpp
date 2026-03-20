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
#include <QtQml/qqml.h>

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

    // Editude manages project state through its own server — suppress
    // MuseScore's "previous session quit unexpectedly" recovery dialog.
    if (m_sessionsManager()) {
        m_sessionsManager()->reset();
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
    });
}

void EditudeModuleContext::onDeinit()
{
    m_globalContext()->currentNotationChanged().disconnect(this);
    m_service.reset();
}
