/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "appshellmodule.h"

#include <qqml.h>

#include "modularity/ioc.h"

#include "ui/iuiactionsregister.h"
#include "interactive/iinteractiveuriregister.h"

#include "internal/applicationuiactions.h"
#include "internal/applicationactioncontroller.h"
#include "internal/appshellconfiguration.h"
#include "internal/applicationactioncontroller.h"
#include "internal/startupscenario.h"

#include "view/notationpagemodel.h"
#include "view/notationstatusbarmodel.h"
#include "view/navigableappmenumodel.h"

using namespace mu::appshell;
using namespace muse;
using namespace muse::modularity;
using namespace muse::dock;

static void appshell_init_qrc()
{
    Q_INIT_RESOURCE(appshell);
}

std::string AppShellModule::moduleName() const
{
    return "appshell";
}

void AppShellModule::registerExports()
{
    m_appShellConfiguration = std::make_shared<AppShellConfiguration>(globalCtx());
    globalIoc()->registerExport<IAppShellConfiguration>(moduleName(), m_appShellConfiguration);
}

// [editude] Contextual services moved here; see the header.
muse::modularity::IContextSetup* AppShellModule::newContext(const muse::modularity::ContextPtr& ctx) const
{
    return new AppShellContext(ctx);
}

void AppShellContext::registerExports()
{
    m_applicationActionController = std::make_shared<ApplicationActionController>(iocContext());
    m_applicationUiActions = std::make_shared<ApplicationUiActions>(m_applicationActionController, iocContext());
    ioc()->registerExport<IStartupScenario>("appshell", new StartupScenario(iocContext()));
}

void AppShellContext::resolveImports()
{
    auto ar = ioc()->resolve<ui::IUiActionsRegister>("appshell");
    if (ar) {
        ar->reg(m_applicationUiActions);
    }
}

void AppShellContext::onPreInit(const muse::IApplication::RunMode& mode)
{
    if (mode == IApplication::RunMode::AudioPluginRegistration) {
        return;
    }

    m_applicationActionController->preInit();
}

void AppShellContext::onInit(const muse::IApplication::RunMode& mode)
{
    if (mode == IApplication::RunMode::AudioPluginRegistration) {
        return;
    }

    m_applicationActionController->init();
    m_applicationUiActions->init();
}
// [/editude]

void AppShellModule::resolveImports()
{
    auto ir = globalIoc()->resolve<interactive::IInteractiveUriRegister>(moduleName());
    if (ir) {
        ir->registerPageUri(Uri("musescore://notation"));
        ir->registerPageUri(Uri("musescore://devtools"));
    }
}

void AppShellModule::registerResources()
{
    appshell_init_qrc();
}

void AppShellModule::registerUiTypes()
{
    qmlRegisterType<NavigableAppMenuModel>("MuseScore.AppShell", 1, 0, "AppMenuModel");
    qmlRegisterType<NotationPageModel>("MuseScore.AppShell", 1, 0, "NotationPageModel");
    qmlRegisterType<NotationStatusBarModel>("MuseScore.AppShell", 1, 0, "NotationStatusBarModel");
}

void AppShellModule::onPreInit(const IApplication::RunMode& mode)
{
    UNUSED(mode);
}

void AppShellModule::onInit(const IApplication::RunMode& mode)
{
    if (mode == IApplication::RunMode::AudioPluginRegistration) {
        return;
    }

    m_appShellConfiguration->init();
}

void AppShellModule::onAllInited(const IApplication::RunMode& mode)
{
    if (mode == IApplication::RunMode::AudioPluginRegistration) {
        return;
    }
}

void AppShellModule::onDeinit()
{
}
