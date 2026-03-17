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
#include "engraving/types/types.h"

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

// ---------------------------------------------------------------------------
// NoteHeadGroup ↔ string
// ---------------------------------------------------------------------------

inline QString noteheadGroupToString(mu::engraving::NoteHeadGroup g)
{
    using mu::engraving::NoteHeadGroup;
    switch (g) {
    case NoteHeadGroup::HEAD_NORMAL:         return QStringLiteral("normal");
    case NoteHeadGroup::HEAD_CROSS:          return QStringLiteral("cross");
    case NoteHeadGroup::HEAD_PLUS:           return QStringLiteral("plus");
    case NoteHeadGroup::HEAD_XCIRCLE:        return QStringLiteral("xcircle");
    case NoteHeadGroup::HEAD_WITHX:          return QStringLiteral("withx");
    case NoteHeadGroup::HEAD_TRIANGLE_UP:    return QStringLiteral("triangle_up");
    case NoteHeadGroup::HEAD_TRIANGLE_DOWN:  return QStringLiteral("triangle_down");
    case NoteHeadGroup::HEAD_SLASHED1:       return QStringLiteral("slashed1");
    case NoteHeadGroup::HEAD_SLASHED2:       return QStringLiteral("slashed2");
    case NoteHeadGroup::HEAD_DIAMOND:        return QStringLiteral("diamond");
    case NoteHeadGroup::HEAD_DIAMOND_OLD:    return QStringLiteral("diamond_old");
    case NoteHeadGroup::HEAD_CIRCLED:        return QStringLiteral("circled");
    case NoteHeadGroup::HEAD_CIRCLED_LARGE:  return QStringLiteral("circled_large");
    case NoteHeadGroup::HEAD_LARGE_ARROW:    return QStringLiteral("large_arrow");
    case NoteHeadGroup::HEAD_BREVIS_ALT:     return QStringLiteral("brevis_alt");
    case NoteHeadGroup::HEAD_SLASH:          return QStringLiteral("slash");
    case NoteHeadGroup::HEAD_LARGE_DIAMOND:  return QStringLiteral("large_diamond");
    case NoteHeadGroup::HEAD_HEAVY_CROSS:    return QStringLiteral("heavy_cross");
    case NoteHeadGroup::HEAD_HEAVY_CROSS_HAT: return QStringLiteral("heavy_cross_hat");
    default:                                 return QStringLiteral("normal");
    }
}

inline mu::engraving::NoteHeadGroup noteheadGroupFromString(const QString& s)
{
    using mu::engraving::NoteHeadGroup;
    static const QHash<QString, NoteHeadGroup> m = {
        { QStringLiteral("normal"),          NoteHeadGroup::HEAD_NORMAL         },
        { QStringLiteral("cross"),           NoteHeadGroup::HEAD_CROSS          },
        { QStringLiteral("plus"),            NoteHeadGroup::HEAD_PLUS           },
        { QStringLiteral("xcircle"),         NoteHeadGroup::HEAD_XCIRCLE        },
        { QStringLiteral("withx"),           NoteHeadGroup::HEAD_WITHX          },
        { QStringLiteral("triangle_up"),     NoteHeadGroup::HEAD_TRIANGLE_UP    },
        { QStringLiteral("triangle_down"),   NoteHeadGroup::HEAD_TRIANGLE_DOWN  },
        { QStringLiteral("slashed1"),        NoteHeadGroup::HEAD_SLASHED1       },
        { QStringLiteral("slashed2"),        NoteHeadGroup::HEAD_SLASHED2       },
        { QStringLiteral("diamond"),         NoteHeadGroup::HEAD_DIAMOND        },
        { QStringLiteral("diamond_old"),     NoteHeadGroup::HEAD_DIAMOND_OLD    },
        { QStringLiteral("circled"),         NoteHeadGroup::HEAD_CIRCLED        },
        { QStringLiteral("circled_large"),   NoteHeadGroup::HEAD_CIRCLED_LARGE  },
        { QStringLiteral("large_arrow"),     NoteHeadGroup::HEAD_LARGE_ARROW    },
        { QStringLiteral("brevis_alt"),      NoteHeadGroup::HEAD_BREVIS_ALT     },
        { QStringLiteral("slash"),           NoteHeadGroup::HEAD_SLASH          },
        { QStringLiteral("large_diamond"),   NoteHeadGroup::HEAD_LARGE_DIAMOND  },
        { QStringLiteral("heavy_cross"),     NoteHeadGroup::HEAD_HEAVY_CROSS    },
        { QStringLiteral("heavy_cross_hat"), NoteHeadGroup::HEAD_HEAVY_CROSS_HAT },
    };
    return m.value(s, NoteHeadGroup::HEAD_NORMAL);
}

// ---------------------------------------------------------------------------
// StemDirection (DirectionV) ↔ string
// ---------------------------------------------------------------------------

inline QString stemDirectionToString(mu::engraving::DirectionV d)
{
    using mu::engraving::DirectionV;
    switch (d) {
    case DirectionV::UP:   return QStringLiteral("up");
    case DirectionV::DOWN: return QStringLiteral("down");
    default:               return QStringLiteral("auto");
    }
}

inline mu::engraving::DirectionV stemDirectionFromString(const QString& s)
{
    using mu::engraving::DirectionV;
    if (s == QLatin1String("up"))   return DirectionV::UP;
    if (s == QLatin1String("down")) return DirectionV::DOWN;
    return DirectionV::AUTO;
}

// ---------------------------------------------------------------------------
// TremoloType ↔ string (for DrumInstrumentVariant)
// ---------------------------------------------------------------------------

inline QString tremoloTypeToString(mu::engraving::TremoloType t)
{
    using mu::engraving::TremoloType;
    switch (t) {
    case TremoloType::R8:         return QStringLiteral("r8");
    case TremoloType::R16:        return QStringLiteral("r16");
    case TremoloType::R32:        return QStringLiteral("r32");
    case TremoloType::R64:        return QStringLiteral("r64");
    case TremoloType::BUZZ_ROLL:  return QStringLiteral("buzz_roll");
    case TremoloType::C8:         return QStringLiteral("c8");
    case TremoloType::C16:        return QStringLiteral("c16");
    case TremoloType::C32:        return QStringLiteral("c32");
    case TremoloType::C64:        return QStringLiteral("c64");
    default:                      return QString();
    }
}

inline mu::engraving::TremoloType tremoloTypeFromString(const QString& s)
{
    using mu::engraving::TremoloType;
    if (s == QLatin1String("r8"))        return TremoloType::R8;
    if (s == QLatin1String("r16"))       return TremoloType::R16;
    if (s == QLatin1String("r32"))       return TremoloType::R32;
    if (s == QLatin1String("r64"))       return TremoloType::R64;
    if (s == QLatin1String("buzz_roll")) return TremoloType::BUZZ_ROLL;
    if (s == QLatin1String("c8"))        return TremoloType::C8;
    if (s == QLatin1String("c16"))       return TremoloType::C16;
    if (s == QLatin1String("c32"))       return TremoloType::C32;
    if (s == QLatin1String("c64"))       return TremoloType::C64;
    return TremoloType::INVALID_TREMOLO;
}

} // namespace mu::editude::internal
