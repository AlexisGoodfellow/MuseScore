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

#include "modularity/imodulesetup.h"
#include "global/modularity/ioc.h"
#include "global/async/asyncable.h"
#include "context/iglobalcontext.h"
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

class EditudeModuleContext : public muse::modularity::IContextSetup, public muse::async::Asyncable
{
    muse::ContextInject<mu::context::IGlobalContext> m_globalContext{ iocContext() };

public:
    EditudeModuleContext(const muse::modularity::ContextPtr& ctx)
        : muse::modularity::IContextSetup(ctx) {}

    void registerExports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    std::shared_ptr<internal::EditudeService> m_service;
    std::shared_ptr<internal::EditudePresenceModel> m_presenceModel;
    std::shared_ptr<internal::EditudeAnnotationModel> m_annotationModel;
#ifdef MUE_BUILD_EDITUDE_TEST_SERVER
    std::unique_ptr<internal::EditudeTestServer> m_testServer;
#endif
};
}

#endif // MU_EDITUDE_EDITUDEMODULE_H
