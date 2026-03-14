// SPDX-License-Identifier: GPL-3.0-only

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

#include "editudetestserver.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/undo.h"
#include "engraving/types/types.h"
#include "engraving/types/typesconv.h"

#include "engraving/dom/articulation.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/note.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/types/bps.h"

#include "global/log.h"

#include "internal/editudeservice.h"
#include "internal/scoreapplicator.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

EditudeTestServer::EditudeTestServer(EditudeService* svc, quint16 port, QObject* parent)
    : QObject(parent)
    , m_svc(svc)
    , m_port(port)
{
}

void EditudeTestServer::start()
{
    m_server = new QHttpServer(this);
    m_server->route("/health", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest&) { return handleHealth(); });
    m_server->route("/score", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest&) { return handleScore(); });
    m_server->route("/wait_revision", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) { return handleWaitRevision(req); });
    m_server->route("/action", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) { return handleAction(req); });
    m_server->route("/connect", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest& req) { return handleConnect(req); });
    m_server->route("/status", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest&) { return handleStatus(); });
    m_server->listen(QHostAddress::LocalHost, m_port);
    LOGD() << "[EditudeTestServer] listening on port" << m_port;
}

QHttpServerResponse EditudeTestServer::handleHealth()
{
    return QHttpServerResponse(QJsonDocument(QJsonObject{ { "status", "ok" } }));
}

QHttpServerResponse EditudeTestServer::handleScore()
{
    if (m_svc->scoreForTest() == nullptr) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }
    const QJsonObject obj = serializeScore();
    return QHttpServerResponse(QJsonDocument(obj));
}

QHttpServerResponse EditudeTestServer::handleWaitRevision(const QHttpServerRequest& req)
{
    const QJsonObject body = QJsonDocument::fromJson(req.body()).object();
    const int minRevision = body.value("min_revision").toInt();
    const int timeoutMs   = body.value("timeout_ms").toInt(5000);

    if (m_svc->serverRevisionForTest() >= minRevision) {
        return QHttpServerResponse(QJsonDocument(QJsonObject{
            { "revision", m_svc->serverRevisionForTest() }
        }));
    }

    QEventLoop loop;
    QTimer pollTimer;
    QTimer deadlineTimer;
    int achieved = -1;

    QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
        if (m_svc->serverRevisionForTest() >= minRevision) {
            achieved = m_svc->serverRevisionForTest();
            loop.quit();
        }
    });
    QObject::connect(&deadlineTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    pollTimer.start(20);
    deadlineTimer.start(timeoutMs);
    loop.exec();

    if (achieved >= 0) {
        return QHttpServerResponse(QJsonDocument(QJsonObject{ { "revision", achieved } }));
    }
    return QHttpServerResponse(
        QHttpServerResponse::StatusCode::RequestTimeout,
        QJsonDocument(QJsonObject{
            { "error", "timeout" },
            { "revision", m_svc->serverRevisionForTest() }
        })
    );
}

QHttpServerResponse EditudeTestServer::handleAction(const QHttpServerRequest& req)
{
    const QJsonObject body = QJsonDocument::fromJson(req.body()).object();
    const QString action   = body.value("action").toString();

    if (action == QLatin1String("insert_note"))   return actionInsertNote(body);
    if (action == QLatin1String("insert_rest"))   return actionInsertRest(body);
    if (action == QLatin1String("delete_event"))  return actionDeleteEvent(body);
    if (action == QLatin1String("set_pitch"))     return actionSetPitch(body);
    if (action == QLatin1String("undo"))          return actionUndo();
    if (action == QLatin1String("add_part"))      return actionAddPart(body);
    if (action == QLatin1String("remove_part"))   return actionRemovePart(body);
    if (action == QLatin1String("set_part_name")) return actionSetPartName(body);
    if (action == QLatin1String("set_staff_count")) return actionSetStaffCount(body);
    if (action == QLatin1String("set_part_instrument")) return actionSetPartInstrument(body);
    if (action == QLatin1String("add_articulation"))    return actionAddArticulation(body);
    if (action == QLatin1String("remove_articulation")) return actionRemoveArticulation(body);
    if (action == QLatin1String("add_dynamic"))         return actionAddDynamic(body);
    if (action == QLatin1String("set_dynamic"))         return actionSetDynamic(body);
    if (action == QLatin1String("remove_dynamic"))      return actionRemoveDynamic(body);
    if (action == QLatin1String("add_slur"))            return actionAddSlur(body);
    if (action == QLatin1String("remove_slur"))         return actionRemoveSlur(body);
    if (action == QLatin1String("add_hairpin"))         return actionAddHairpin(body);
    if (action == QLatin1String("remove_hairpin"))      return actionRemoveHairpin(body);
    if (action == QLatin1String("add_lyric"))           return actionAddLyric(body);
    if (action == QLatin1String("set_lyric"))           return actionSetLyric(body);
    if (action == QLatin1String("remove_lyric"))        return actionRemoveLyric(body);
    if (action == QLatin1String("insert_volta"))        return actionInsertVolta(body);
    if (action == QLatin1String("remove_volta"))        return actionRemoveVolta(body);
    if (action == QLatin1String("insert_marker"))       return actionInsertMarker(body);
    if (action == QLatin1String("remove_marker"))       return actionRemoveMarker(body);
    if (action == QLatin1String("insert_jump"))         return actionInsertJump(body);
    if (action == QLatin1String("remove_jump"))         return actionRemoveJump(body);
    if (action == QLatin1String("insert_beats"))        return actionInsertBeats(body);
    if (action == QLatin1String("delete_beats"))        return actionDeleteBeats(body);
    if (action == QLatin1String("set_score_metadata"))  return actionSetScoreMetadata(body);

    return errorResponse(QHttpServerResponse::StatusCode::BadRequest,
                         QString("unknown action: %1").arg(action));
}

QHttpServerResponse EditudeTestServer::actionInsertNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    const QJsonObject pitchObj = body["pitch"].toObject();
    const QJsonObject durObj   = body["duration"].toObject();
    const track_idx_t track    = static_cast<track_idx_t>(body.value("track").toInt(0));

    const int midi = ScoreApplicator::pitchToMidi(
        pitchObj["step"].toString(),
        pitchObj["octave"].toInt(),
        pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "invalid pitch");
    }

    const DurationType dt = ScoreApplicator::parseDurationType(durObj["type"].toString());
    if (dt == DurationType::V_INVALID) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "invalid duration type");
    }
    const int dots = durObj["dots"].toInt(0);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "beat not found in score");
    }

    TDuration dur(dt);
    dur.setDots(dots);
    NoteVal nval(midi);

    score->startCmd(TranslatableString("test", "insert note"));
    score->setNoteRest(seg, track, nval, dur.ticks());
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionInsertRest(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    const QJsonObject durObj  = body["duration"].toObject();
    const track_idx_t track   = static_cast<track_idx_t>(body.value("track").toInt(0));

    const DurationType dt = ScoreApplicator::parseDurationType(durObj["type"].toString());
    if (dt == DurationType::V_INVALID) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "invalid duration type");
    }
    const int dots = durObj["dots"].toInt(0);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "beat not found in score");
    }

    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("test", "insert rest"));
    score->setNoteRest(seg, track, NoteVal(), dur.ticks());
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionDeleteEvent(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    if (eventId.isEmpty()) {
        return errorResponse(QHttpServerResponse::StatusCode::BadRequest,
                             "event_id required");
    }

    EngravingObject* obj = nullptr;
    for (auto it = m_svc->translatorLocalElementToUuid().begin();
         it != m_svc->translatorLocalElementToUuid().end(); ++it) {
        if (it.value() == eventId) {
            obj = it.key();
            break;
        }
    }
    if (!obj) {
        for (auto it = m_svc->applicatorElementToUuid().begin();
             it != m_svc->applicatorElementToUuid().end(); ++it) {
            if (it.value() == eventId) {
                obj = it.key();
                break;
            }
        }
    }
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "event not found");
    }

    auto* item = dynamic_cast<EngravingItem*>(obj);
    if (!item) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not an EngravingItem");
    }

    score->startCmd(TranslatableString("test", "delete event"));
    score->deleteItem(item);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionSetPitch(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    if (eventId.isEmpty()) {
        return errorResponse(QHttpServerResponse::StatusCode::BadRequest,
                             "event_id required");
    }

    EngravingObject* obj = nullptr;
    for (auto it = m_svc->translatorLocalElementToUuid().begin();
         it != m_svc->translatorLocalElementToUuid().end(); ++it) {
        if (it.value() == eventId) {
            obj = it.key();
            break;
        }
    }
    if (!obj) {
        for (auto it = m_svc->applicatorElementToUuid().begin();
             it != m_svc->applicatorElementToUuid().end(); ++it) {
            if (it.value() == eventId) {
                obj = it.key();
                break;
            }
        }
    }
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "event not found");
    }

    Note* note = dynamic_cast<Note*>(obj);
    if (!note) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Note");
    }

    const QJsonObject pitchObj = body["pitch"].toObject();
    const int midi = ScoreApplicator::pitchToMidi(
        pitchObj["step"].toString(),
        pitchObj["octave"].toInt(),
        pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "invalid pitch");
    }

    score->startCmd(TranslatableString("test", "set pitch"));
    note->undoChangeProperty(Pid::PITCH, midi);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionUndo()
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }
    EditData ed;
    score->undoStack()->undo(&ed);
    return okResponse();
}

QHttpServerResponse EditudeTestServer::handleConnect(const QHttpServerRequest& req)
{
    const QJsonObject body = QJsonDocument::fromJson(req.body()).object();
    const QString sessionUrl = body.value("session_url").toString();
    if (sessionUrl.isEmpty()) {
        return QHttpServerResponse(
            QHttpServerResponse::StatusCode::BadRequest,
            QJsonDocument(QJsonObject{ { "error", "session_url required" } })
        );
    }
    m_svc->connectToSession(sessionUrl);
    return QHttpServerResponse(QJsonDocument(QJsonObject{ { "ok", true } }));
}

QHttpServerResponse EditudeTestServer::handleStatus()
{
    return QHttpServerResponse(QJsonDocument(QJsonObject{
        { "state",    m_svc->stateForTest() },
        { "revision", m_svc->serverRevisionForTest() },
    }));
}

// ---------------------------------------------------------------------------
// Score serialisation
// ---------------------------------------------------------------------------

static QJsonValue strOrNull(const QString& s)
{
    return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}

QJsonObject EditudeTestServer::serializeScore()
{
    Score* score = m_svc->scoreForTest();

    QJsonArray partsArr;
    for (Part* part : score->parts()) {
        partsArr.append(serializePart(part));
    }

    return QJsonObject{
        { "parts",            partsArr },
        { "metric_grid",      QJsonArray() },
        { "tempo_map",        QJsonArray() },
        { "chord_symbols",    QJsonObject() },
        { "repeat_barlines",  QJsonArray() },
        { "voltas",           serializeScoreVoltas() },
        { "markers",          serializeScoreMarkers() },
        { "jumps",            serializeScoreJumps() },
        { "title",            strOrNull(score->metaTag(u"workTitle").toQString()) },
        { "subtitle",         strOrNull(score->metaTag(u"subtitle").toQString()) },
        { "composer",         strOrNull(score->metaTag(u"composer").toQString()) },
        { "arranger",         strOrNull(score->metaTag(u"arranger").toQString()) },
        { "lyricist",         strOrNull(score->metaTag(u"lyricist").toQString()) },
        { "copyright",        strOrNull(score->metaTag(u"copyright").toQString()) },
        { "work_number",      strOrNull(score->metaTag(u"workNumber").toQString()) },
        { "movement_number",  strOrNull(score->metaTag(u"movementNumber").toQString()) },
        { "movement_title",   strOrNull(score->metaTag(u"movementTitle").toQString()) },
    };
}

QJsonObject EditudeTestServer::serializePart(Part* part)
{
    const QJsonObject instrument{
        { "musescore_id", part->instrumentId() },
        { "name",         part->longName() },
        { "short_name",   part->shortName() },
    };

    return QJsonObject{
        { "id",          QString::fromStdString(part->id().toStdString()) },
        { "instrument",  instrument },
        { "name",        QJsonValue::Null },
        { "staff_count", static_cast<int>(part->nstaves()) },
        { "events",      serializePartEvents(part) },
        { "clef_changes", QJsonArray() },
        { "key_changes",  QJsonArray() },
        { "articulations", serializePartArticulations(part) },
        { "dynamics",      serializePartDynamics(part) },
        { "slurs",         serializePartSlurs(part) },
        { "hairpins",      serializePartHairpins(part) },
        { "tuplets",       QJsonObject() },
        { "lyrics",        serializePartLyricsMap(part) },
    };
}

QJsonArray EditudeTestServer::serializePartEvents(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonArray events;

    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::ChordRest); seg;
             seg = seg->next(SegmentType::ChordRest)) {
            for (track_idx_t track = part->startTrack(); track < part->endTrack(); ++track) {
                EngravingItem* el = seg->element(track);
                if (!el) {
                    continue;
                }
                // Skip full-measure rests and generated elements.
                if (el->isRest()) {
                    Rest* r = toRest(el);
                    if (r->isFullMeasureRest() || r->generated()) {
                        continue;
                    }
                }
                const QString uuid = uuidForElement(el);
                if (uuid.isEmpty()) {
                    continue;
                }
                const Fraction tick = seg->tick();
                if (el->isNote()) {
                    events.append(serializeNote(toNote(el), uuid, tick));
                } else if (el->isRest()) {
                    events.append(serializeRest(toRest(el), uuid, tick));
                } else if (el->isChord()) {
                    events.append(serializeChord(toChord(el), uuid, tick));
                }
            }
        }
    }
    return events;
}

QJsonObject EditudeTestServer::serializeNote(Note* note, const QString& uuid,
                                              const Fraction& tick)
{
    return QJsonObject{
        { "kind",     "note" },
        { "id",       uuid },
        { "beat",     beatJson(tick) },
        { "duration", QJsonObject{
            { "type", durationTypeName(note->chord()->durationType().type()) },
            { "dots", note->chord()->dots() }
        }},
        { "pitch",    pitchJson(note) },
        { "tie",      QJsonValue::Null },
        { "track",    static_cast<int>(note->track()) },
    };
}

QJsonObject EditudeTestServer::serializeRest(Rest* rest, const QString& uuid,
                                              const Fraction& tick)
{
    return QJsonObject{
        { "kind",     "rest" },
        { "id",       uuid },
        { "beat",     beatJson(tick) },
        { "duration", QJsonObject{
            { "type", durationTypeName(rest->durationType().type()) },
            { "dots", rest->dots() }
        }},
        { "track",    static_cast<int>(rest->track()) },
    };
}

QJsonObject EditudeTestServer::serializeChord(Chord* chord, const QString& uuid,
                                               const Fraction& tick)
{
    QJsonArray pitches;
    for (Note* n : chord->notes()) {
        pitches.append(pitchJson(n));
    }
    return QJsonObject{
        { "kind",     "chord" },
        { "id",       uuid },
        { "beat",     beatJson(tick) },
        { "duration", QJsonObject{
            { "type", durationTypeName(chord->durationType().type()) },
            { "dots", chord->dots() }
        }},
        { "pitches",  pitches },
        { "tie",      QJsonValue::Null },
        { "track",    static_cast<int>(chord->track()) },
    };
}

QString EditudeTestServer::uuidForElement(EngravingObject* obj) const
{
    // Check translator (locally-inserted) map first.
    const auto& localMap = m_svc->translatorLocalElementToUuid();
    auto it = localMap.find(obj);
    if (it != localMap.end()) {
        return it.value();
    }
    // Fall back to applicator (remotely-applied) map.
    const auto& applMap = m_svc->applicatorElementToUuid();
    auto it2 = applMap.find(obj);
    if (it2 != applMap.end()) {
        return it2.value();
    }
    // Fall back to Tier 3 applicator map.
    const auto& tier3Map = m_svc->applicatorTier3ElementToUuid();
    auto it3 = tier3Map.find(obj);
    if (it3 != tier3Map.end()) {
        return it3.value();
    }
    return QString();
}

mu::engraving::EngravingObject* EditudeTestServer::findByUuid(const QString& uuid) const
{
    const auto& localMap = m_svc->translatorLocalElementToUuid();
    for (auto it = localMap.begin(); it != localMap.end(); ++it) {
        if (it.value() == uuid) {
            return it.key();
        }
    }
    const auto& applMap = m_svc->applicatorElementToUuid();
    for (auto it = applMap.begin(); it != applMap.end(); ++it) {
        if (it.value() == uuid) {
            return it.key();
        }
    }
    const auto& tier3Map = m_svc->applicatorTier3ElementToUuid();
    for (auto it = tier3Map.begin(); it != tier3Map.end(); ++it) {
        if (it.value() == uuid) {
            return it.key();
        }
    }
    return nullptr;
}

QString EditudeTestServer::uuidForChordRest(EngravingObject* obj) const
{
    const QString direct = uuidForElement(obj);
    if (!direct.isEmpty()) {
        return direct;
    }
    if (obj && obj->isChord()) {
        Chord* chord = toChord(static_cast<EngravingItem*>(obj));
        if (chord->notes().size() == 1) {
            return uuidForElement(chord->notes().front());
        }
    }
    return {};
}

QJsonObject EditudeTestServer::beatJson(const Fraction& tick)
{
    return QJsonObject{
        { "numerator",   tick.numerator() },
        { "denominator", tick.denominator() },
    };
}

QString EditudeTestServer::durationTypeName(DurationType dt)
{
    switch (dt) {
    case DurationType::V_WHOLE:   return QStringLiteral("whole");
    case DurationType::V_HALF:    return QStringLiteral("half");
    case DurationType::V_QUARTER: return QStringLiteral("quarter");
    case DurationType::V_EIGHTH:  return QStringLiteral("eighth");
    case DurationType::V_16TH:    return QStringLiteral("16th");
    case DurationType::V_32ND:    return QStringLiteral("32nd");
    case DurationType::V_64TH:    return QStringLiteral("64th");
    default:                      return QStringLiteral("quarter");
    }
}

QJsonObject EditudeTestServer::pitchJson(Note* note)
{
    // Mirror OperationTranslator::pitchJson exactly for consistency.
    static const char* const kSteps[] = { "F", "C", "G", "D", "A", "E", "B" };
    static const int kNaturalTpc[]    = { 13, 14, 15, 16, 17, 18, 19 };
    static const char* const kAccidentals[] = {
        "double-flat", "flat", nullptr, "sharp", "double-sharp"
    };

    const int tpc      = note->tpc1();
    const int octave   = note->octave();
    const int stepIndex = (tpc + 1) % 7;
    const int accOffset = (tpc - kNaturalTpc[stepIndex]) / 7; // range: -2..+2

    QJsonObject pitch;
    pitch["step"]   = kSteps[stepIndex];
    pitch["octave"] = octave;

    const int accIdx = accOffset + 2; // map -2..+2 → 0..4
    if (accIdx >= 0 && accIdx <= 4 && kAccidentals[accIdx]) {
        pitch["accidental"] = kAccidentals[accIdx];
    } else {
        pitch["accidental"] = QJsonValue::Null;
    }
    return pitch;
}

// ---------------------------------------------------------------------------
// Phase 1 — Part/Staff action handlers
// ---------------------------------------------------------------------------

QHttpServerResponse EditudeTestServer::actionAddPart(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "AddPart";
    if (!op.contains("part_id")) {
        op["part_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!op.contains("staff_count")) {
        op["staff_count"] = 1;
    }
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError, "add_part failed");
}

QHttpServerResponse EditudeTestServer::actionRemovePart(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "RemovePart";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError, "remove_part failed");
}

QHttpServerResponse EditudeTestServer::actionSetPartName(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "SetPartName";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError, "set_part_name failed");
}

QHttpServerResponse EditudeTestServer::actionSetStaffCount(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "SetStaffCount";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError, "set_staff_count failed");
}

// ---------------------------------------------------------------------------
// Tier 3 serialization helpers
// ---------------------------------------------------------------------------

static QString articulationNameFromSymId(SymId id)
{
    static const QHash<SymId, QString> s_map = {
        { SymId::articStaccatoAbove,     QStringLiteral("staccato")      },
        { SymId::articAccentAbove,       QStringLiteral("accent")        },
        { SymId::articTenutoAbove,       QStringLiteral("tenuto")        },
        { SymId::articMarcatoAbove,      QStringLiteral("marcato")       },
        { SymId::articStaccatissimoAbove,QStringLiteral("staccatissimo") },
        { SymId::fermataAbove,           QStringLiteral("fermata")       },
        { SymId::ornamentTrill,          QStringLiteral("trill")         },
        { SymId::ornamentMordent,        QStringLiteral("mordent")       },
        { SymId::ornamentTurn,           QStringLiteral("turn")          },
    };
    return s_map.value(id, QStringLiteral("staccato"));
}

static QString dynamicKindName(DynamicType dt)
{
    static const QHash<DynamicType, QString> s_map = {
        { DynamicType::PPP, QStringLiteral("ppp") },
        { DynamicType::PP,  QStringLiteral("pp")  },
        { DynamicType::P,   QStringLiteral("p")   },
        { DynamicType::MP,  QStringLiteral("mp")  },
        { DynamicType::MF,  QStringLiteral("mf")  },
        { DynamicType::F,   QStringLiteral("f")   },
        { DynamicType::FF,  QStringLiteral("ff")  },
        { DynamicType::FFF, QStringLiteral("fff") },
        { DynamicType::SFZ, QStringLiteral("sfz") },
        { DynamicType::FP,  QStringLiteral("fp")  },
        { DynamicType::RF,  QStringLiteral("rf")  },
    };
    return s_map.value(dt, QStringLiteral("mf"));
}

static QString markerKindName(MarkerType mt)
{
    static const QHash<MarkerType, QString> s_map = {
        { MarkerType::SEGNO,    QStringLiteral("segno")     },
        { MarkerType::CODA,     QStringLiteral("coda")      },
        { MarkerType::FINE,     QStringLiteral("fine")      },
        { MarkerType::TOCODA,   QStringLiteral("to_coda")   },
        { MarkerType::VARSEGNO, QStringLiteral("segno_var") },
    };
    return s_map.value(mt, QStringLiteral("segno"));
}

QJsonObject EditudeTestServer::serializePartArticulations(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::ChordRest); seg;
             seg = seg->next(SegmentType::ChordRest)) {
            for (track_idx_t track = part->startTrack(); track < part->endTrack(); ++track) {
                EngravingItem* el = seg->element(track);
                if (!el || !el->isChord()) {
                    continue;
                }
                Chord* chord = toChord(el);
                for (Articulation* art : chord->articulations()) {
                    const QString artUuid = uuidForElement(art);
                    if (artUuid.isEmpty()) {
                        continue;
                    }
                    const QString eventUuid = uuidForChordRest(chord);
                    result[artUuid] = QJsonObject{
                        { "id",           artUuid },
                        { "event_id",     eventUuid },
                        { "articulation", articulationNameFromSymId(art->symId()) },
                    };
                }
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartDynamics(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || !el->isDynamic()) {
                    continue;
                }
                if (el->track() < part->startTrack() || el->track() >= part->endTrack()) {
                    continue;
                }
                Dynamic* dyn = toDynamic(el);
                const QString uuid = uuidForElement(dyn);
                if (uuid.isEmpty()) {
                    continue;
                }
                result[uuid] = QJsonObject{
                    { "id",   uuid },
                    { "beat", beatJson(dyn->tick()) },
                    { "kind", dynamicKindName(dyn->dynamicType()) },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartSlurs(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isSlur()) {
            continue;
        }
        if (sp->track() < part->startTrack() || sp->track() >= part->endTrack()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        ChordRest* startCR = dynamic_cast<ChordRest*>(sp->startElement());
        ChordRest* endCR   = dynamic_cast<ChordRest*>(sp->endElement());
        const QString startUuid = startCR ? uuidForChordRest(startCR) : QString();
        const QString endUuid   = endCR   ? uuidForChordRest(endCR)   : QString();
        result[uuid] = QJsonObject{
            { "id",             uuid      },
            { "start_event_id", startUuid },
            { "end_event_id",   endUuid   },
        };
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartHairpins(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isHairpin()) {
            continue;
        }
        if (sp->track() < part->startTrack() || sp->track() >= part->endTrack()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        Hairpin* hp = toHairpin(sp);
        const QString kind = hp->isCrescendo()
            ? QStringLiteral("crescendo")
            : QStringLiteral("decrescendo");
        result[uuid] = QJsonObject{
            { "id",         uuid },
            { "start_beat", beatJson(hp->tick()) },
            { "end_beat",   beatJson(hp->tick2()) },
            { "kind",       kind },
        };
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartLyricsMap(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::ChordRest); seg;
             seg = seg->next(SegmentType::ChordRest)) {
            for (track_idx_t track = part->startTrack(); track < part->endTrack(); ++track) {
                EngravingItem* el = seg->element(track);
                if (!el || !el->isChordRest()) {
                    continue;
                }
                ChordRest* cr = toChordRest(el);
                for (Lyrics* lyr : cr->lyrics()) {
                    const QString uuid = uuidForElement(lyr);
                    if (uuid.isEmpty()) {
                        continue;
                    }
                    const QString eventUuid = uuidForChordRest(cr);
                    static const QHash<LyricsSyllabic, QString> s_syl = {
                        { LyricsSyllabic::SINGLE, QStringLiteral("single") },
                        { LyricsSyllabic::BEGIN,  QStringLiteral("begin")  },
                        { LyricsSyllabic::MIDDLE, QStringLiteral("middle") },
                        { LyricsSyllabic::END,    QStringLiteral("end")    },
                    };
                    const QString syllabic = s_syl.value(lyr->syllabic(), QStringLiteral("single"));
                    result[uuid] = QJsonObject{
                        { "id",       uuid },
                        { "event_id", eventUuid },
                        { "verse",    lyr->verse() },
                        { "syllabic", syllabic },
                        { "text",     lyr->plainText().toQString() },
                    };
                }
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tier 4 serialization helpers
// ---------------------------------------------------------------------------

QJsonObject EditudeTestServer::serializeScoreVoltas()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isVolta()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        Volta* volta = toVolta(sp);
        QJsonArray numbers;
        for (int n : volta->endings()) {
            numbers.append(n);
        }
        result[uuid] = QJsonObject{
            { "id",         uuid },
            { "start_beat", beatJson(volta->tick()) },
            { "end_beat",   beatJson(volta->tick2()) },
            { "numbers",    numbers },
            { "open_end",   volta->voltaType() == Volta::Type::OPEN },
        };
    }
    return result;
}

QJsonObject EditudeTestServer::serializeScoreMarkers()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (EngravingItem* el : m->el()) {
            if (!el || !el->isMarker()) {
                continue;
            }
            Marker* marker = toMarker(el);
            const QString uuid = uuidForElement(marker);
            if (uuid.isEmpty()) {
                continue;
            }
            const QString label = marker->label().toQString();
            QJsonObject obj{
                { "id",   uuid },
                { "beat", beatJson(marker->tick()) },
                { "kind", markerKindName(marker->markerType()) },
                { "label", label.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(label) },
            };
            result[uuid] = obj;
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializeScoreJumps()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (EngravingItem* el : m->el()) {
            if (!el || !el->isJump()) {
                continue;
            }
            Jump* jump = toJump(el);
            const QString uuid = uuidForElement(jump);
            if (uuid.isEmpty()) {
                continue;
            }
            auto toJsonOrNull = [](const String& s) -> QJsonValue {
                return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.toQString());
            };
            result[uuid] = QJsonObject{
                { "id",          uuid },
                { "beat",        beatJson(jump->tick()) },
                { "jump_to",     jump->jumpTo().toQString() },
                { "play_until",  toJsonOrNull(jump->playUntil()) },
                { "continue_at", toJsonOrNull(jump->continueAt()) },
                { "text",        toJsonOrNull(jump->plainText()) },
            };
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tier 3 action handlers
// ---------------------------------------------------------------------------

QHttpServerResponse EditudeTestServer::actionSetPartInstrument(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }
    QJsonObject op = body;
    op["type"] = "SetPartInstrument";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "set_part_instrument failed");
}

QHttpServerResponse EditudeTestServer::actionAddArticulation(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    const QString artName = body["articulation"].toString();

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "event not found");
    }

    ChordRest* cr = nullptr;
    if (obj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(obj));
    }
    if (!cr) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "event is not a note or chordrest");
    }

    static const QHash<QString, SymId> s_artMap = {
        { "staccato",      SymId::articStaccatoAbove      },
        { "accent",        SymId::articAccentAbove         },
        { "tenuto",        SymId::articTenutoAbove         },
        { "marcato",       SymId::articMarcatoAbove        },
        { "staccatissimo", SymId::articStaccatissimoAbove  },
        { "fermata",       SymId::fermataAbove             },
    };
    const SymId symId = s_artMap.value(artName, SymId::noSym);
    if (symId == SymId::noSym) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "unknown articulation type");
    }

    score->startCmd(TranslatableString("test", "add articulation"));
    Articulation* art = Factory::createArticulation(score->dummy()->chord());
    art->setSymId(symId);
    art->setParent(cr);
    art->setTrack(cr->track());
    score->undoAddElement(art);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionRemoveArticulation(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "articulation not found");
    }

    Articulation* art = dynamic_cast<Articulation*>(obj);
    if (!art) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not an Articulation");
    }

    score->startCmd(TranslatableString("test", "remove articulation"));
    score->undoRemoveElement(art);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionAddDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString kind = body["kind"].toString();

    static const QHash<QString, DynamicType> s_dynMap = {
        { "ppp", DynamicType::PPP }, { "pp",  DynamicType::PP  },
        { "p",   DynamicType::P   }, { "mp",  DynamicType::MP  },
        { "mf",  DynamicType::MF  }, { "f",   DynamicType::F   },
        { "ff",  DynamicType::FF  }, { "fff", DynamicType::FFF },
        { "sfz", DynamicType::SFZ }, { "fp",  DynamicType::FP  },
    };
    const DynamicType dt = s_dynMap.value(kind, DynamicType::OTHER);
    if (dt == DynamicType::OTHER) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "unknown dynamic kind");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "beat not found in score");
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);
    score->startCmd(TranslatableString("test", "add dynamic"));
    Dynamic* dyn = Factory::createDynamic(seg);
    dyn->setParent(seg);
    dyn->setTrack(part->startTrack());
    dyn->setDynamicType(dt);
    score->undoAddElement(dyn);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionSetDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "dynamic not found");
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(obj);
    if (!dyn) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Dynamic");
    }

    static const QHash<QString, DynamicType> s_dynMap = {
        { "ppp", DynamicType::PPP }, { "pp",  DynamicType::PP  },
        { "p",   DynamicType::P   }, { "mp",  DynamicType::MP  },
        { "mf",  DynamicType::MF  }, { "f",   DynamicType::F   },
        { "ff",  DynamicType::FF  }, { "fff", DynamicType::FFF },
    };
    const QString kind = body["kind"].toString();
    const DynamicType dt = s_dynMap.value(kind, DynamicType::OTHER);
    if (dt == DynamicType::OTHER) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "unknown dynamic kind");
    }

    score->startCmd(TranslatableString("test", "set dynamic"));
    dyn->undoChangeProperty(Pid::DYNAMIC_TYPE, PropertyValue(dt));
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionRemoveDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "dynamic not found");
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(obj);
    if (!dyn) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Dynamic");
    }

    score->startCmd(TranslatableString("test", "remove dynamic"));
    score->undoRemoveElement(dyn);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionAddSlur(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString startId = body["start_event_id"].toString();
    const QString endId   = body["end_event_id"].toString();

    EngravingObject* startObj = findByUuid(startId);
    EngravingObject* endObj   = findByUuid(endId);
    if (!startObj || !endObj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "event not found");
    }

    auto toCR = [](EngravingObject* o) -> ChordRest* {
        if (o->isNote()) return toNote(static_cast<EngravingItem*>(o))->chord();
        if (o->isChordRest()) return toChordRest(static_cast<EngravingItem*>(o));
        return nullptr;
    };
    ChordRest* startCR = toCR(startObj);
    ChordRest* endCR   = toCR(endObj);
    if (!startCR || !endCR) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "event is not a note or chordrest");
    }

    score->startCmd(TranslatableString("test", "add slur"));
    Slur* slur = Factory::createSlur(score->dummy());
    slur->setScore(score);
    slur->setTick(startCR->tick());
    slur->setTick2(endCR->tick());
    slur->setTrack(startCR->track());
    slur->setTrack2(endCR->track());
    slur->setStartElement(startCR);
    slur->setEndElement(endCR);
    score->undoAddElement(slur);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionRemoveSlur(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "slur not found");
    }

    Slur* slur = dynamic_cast<Slur*>(obj);
    if (!slur) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Slur");
    }

    score->startCmd(TranslatableString("test", "remove slur"));
    score->undoRemoveElement(slur);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionAddHairpin(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject sb = body["start_beat"].toObject();
    const QJsonObject eb = body["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());

    const bool isCrescendo = (body["kind"].toString() == QStringLiteral("crescendo"));
    const HairpinType hpType = isCrescendo ? HairpinType::CRESC_HAIRPIN : HairpinType::DIM_HAIRPIN;

    score->startCmd(TranslatableString("test", "add hairpin"));
    score->addHairpin(hpType, startTick, endTick, part->startTrack());
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionRemoveHairpin(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "hairpin not found");
    }

    Hairpin* hp = dynamic_cast<Hairpin*>(obj);
    if (!hp) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Hairpin");
    }

    score->startCmd(TranslatableString("test", "remove hairpin"));
    score->undoRemoveElement(hp);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionAddLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "event not found");
    }

    ChordRest* cr = nullptr;
    if (obj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(obj));
    }
    if (!cr) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "event is not a note or chordrest");
    }

    const int verse      = body.value("verse").toInt(0);
    const QString text   = body["text"].toString();
    const QString sylStr = body.value("syllabic").toString(QStringLiteral("single"));

    static const QHash<QString, LyricsSyllabic> s_syl = {
        { "single", LyricsSyllabic::SINGLE },
        { "begin",  LyricsSyllabic::BEGIN  },
        { "middle", LyricsSyllabic::MIDDLE },
        { "end",    LyricsSyllabic::END    },
    };
    const LyricsSyllabic syllabic = s_syl.value(sylStr, LyricsSyllabic::SINGLE);

    score->startCmd(TranslatableString("test", "add lyric"));
    Lyrics* lyric = Factory::createLyrics(cr);
    lyric->setTrack(cr->track());
    lyric->setParent(cr);
    lyric->setVerse(verse);
    lyric->setSyllabic(syllabic);
    lyric->setPlainText(String(text));
    score->undoAddElement(lyric);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionSetLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "lyric not found");
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(obj);
    if (!lyric) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not Lyrics");
    }

    const QString text = body["text"].toString();
    score->startCmd(TranslatableString("test", "set lyric"));
    lyric->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionRemoveLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "lyric not found");
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(obj);
    if (!lyric) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not Lyrics");
    }

    score->startCmd(TranslatableString("test", "remove lyric"));
    score->undoRemoveElement(lyric);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Tier 4 action handlers
// ---------------------------------------------------------------------------

QHttpServerResponse EditudeTestServer::actionInsertVolta(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "InsertVolta";
    if (!op.contains("id")) {
        op["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "insert_volta failed");
}

QHttpServerResponse EditudeTestServer::actionRemoveVolta(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "volta not found");
    }

    Volta* volta = dynamic_cast<Volta*>(obj);
    if (!volta) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Volta");
    }

    score->startCmd(TranslatableString("test", "remove volta"));
    score->undoRemoveElement(volta);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionInsertMarker(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "InsertMarker";
    if (!op.contains("id")) {
        op["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "insert_marker failed");
}

QHttpServerResponse EditudeTestServer::actionRemoveMarker(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "marker not found");
    }

    Marker* marker = dynamic_cast<Marker*>(obj);
    if (!marker) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Marker");
    }

    score->startCmd(TranslatableString("test", "remove marker"));
    score->undoRemoveElement(marker);
    score->endCmd();

    return okResponse();
}

QHttpServerResponse EditudeTestServer::actionInsertJump(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "InsertJump";
    if (!op.contains("id")) {
        op["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "insert_jump failed");
}

QHttpServerResponse EditudeTestServer::actionRemoveJump(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(QHttpServerResponse::StatusCode::NotFound, "jump not found");
    }

    Jump* jump = dynamic_cast<Jump*>(obj);
    if (!jump) {
        return errorResponse(QHttpServerResponse::StatusCode::UnprocessableEntity,
                             "element is not a Jump");
    }

    score->startCmd(TranslatableString("test", "remove jump"));
    score->undoRemoveElement(jump);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Structural + metadata action handlers
// ---------------------------------------------------------------------------

QHttpServerResponse EditudeTestServer::actionInsertBeats(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "InsertBeats";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "insert_beats failed");
}

QHttpServerResponse EditudeTestServer::actionDeleteBeats(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "DeleteBeats";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "delete_beats failed");
}

QHttpServerResponse EditudeTestServer::actionSetScoreMetadata(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(QHttpServerResponse::StatusCode::ServiceUnavailable,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "SetScoreMetadata";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(QHttpServerResponse::StatusCode::InternalServerError,
                        "set_score_metadata failed");
}

// ---------------------------------------------------------------------------
// Helper responses
// ---------------------------------------------------------------------------

QHttpServerResponse EditudeTestServer::errorResponse(QHttpServerResponse::StatusCode code,
                                                       const QString& msg)
{
    return QHttpServerResponse(
        code,
        QJsonDocument(QJsonObject{ { "error", msg } })
    );
}

QHttpServerResponse EditudeTestServer::okResponse()
{
    return QHttpServerResponse(QJsonDocument(QJsonObject{ { "ok", true } }));
}

} // namespace mu::editude::internal

#endif // MUE_BUILD_EDITUDE_TEST_SERVER
