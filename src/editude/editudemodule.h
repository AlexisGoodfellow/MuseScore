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
#ifndef MU_EDITUDE_EDITUDEMODULE_H
#define MU_EDITUDE_EDITUDEMODULE_H

#include <memory>

#include <QString>

#include "modularity/imodulesetup.h"
#include "global/modularity/ioc.h"
#include "global/async/asyncable.h"
#include "actions/iactionsdispatcher.h"
#include "actions/actionable.h"
#include "ui/iuiactionsregister.h"
#include "context/iglobalcontext.h"
#include "appshell/internal/isessionsmanager.h"
#include "appshell/internal/istartupscenario.h"
#include "qml/Editude/editudeannotationmodel.h"
#include "qml/Editude/editudepresencemodel.h"

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER
#include "testing/editudetestserver.h"
#endif

namespace mu::editude {
namespace internal { class EditudeService; }

class EditudeModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    muse::modularity::IContextSetup* newContext(const muse::modularity::ContextPtr& ctx) const override;
};

namespace internal { class EditudeUiActions; }

class EditudeModuleContext : public muse::modularity::IContextSetup, public muse::async::Asyncable, public muse::actions::Actionable
{
    muse::ContextInject<mu::context::IGlobalContext> m_globalContext{ iocContext() };
    muse::ContextInject<muse::actions::IActionsDispatcher> m_dispatcher{ iocContext() };
    muse::ContextInject<muse::ui::IUiActionsRegister> m_actionsRegister{ iocContext() };
    muse::ContextInject<mu::appshell::ISessionsManager> m_sessionsManager{ iocContext() };
    muse::ContextInject<mu::appshell::IStartupScenario> m_startupScenario{ iocContext() };

public:
    EditudeModuleContext(const muse::modularity::ContextPtr& ctx)
        : muse::modularity::IContextSetup(ctx) {}

    void registerExports() override;
    void resolveImports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    QString m_snapshotTmpPath;
    std::shared_ptr<internal::EditudeService> m_service;
    std::shared_ptr<internal::EditudeUiActions> m_uiActions;
    internal::EditudePresenceModel* m_presenceModel = nullptr;   // application-scoped singleton
    internal::EditudeAnnotationModel* m_annotationModel = nullptr; // application-scoped singleton
#ifdef MUE_BUILD_EDITUDE_TEST_SERVER
    std::unique_ptr<internal::EditudeTestServer> m_testServer;
#endif
};
}

#endif // MU_EDITUDE_EDITUDEMODULE_H
