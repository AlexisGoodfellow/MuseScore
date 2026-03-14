// SPDX-License-Identifier: GPL-3.0-only

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

#include "editudetestserver.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

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
        { "voltas",           QJsonObject() },
        { "markers",          QJsonObject() },
        { "jumps",            QJsonObject() },
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
        { "articulations", QJsonObject() },
        { "dynamics",      QJsonObject() },
        { "slurs",         QJsonObject() },
        { "hairpins",      QJsonObject() },
        { "tuplets",       QJsonObject() },
        { "lyrics",        QJsonObject() },
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
    return QString();
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
