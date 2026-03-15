// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared inline helpers used by both OperationTranslator (translation) and
// EditudeTestServer (serialization). Defined inline here to avoid ODR
// violations in Unity builds where both .cpp files compile into the same TU.

#include <QString>

#include "engraving/dom/articulation.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/marker.h"

namespace mu::editude::internal {

inline QString articulationNameFromSymId(mu::engraving::SymId id)
{
    using mu::engraving::SymId;
    static const QHash<SymId, QString> s_map = {
        { SymId::articStaccatoAbove,      QStringLiteral("staccato")      },
        { SymId::articAccentAbove,        QStringLiteral("accent")        },
        { SymId::articTenutoAbove,        QStringLiteral("tenuto")        },
        { SymId::articMarcatoAbove,       QStringLiteral("marcato")       },
        { SymId::articStaccatissimoAbove, QStringLiteral("staccatissimo") },
        { SymId::fermataAbove,            QStringLiteral("fermata")       },
        { SymId::ornamentTrill,           QStringLiteral("trill")         },
        { SymId::ornamentMordent,         QStringLiteral("mordent")       },
        { SymId::ornamentTurn,            QStringLiteral("turn")          },
    };
    return s_map.value(id, QStringLiteral("staccato"));
}

inline QString dynamicKindName(mu::engraving::DynamicType dt)
{
    using mu::engraving::DynamicType;
    switch (dt) {
    case DynamicType::PPP: return QStringLiteral("ppp");
    case DynamicType::PP:  return QStringLiteral("pp");
    case DynamicType::P:   return QStringLiteral("p");
    case DynamicType::MP:  return QStringLiteral("mp");
    case DynamicType::MF:  return QStringLiteral("mf");
    case DynamicType::F:   return QStringLiteral("f");
    case DynamicType::FF:  return QStringLiteral("ff");
    case DynamicType::FFF: return QStringLiteral("fff");
    case DynamicType::SFZ: return QStringLiteral("sfz");
    case DynamicType::FP:  return QStringLiteral("fp");
    case DynamicType::RF:  return QStringLiteral("rf");
    default:               return QStringLiteral("mf");
    }
}

inline QString markerKindName(mu::engraving::MarkerType mt)
{
    using mu::engraving::MarkerType;
    switch (mt) {
    case MarkerType::SEGNO:    return QStringLiteral("segno");
    case MarkerType::CODA:     return QStringLiteral("coda");
    case MarkerType::FINE:     return QStringLiteral("fine");
    case MarkerType::TOCODA:   return QStringLiteral("to_coda");
    case MarkerType::VARSEGNO: return QStringLiteral("segno_var");
    default:                   return QStringLiteral("segno");
    }
}

} // namespace mu::editude::internal
