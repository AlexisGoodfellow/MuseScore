// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared inline helpers used by both OperationTranslator (translation) and
// EditudeTestServer (serialization). Defined inline here to avoid ODR
// violations in Unity builds where both .cpp files compile into the same TU.

#include <QString>

#include "engraving/dom/articulation.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/marker.h"
#include "engraving/types/symnames.h"

namespace mu::editude::internal {

inline QString articulationNameFromSymId(mu::engraving::SymId id)
{
    using mu::engraving::SymId;
    static const QHash<SymId, QString> s_map = {
        // --- Standard articulations (above + below variants) ---
        { SymId::articStaccatoAbove,                 QStringLiteral("staccato")                    },
        { SymId::articStaccatoBelow,                 QStringLiteral("staccato")                    },
        { SymId::articAccentAbove,                   QStringLiteral("accent")                      },
        { SymId::articAccentBelow,                   QStringLiteral("accent")                      },
        { SymId::articTenutoAbove,                   QStringLiteral("tenuto")                      },
        { SymId::articTenutoBelow,                   QStringLiteral("tenuto")                      },
        { SymId::articMarcatoAbove,                  QStringLiteral("marcato")                     },
        { SymId::articMarcatoBelow,                  QStringLiteral("marcato")                     },
        { SymId::articStaccatissimoAbove,            QStringLiteral("staccatissimo")               },
        { SymId::articStaccatissimoBelow,            QStringLiteral("staccatissimo")               },
        { SymId::articStaccatissimoStrokeAbove,      QStringLiteral("staccatissimo_stroke")        },
        { SymId::articStaccatissimoStrokeBelow,      QStringLiteral("staccatissimo_stroke")        },
        { SymId::articStaccatissimoWedgeAbove,       QStringLiteral("staccatissimo_wedge")         },
        { SymId::articStaccatissimoWedgeBelow,       QStringLiteral("staccatissimo_wedge")         },
        { SymId::articTenutoStaccatoAbove,           QStringLiteral("tenuto_staccato")             },
        { SymId::articTenutoStaccatoBelow,           QStringLiteral("tenuto_staccato")             },
        { SymId::articAccentStaccatoAbove,           QStringLiteral("accent_staccato")             },
        { SymId::articAccentStaccatoBelow,           QStringLiteral("accent_staccato")             },
        { SymId::articMarcatoStaccatoAbove,          QStringLiteral("marcato_staccato")            },
        { SymId::articMarcatoStaccatoBelow,          QStringLiteral("marcato_staccato")            },
        { SymId::articMarcatoTenutoAbove,            QStringLiteral("marcato_tenuto")              },
        { SymId::articMarcatoTenutoBelow,            QStringLiteral("marcato_tenuto")              },
        { SymId::articTenutoAccentAbove,             QStringLiteral("tenuto_accent")               },
        { SymId::articTenutoAccentBelow,             QStringLiteral("tenuto_accent")               },
        { SymId::articStressAbove,                   QStringLiteral("stress")                      },
        { SymId::articStressBelow,                   QStringLiteral("stress")                      },
        { SymId::articUnstressAbove,                 QStringLiteral("unstress")                    },
        { SymId::articUnstressBelow,                 QStringLiteral("unstress")                    },
        { SymId::articSoftAccentAbove,               QStringLiteral("soft_accent")                 },
        { SymId::articSoftAccentBelow,               QStringLiteral("soft_accent")                 },
        { SymId::articSoftAccentStaccatoAbove,       QStringLiteral("soft_accent_staccato")        },
        { SymId::articSoftAccentStaccatoBelow,       QStringLiteral("soft_accent_staccato")        },
        { SymId::articSoftAccentTenutoAbove,         QStringLiteral("soft_accent_tenuto")          },
        { SymId::articSoftAccentTenutoBelow,         QStringLiteral("soft_accent_tenuto")          },
        { SymId::articSoftAccentTenutoStaccatoAbove, QStringLiteral("soft_accent_tenuto_staccato") },
        { SymId::articSoftAccentTenutoStaccatoBelow, QStringLiteral("soft_accent_tenuto_staccato") },

        // --- Fermatas ---
        { SymId::fermataAbove,          QStringLiteral("fermata")            },
        { SymId::fermataBelow,          QStringLiteral("fermata")            },
        { SymId::fermataShortAbove,     QStringLiteral("fermata_short")      },
        { SymId::fermataShortBelow,     QStringLiteral("fermata_short")      },
        { SymId::fermataLongAbove,      QStringLiteral("fermata_long")       },
        { SymId::fermataLongBelow,      QStringLiteral("fermata_long")       },
        { SymId::fermataVeryShortAbove, QStringLiteral("fermata_very_short") },
        { SymId::fermataVeryShortBelow, QStringLiteral("fermata_very_short") },
        { SymId::fermataVeryLongAbove,  QStringLiteral("fermata_very_long")  },
        { SymId::fermataVeryLongBelow,  QStringLiteral("fermata_very_long")  },
        { SymId::fermataLongHenzeAbove, QStringLiteral("fermata_long_henze") },
        { SymId::fermataLongHenzeBelow, QStringLiteral("fermata_long_henze") },
        { SymId::fermataShortHenzeAbove, QStringLiteral("fermata_short_henze") },
        { SymId::fermataShortHenzeBelow, QStringLiteral("fermata_short_henze") },

        // --- Ornaments ---
        { SymId::ornamentTrill,                        QStringLiteral("trill")                 },
        { SymId::ornamentMordent,                      QStringLiteral("mordent")               },
        { SymId::ornamentTurn,                         QStringLiteral("turn")                  },
        { SymId::ornamentTurnInverted,                 QStringLiteral("turn_inverted")         },
        { SymId::ornamentTurnSlash,                    QStringLiteral("turn_slash")            },
        { SymId::ornamentTurnUp,                       QStringLiteral("turn_up")               },
        { SymId::ornamentShortTrill,                   QStringLiteral("short_trill")           },
        { SymId::ornamentTremblement,                  QStringLiteral("tremblement")           },
        { SymId::ornamentPrallMordent,                 QStringLiteral("prall_mordent")         },
        { SymId::ornamentUpPrall,                      QStringLiteral("up_prall")              },
        { SymId::ornamentPrecompMordentUpperPrefix,    QStringLiteral("mordent_upper_prefix")  },
        { SymId::ornamentUpMordent,                    QStringLiteral("up_mordent")            },
        { SymId::ornamentDownMordent,                  QStringLiteral("down_mordent")          },
        { SymId::ornamentPrallDown,                    QStringLiteral("prall_down")            },
        { SymId::ornamentPrallUp,                      QStringLiteral("prall_up")              },
        { SymId::ornamentLinePrall,                    QStringLiteral("line_prall")            },
        { SymId::ornamentPrecompSlide,                 QStringLiteral("precomp_slide")         },
        { SymId::ornamentShake3,                       QStringLiteral("shake")                 },
        { SymId::ornamentShakeMuffat1,                 QStringLiteral("shake_muffat")          },
        { SymId::ornamentTremblementCouperin,          QStringLiteral("tremblement_couperin")  },
        { SymId::ornamentPinceCouperin,                QStringLiteral("pince_couperin")        },
        { SymId::ornamentHaydn,                        QStringLiteral("haydn")                 },

        // --- Bowing / string techniques ---
        { SymId::stringsUpBow,                QStringLiteral("up_bow")               },
        { SymId::stringsDownBow,              QStringLiteral("down_bow")             },
        { SymId::stringsHarmonic,             QStringLiteral("harmonic")             },
        { SymId::pluckedSnapPizzicatoAbove,   QStringLiteral("snap_pizzicato")       },
        { SymId::pluckedSnapPizzicatoBelow,   QStringLiteral("snap_pizzicato")       },
        { SymId::pluckedLeftHandPizzicato,    QStringLiteral("left_hand_pizzicato")  },

        // --- Brass ---
        { SymId::brassMuteOpen,   QStringLiteral("brass_mute_open")   },
        { SymId::brassMuteClosed, QStringLiteral("brass_mute_closed") },

        // --- Guitar ---
        { SymId::guitarFadeIn,      QStringLiteral("guitar_fade_in")      },
        { SymId::guitarFadeOut,     QStringLiteral("guitar_fade_out")     },
        { SymId::guitarVolumeSwell, QStringLiteral("guitar_volume_swell") },
    };
    const QString mapped = s_map.value(id);
    if (!mapped.isEmpty()) {
        return mapped;
    }
    // Fallback: use the raw SMuFL symbol name so the Python _missing_ hook
    // can accept it as an unknown articulation type rather than dropping it.
    const auto rawName = mu::engraving::SymNames::nameForSymId(id);
    return QString::fromUtf8(rawName.ascii(), int(rawName.size()));
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
