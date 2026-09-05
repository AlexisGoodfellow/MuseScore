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
#include "appshellconfiguration.h"

using namespace muse;
using namespace mu::appshell;

static const std::string module_name("appshell");

static const QString NOTATION_NAVIGATOR_VISIBLE_KEY("showNavigator");

void AppShellConfiguration::init()
{
}

std::string AppShellConfiguration::museScoreVersion() const
{
    return String(application()->version().toString() + u"." + application()->build()).toStdString();
}

std::string AppShellConfiguration::museScoreRevision() const
{
    return application()->revision().toStdString();
}

// [editude] Upstream moved navigator visibility off IUiConfiguration onto a
// new appShellState() service that the web appshell does not wire up. The WASM
// shell has no navigator panel, so hold the flag locally to satisfy the
// interface without pulling in that service.
bool AppShellConfiguration::isNotationNavigatorVisible() const
{
    return m_notationNavigatorVisible;
}

void AppShellConfiguration::setIsNotationNavigatorVisible(bool visible) const
{
    if (m_notationNavigatorVisible == visible) {
        return;
    }
    m_notationNavigatorVisible = visible;
    m_notationNavigatorVisibleChanged.notify();
}

muse::async::Notification AppShellConfiguration::isNotationNavigatorVisibleChanged() const
{
    return m_notationNavigatorVisibleChanged;
}
// [/editude]
