// SPDX-License-Identifier: GPL-3.0-only

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

#include "editudetestserver.h"

#include <QCoreApplication>
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
#include "engraving/editing/undo.h"
#include "engraving/types/types.h"
#include "engraving/types/typesconv.h"

#include "engraving/dom/arpeggio.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/stringdata.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/note.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/accidental.h"
#include "engraving/dom/glissando.h"
#include "engraving/dom/ornament.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/tremolosinglechord.h"
#include "engraving/dom/tremolotwochord.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/volta.h"
#include "engraving/types/bps.h"
#include "engraving/types/symnames.h"

#include "global/log.h"

#include "internal/editudeservice.h"
#include "internal/editudeutils.h"
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
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &EditudeTestServer::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, m_port)) {
        LOGW() << "[EditudeTestServer] failed to listen on port" << m_port;
    } else {
        LOGD() << "[EditudeTestServer] listening on port" << m_port;
    }
}

void EditudeTestServer::onNewConnection()
{
    QTcpSocket* socket = m_server->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_buffers.remove(socket);
        socket->deleteLater();
    });
}

void EditudeTestServer::onReadyRead(QTcpSocket* socket)
{
    m_buffers[socket] += socket->readAll();
    const QByteArray& buf = m_buffers[socket];

    // Wait until the full HTTP header section has arrived.
    const int sep = buf.indexOf("\r\n\r\n");
    if (sep < 0)
        return;

    // Parse request line: "METHOD /path HTTP/1.1"
    const int lineEnd = buf.indexOf("\r\n");
    const QList<QByteArray> reqParts = buf.left(lineEnd).split(' ');
    if (reqParts.size() < 2) { socket->disconnectFromHost(); return; }
    const QString method = QString::fromLatin1(reqParts[0]);
    const QString path   = QString::fromLatin1(reqParts[1]).section('?', 0, 0);

    // Extract Content-Length from headers.
    int contentLength = 0;
    for (const QByteArray& line : buf.mid(lineEnd + 2, sep - lineEnd - 2).split('\n')) {
        if (line.trimmed().toLower().startsWith("content-length:"))
            contentLength = line.trimmed().mid(15).trimmed().toInt();
    }

    // Wait until the full body has arrived.
    if (buf.size() - (sep + 4) < contentLength)
        return;

    const QJsonObject bodyObj = QJsonDocument::fromJson(buf.mid(sep + 4, contentLength)).object();

    // Dispatch to the appropriate handler.
    Reply reply;
    if      (method == "GET"  && path == "/health")        reply = handleHealth();
    else if (method == "GET"  && path == "/score")         reply = handleScore();
    else if (method == "GET"  && path == "/status")        reply = handleStatus();
    else if (method == "POST" && path == "/wait_revision") reply = handleWaitRevision(bodyObj);
    else if (method == "POST" && path == "/action")        reply = handleAction(bodyObj);
    else if (method == "POST" && path == "/connect")       reply = handleConnect(bodyObj);
    else                                                   reply = errorResponse(404, "not found");

    // Write the HTTP response and close the connection.
    const QByteArray statusText = reply.status == 200 ? "OK"
                                : reply.status == 408 ? "Request Timeout"
                                : "Error";
    socket->write("HTTP/1.1 " + QByteArray::number(reply.status) + " " + statusText + "\r\n");
    socket->write("Content-Type: application/json\r\n");
    socket->write("Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n");
    socket->write("Connection: close\r\n");
    socket->write("\r\n");
    socket->write(reply.body);
    socket->flush();
    socket->disconnectFromHost();
    m_buffers.remove(socket);
}

EditudeTestServer::Reply EditudeTestServer::handleHealth()
{
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::handleScore()
{
    // Flush any pending deferred applies (QTimer::singleShot(0) from the
    // op/op_batch handlers) so the score DOM is consistent with
    // m_serverRevision before we serialize.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (m_svc->scoreForTest() == nullptr)
        return errorResponse(503, "score not ready");
    return { 200, QJsonDocument(serializeScore()).toJson(QJsonDocument::Compact) };
}

EditudeTestServer::Reply EditudeTestServer::handleWaitRevision(const QJsonObject& body)
{
    const int minRevision = body.value("min_revision").toInt();
    const int timeoutMs   = body.value("timeout_ms").toInt(5000);

    // Flush any pending deferred applies before the fast-path check.
    // The op/op_batch handlers update m_serverRevision immediately but defer
    // the actual score mutation via QTimer::singleShot(0).  Without this
    // flush, the fast path below can return "revision met" while the score
    // DOM hasn't been mutated yet, causing a subsequent /score to see stale
    // state.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (m_svc->serverRevisionForTest() >= minRevision) {
        return { 200, QJsonDocument(QJsonObject{
            { "revision", m_svc->serverRevisionForTest() }
        }).toJson(QJsonDocument::Compact) };
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

    if (achieved >= 0)
        return { 200, QJsonDocument(QJsonObject{ { "revision", achieved } }).toJson(QJsonDocument::Compact) };
    return { 408, QJsonDocument(QJsonObject{
        { "error", "timeout" },
        { "revision", m_svc->serverRevisionForTest() }
    }).toJson(QJsonDocument::Compact) };
}

EditudeTestServer::Reply EditudeTestServer::dispatchAction(const QJsonObject& body)
{
    const QString action = body.value("action").toString();
    LOGD() << "[editude-test] dispatchAction:" << action
           << "state=" << m_svc->stateForTest()
           << "rev=" << m_svc->serverRevisionForTest();

    if (action == QLatin1String("insert_note"))   return actionInsertNote(body);
    if (action == QLatin1String("insert_rest"))   return actionInsertRest(body);
    if (action == QLatin1String("insert_chord"))  return actionInsertChord(body);
    if (action == QLatin1String("delete_event"))  return actionDeleteEvent(body);
    if (action == QLatin1String("set_pitch"))     return actionSetPitch(body);
    if (action == QLatin1String("add_chord_note"))    return actionAddChordNote(body);
    if (action == QLatin1String("remove_chord_note")) return actionRemoveChordNote(body);
    if (action == QLatin1String("set_tie"))       return actionSetTie(body);
    if (action == QLatin1String("set_track"))     return actionSetTrack(body);
    if (action == QLatin1String("undo"))          return actionUndo();
    if (action == QLatin1String("set_time_signature")) return actionSetTimeSignature(body);
    if (action == QLatin1String("set_tempo"))          return actionSetTempo(body);
    if (action == QLatin1String("set_key_signature"))  return actionSetKeySignature(body);
    if (action == QLatin1String("set_clef"))           return actionSetClef(body);
    if (action == QLatin1String("add_part"))      return actionAddPart(body);
    if (action == QLatin1String("remove_part"))   return actionRemovePart(body);
    if (action == QLatin1String("set_part_name")) return actionSetPartName(body);
    if (action == QLatin1String("set_staff_count")) return actionSetStaffCount(body);
    if (action == QLatin1String("set_part_instrument")) return actionSetPartInstrument(body);
    if (action == QLatin1String("set_string_data"))    return actionSetStringData(body);
    if (action == QLatin1String("set_capo"))            return actionSetCapo(body);
    if (action == QLatin1String("set_tab_note"))       return actionSetTabNote(body);
    if (action == QLatin1String("set_drumset"))        return actionSetDrumset(body);
    if (action == QLatin1String("set_note_head"))      return actionSetNoteHead(body);
    if (action == QLatin1String("add_chord_symbol"))    return actionAddChordSymbol(body);
    if (action == QLatin1String("set_chord_symbol"))    return actionSetChordSymbol(body);
    if (action == QLatin1String("remove_chord_symbol")) return actionRemoveChordSymbol(body);
    if (action == QLatin1String("add_articulation"))    return actionAddArticulation(body);
    if (action == QLatin1String("remove_articulation")) return actionRemoveArticulation(body);
    if (action == QLatin1String("add_dynamic"))         return actionAddDynamic(body);
    if (action == QLatin1String("set_dynamic"))         return actionSetDynamic(body);
    if (action == QLatin1String("remove_dynamic"))      return actionRemoveDynamic(body);
    if (action == QLatin1String("add_slur"))            return actionAddSlur(body);
    if (action == QLatin1String("remove_slur"))         return actionRemoveSlur(body);
    if (action == QLatin1String("add_hairpin"))         return actionAddHairpin(body);
    if (action == QLatin1String("remove_hairpin"))      return actionRemoveHairpin(body);
    if (action == QLatin1String("add_octave_line"))      return actionAddOctaveLine(body);
    if (action == QLatin1String("remove_octave_line"))   return actionRemoveOctaveLine(body);
    if (action == QLatin1String("add_glissando"))        return actionAddGlissando(body);
    if (action == QLatin1String("remove_glissando"))     return actionRemoveGlissando(body);
    if (action == QLatin1String("add_pedal_line"))       return actionAddPedalLine(body);
    if (action == QLatin1String("remove_pedal_line"))    return actionRemovePedalLine(body);
    if (action == QLatin1String("add_trill_line"))       return actionAddTrillLine(body);
    if (action == QLatin1String("remove_trill_line"))    return actionRemoveTrillLine(body);
    if (action == QLatin1String("add_arpeggio"))         return actionAddArpeggio(body);
    if (action == QLatin1String("remove_arpeggio"))      return actionRemoveArpeggio(body);
    if (action == QLatin1String("add_tuplet"))            return actionAddTuplet(body);
    if (action == QLatin1String("remove_tuplet"))         return actionRemoveTuplet(body);
    if (action == QLatin1String("add_grace_note"))       return actionAddGraceNote(body);
    if (action == QLatin1String("remove_grace_note"))    return actionRemoveGraceNote(body);
    if (action == QLatin1String("add_breath_mark"))      return actionAddBreathMark(body);
    if (action == QLatin1String("remove_breath_mark"))   return actionRemoveBreathMark(body);
    if (action == QLatin1String("add_tremolo"))          return actionAddTremolo(body);
    if (action == QLatin1String("remove_tremolo"))       return actionRemoveTremolo(body);
    if (action == QLatin1String("add_two_note_tremolo"))  return actionAddTwoNoteTremolo(body);
    if (action == QLatin1String("remove_two_note_tremolo")) return actionRemoveTwoNoteTremolo(body);
    if (action == QLatin1String("add_lyric"))             return actionAddLyric(body);
    if (action == QLatin1String("set_lyric"))             return actionSetLyric(body);
    if (action == QLatin1String("remove_lyric"))          return actionRemoveLyric(body);
    if (action == QLatin1String("add_staff_text"))        return actionAddStaffText(body);
    if (action == QLatin1String("set_staff_text"))        return actionSetStaffText(body);
    if (action == QLatin1String("remove_staff_text"))     return actionRemoveStaffText(body);
    if (action == QLatin1String("add_system_text"))       return actionAddSystemText(body);
    if (action == QLatin1String("set_system_text"))       return actionSetSystemText(body);
    if (action == QLatin1String("remove_system_text"))    return actionRemoveSystemText(body);
    if (action == QLatin1String("add_rehearsal_mark"))    return actionAddRehearsalMark(body);
    if (action == QLatin1String("set_rehearsal_mark"))    return actionSetRehearsalMark(body);
    if (action == QLatin1String("remove_rehearsal_mark")) return actionRemoveRehearsalMark(body);
    if (action == QLatin1String("insert_volta"))          return actionInsertVolta(body);
    if (action == QLatin1String("remove_volta"))        return actionRemoveVolta(body);
    if (action == QLatin1String("insert_marker"))       return actionInsertMarker(body);
    if (action == QLatin1String("remove_marker"))       return actionRemoveMarker(body);
    if (action == QLatin1String("insert_jump"))         return actionInsertJump(body);
    if (action == QLatin1String("remove_jump"))         return actionRemoveJump(body);
    if (action == QLatin1String("set_measure_len"))      return actionSetMeasureLen(body);
    if (action == QLatin1String("insert_beats"))        return actionInsertBeats(body);
    if (action == QLatin1String("delete_beats"))        return actionDeleteBeats(body);
    if (action == QLatin1String("set_score_metadata"))  return actionSetScoreMetadata(body);

    return errorResponse(400, QString("unknown action: %1").arg(action));
}

EditudeTestServer::Reply EditudeTestServer::handleAction(const QJsonObject& body)
{
    // Defer score mutation to the next event-loop iteration.  Action
    // handlers call ScoreApplicator::apply() which runs startCmd →
    // insertMeasure → endCmd → doLayoutRange.  Running that layout
    // pass inside QTcpSocket::readyRead corrupts score-DOM pointers
    // because Qt's scene-graph (NotationPaintView) still holds live
    // references to the pre-edit layout data.  Deferring with
    // QTimer::singleShot(0) matches the pattern used by
    // EditudeService::onServerMessage for live ops (see lines 266-295
    // of editudeservice.cpp).
    Reply result;
    QEventLoop loop;
    QTimer::singleShot(0, this, [&]() {
        result = dispatchAction(body);
        loop.quit();
    });
    loop.exec();

    // Flush pending events so that any WebSocket data queued by
    // EditudeService::onScoreChanges (called synchronously during the
    // action's endCmd → changesChannel) is actually written to the
    // network before the HTTP response reaches the Python harness.
    // Without this, the harness may call peer.wait_revision() before
    // the editor's op_batch has left the process.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    return result;
}

EditudeTestServer::Reply EditudeTestServer::actionInsertNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
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
        return errorResponse(422,
                             "invalid pitch");
    }

    const DurationType dt = ScoreApplicator::parseDurationType(durObj["type"].toString());
    if (dt == DurationType::V_INVALID) {
        return errorResponse(422,
                             "invalid duration type");
    }
    const int dots = durObj["dots"].toInt(0);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionInsertRest(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    const QJsonObject durObj  = body["duration"].toObject();
    const track_idx_t track   = static_cast<track_idx_t>(body.value("track").toInt(0));

    const DurationType dt = ScoreApplicator::parseDurationType(durObj["type"].toString());
    if (dt == DurationType::V_INVALID) {
        return errorResponse(422,
                             "invalid duration type");
    }
    const int dots = durObj["dots"].toInt(0);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        return errorResponse(422,
                             "beat not found in score");
    }

    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("test", "insert rest"));
    score->setNoteRest(seg, track, NoteVal(), dur.ticks());
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionDeleteEvent(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    if (eventId.isEmpty()) {
        return errorResponse(400,
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
        return errorResponse(404, "event not found");
    }

    auto* item = dynamic_cast<EngravingItem*>(obj);
    if (!item) {
        return errorResponse(422,
                             "element is not an EngravingItem");
    }

    score->startCmd(TranslatableString("test", "delete event"));
    score->deleteItem(item);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetPitch(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    if (eventId.isEmpty()) {
        return errorResponse(400,
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
        return errorResponse(404, "event not found");
    }

    Note* note = dynamic_cast<Note*>(obj);
    if (!note) {
        return errorResponse(422,
                             "element is not a Note");
    }

    const QJsonObject pitchObj = body["pitch"].toObject();
    const int midi = ScoreApplicator::pitchToMidi(
        pitchObj["step"].toString(),
        pitchObj["octave"].toInt(),
        pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(422,
                             "invalid pitch");
    }

    // Set PITCH, TPC1, and TPC2 together — setPitch(int) does NOT update
    // TPC, so the translator's pitchJson(tpc1, octave) would read stale TPC
    // and emit the wrong step+accidental.
    const int tpc1 = note->tpc1default(midi);
    const int tpc2 = note->tpc2default(midi);
    score->startCmd(TranslatableString("test", "set pitch"));
    note->undoChangeProperty(Pid::PITCH, midi);
    note->undoChangeProperty(Pid::TPC1, tpc1);
    note->undoChangeProperty(Pid::TPC2, tpc2);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionUndo()
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }
    // Use Score::undoRedo instead of bare undoStack()->undo().
    // undoRedo calls update() (layout refresh), updateSelection(),
    // and changesChannel().send() (OT op translation).  Without
    // these the score DOM is left stale, causing SIGSEGV when the
    // accessibility system or scene graph accesses it later.
    EditData ed;
    score->undoRedo(true, &ed);
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::handleConnect(const QJsonObject& body)
{
    const QString sessionUrl = body.value("session_url").toString();
    if (sessionUrl.isEmpty())
        return errorResponse(400, "session_url required");
    m_svc->connectToSession(sessionUrl);
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::handleStatus()
{
    return { 200, QJsonDocument(QJsonObject{
        { "state",    m_svc->stateForTest() },
        { "revision", m_svc->serverRevisionForTest() },
    }).toJson(QJsonDocument::Compact) };
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
        { "metric_grid",      serializeMetricGrid() },
        { "measure_len_overrides", serializeMeasureLenOverrides() },
        { "tempo_map",        serializeTempoMap() },
        { "chord_symbols",    serializeScoreChordSymbols() },
        { "system_texts",     serializeScoreSystemTexts() },
        { "rehearsal_marks",  serializeScoreRehearsalMarks() },
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
    QJsonObject instrument{
        { "musescore_id", part->instrumentId().toQString() },
        { "name",         part->longName().toQString() },
        { "short_name",   part->shortName().toQString() },
    };

    // Serialise StringData if the instrument has fretted-instrument string data.
    const Instrument* inst = part->instrument();
    if (inst) {
        const StringData* sd = inst->stringData();
        if (sd && !sd->stringList().empty()) {
            QJsonArray strings;
            for (const instrString& s : sd->stringList()) {
                QJsonObject sObj;
                sObj["pitch"]      = s.pitch;
                sObj["open"]       = s.open;
                sObj["start_fret"] = s.startFret;
                strings.append(sObj);
            }
            QJsonObject sdObj;
            sdObj["frets"]   = sd->frets();
            sdObj["strings"] = strings;
            instrument["string_data"] = sdObj;
        }

        // Percussion: serialize use_drumset and drumset_overrides.
        instrument["use_drumset"] = inst->useDrumset();
        if (inst->useDrumset() && inst->drumset()) {
            const Drumset* ds = inst->drumset();
            QJsonObject instruments;
            for (int pitch = 0; pitch < 128; ++pitch) {
                if (!ds->isValid(pitch)) {
                    continue;
                }
                QJsonObject entry;
                entry["name"]           = ds->name(pitch).toQString();
                entry["notehead"]       = noteheadGroupToString(ds->noteHead(pitch));
                entry["line"]           = ds->line(pitch);
                entry["stem_direction"] = stemDirectionToString(ds->stemDirection(pitch));
                entry["voice"]          = ds->voice(pitch);
                entry["shortcut"]       = ds->shortcut(pitch).toQString();

                const auto& variants = ds->variants(pitch);
                if (!variants.empty()) {
                    QJsonArray varArr;
                    for (const DrumInstrumentVariant& v : variants) {
                        QJsonObject vObj;
                        vObj["pitch"] = v.pitch;
                        if (v.tremolo != TremoloType::INVALID_TREMOLO) {
                            vObj["tremolo_type"] = tremoloTypeToString(v.tremolo);
                        }
                        if (!v.articulationName.isEmpty()) {
                            vObj["articulation_name"] = v.articulationName.toQString();
                        }
                        varArr.append(vObj);
                    }
                    entry["variants"] = varArr;
                }

                instruments[QString::number(pitch)] = entry;
            }
            QJsonObject dsObj;
            dsObj["instruments"] = instruments;
            instrument["drumset_overrides"] = dsObj;
        }
    }

    const QString partUuid = uuidForPart(part);

    return QJsonObject{
        { "id",          partUuid.isEmpty()
                             ? QString::fromStdString(part->id().toStdString())
                             : partUuid },
        { "instrument",  instrument },
        { "name",        part->partName().toQString() },
        { "staff_count", static_cast<int>(part->nstaves()) },
        { "events",      serializePartEvents(part) },
        { "clef_changes", serializePartClefChanges(part) },
        { "key_changes",  serializePartKeyChanges(part) },
        { "articulations", serializePartArticulations(part) },
        { "dynamics",      serializePartDynamics(part) },
        { "slurs",         serializePartSlurs(part) },
        { "hairpins",      serializePartHairpins(part) },
        { "octave_lines",  serializePartOctaveLines(part) },
        { "glissandos",    serializePartGlissandos(part) },
        { "pedal_lines",   serializePartPedalLines(part) },
        { "trill_lines",   serializePartTrillLines(part) },
        { "tuplets",       serializePartTuplets(part) },
        { "lyrics",        serializePartLyricsMap(part) },
        { "staff_texts",   serializePartStaffTexts(part) },
        { "arpeggios",     serializePartArpeggios(part) },
        { "grace_notes",   serializePartGraceNotes(part) },
        { "breaths",       serializePartBreaths(part) },
        { "tremolos",          serializePartTremolos(part) },
        { "two_note_tremolos", serializePartTwoNoteTremolos(part) },
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
                const QString uuid = uuidForChordRest(el);
                if (uuid.isEmpty()) {
                    continue;
                }
                const Fraction tick = seg->tick();
                if (el->isRest()) {
                    events.append(serializeRest(toRest(el), uuid, tick));
                } else if (el->isChord()) {
                    Chord* chord = toChord(el);
                    if (chord->notes().size() == 1) {
                        // Single-note chord → serialize as a "note" event.
                        events.append(serializeNote(chord->notes().front(), uuid, tick));
                    } else {
                        events.append(serializeChord(chord, uuid, tick));
                    }
                }
            }
        }
    }
    return events;
}

QJsonObject EditudeTestServer::serializeNote(Note* note, const QString& uuid,
                                              const Fraction& tick)
{
    QJsonObject obj{
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

    // Tab fields: include fret/string if the note carries tab data.
    if (note->fret() >= 0 && note->string() >= 0) {
        obj["fret"]   = note->fret();
        obj["string"] = note->string();
    }

    // Percussion: include notehead if non-default.
    if (note->headGroup() != NoteHeadGroup::HEAD_NORMAL) {
        obj["notehead"] = noteheadGroupToString(note->headGroup());
    }
    return obj;
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

QString EditudeTestServer::uuidForPart(Part* part) const
{
    // Check translator map (Part* → UUID).
    const auto& knownParts = m_svc->translatorKnownPartUuids();
    auto it = knownParts.find(part);
    if (it != knownParts.end()) {
        return it.value();
    }
    // Fall back to applicator map (UUID → Part*), reverse lookup.
    const auto& applMap = m_svc->applicatorPartUuidToPart();
    for (auto it2 = applMap.cbegin(); it2 != applMap.cend(); ++it2) {
        if (it2.value() == part) {
            return it2.key();
        }
    }
    return QString();
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
    const Fraction r = tick.reduced();
    return QJsonObject{
        { "numerator",   r.numerator() },
        { "denominator", r.denominator() },
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

EditudeTestServer::Reply EditudeTestServer::actionAddPart(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString name = body["name"].toString();
    const int staffCount = body.value("staff_count").toInt(1);

    Part* part = new Part(score);
    part->setPartName(String(name));

    const QJsonObject instr = body["instrument"].toObject();
    if (!instr.isEmpty()) {
        const QString msId      = instr["musescore_id"].toString();
        const QString longName  = instr["name"].toString(name);
        const QString shortName = instr["short_name"].toString();
        // Modify the Part's existing default Instrument in-place.
        // Part() already creates a properly-initialised Instrument at tick -1;
        // calling setInstrument() with a minimal stack Instrument inserts a
        // second entry at a different tick and triggers an assertion during undo.
        Instrument* existing = part->instrument();
        if (existing && !msId.isEmpty()) {
            existing->setId(String(msId));
            existing->setLongName(String(longName));
            existing->setShortName(String(shortName));
        }
        part->setLongNameAll(String(longName));
        part->setShortNameAll(String(shortName));
    }

    // Insert Part FIRST, then staves — same rationale as applyAddPart.
    // During undo (reversed), staves are removed before the Part, so
    // removePart doesn't encounter orphaned staves in Score::m_staves.
    score->startCmd(TranslatableString("test", "add part"));
    score->undoInsertPart(part, score->parts().size());
    for (int i = 0; i < staffCount; ++i) {
        Staff* staff = Factory::createStaff(part);
        score->undoInsertStaff(staff, static_cast<staff_idx_t>(i), false);
    }
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemovePart(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    score->startCmd(TranslatableString("test", "remove part"));
    score->cmdRemovePart(part);
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetPartName(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QString name = body["name"].toString();
    score->startCmd(TranslatableString("test", "set part name"));
    part->setPartName(String(name));
    part->setLongNameAll(String(name));
    // Register a tracked ChangeProperty so the translator's Pass 10 detects
    // the name change via Pid::STAFF_LONG_NAME on Part.
    part->undoChangeProperty(Pid::STAFF_LONG_NAME, PropertyValue(String(name)));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetStaffCount(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const int target = body["staff_count"].toInt(0);
    if (target <= 0) {
        return errorResponse(422,
                             "invalid staff_count");
    }
    const int current = static_cast<int>(part->nstaves());
    if (target == current) {
        return okResponse();
    }

    score->startCmd(TranslatableString("test", "set staff count"));
    if (target > current) {
        for (int i = current; i < target; ++i) {
            Staff* staff = Factory::createStaff(part);
            // ridx is relative to the part, not absolute
            score->undoInsertStaff(staff, static_cast<staff_idx_t>(i), false);
        }
    } else {
        const staff_idx_t partStart = part->startTrack() / VOICES;
        for (int i = current; i > target; --i) {
            // cmdRemoveStaff takes an absolute staff index
            score->cmdRemoveStaff(static_cast<staff_idx_t>(partStart + i - 1));
        }
    }
    score->endCmd();
    return okResponse();
}

// ---------------------------------------------------------------------------
// Tier 3 serialization helpers
// ---------------------------------------------------------------------------

// articulationNameFromSymId, dynamicKindName, markerKindName are defined
// inline in internal/editudeutils.h (shared with operationtranslator.cpp).

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

QJsonObject EditudeTestServer::serializePartTuplets(Part* part)
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
                Tuplet* tup = toChordRest(el)->tuplet();
                if (!tup) {
                    continue;
                }
                const QString tupUuid = uuidForElement(tup);
                if (tupUuid.isEmpty() || result.contains(tupUuid)) {
                    continue;
                }
                QJsonArray memberIds;
                bool allMembersValid = true;
                for (DurationElement* member : tup->elements()) {
                    const QString mid = uuidForChordRest(member);
                    if (mid.isEmpty()) {
                        allMembersValid = false;
                        break;
                    }
                    memberIds.append(mid);
                }
                if (!allMembersValid) {
                    continue;
                }
                QJsonObject baseDur;
                baseDur["type"] = durationTypeName(tup->baseLen().type());
                baseDur["dots"] = tup->baseLen().dots();
                result[tupUuid] = QJsonObject{
                    { "id",            tupUuid },
                    { "beat",          beatJson(tup->tick()) },
                    { "actual_notes",  tup->ratio().numerator() },
                    { "normal_notes",  tup->ratio().denominator() },
                    { "base_duration", baseDur },
                    { "member_ids",    memberIds },
                };
            }
        }
    }
    return result;
}

static QString testGraceNoteTypeName(NoteType nt)
{
    static const QHash<NoteType, QString> s_map = {
        { NoteType::ACCIACCATURA,  QStringLiteral("acciaccatura") },
        { NoteType::APPOGGIATURA,  QStringLiteral("appoggiatura") },
        { NoteType::GRACE4,        QStringLiteral("grace4") },
        { NoteType::GRACE16,       QStringLiteral("grace16") },
        { NoteType::GRACE32,       QStringLiteral("grace32") },
        { NoteType::GRACE8_AFTER,  QStringLiteral("grace8_after") },
        { NoteType::GRACE16_AFTER, QStringLiteral("grace16_after") },
        { NoteType::GRACE32_AFTER, QStringLiteral("grace32_after") },
    };
    return s_map.value(nt, QStringLiteral("acciaccatura"));
}

static QString arpeggioDirectionName(ArpeggioType t)
{
    static const QHash<ArpeggioType, QString> s_map = {
        { ArpeggioType::NORMAL,        QStringLiteral("normal") },
        { ArpeggioType::UP,            QStringLiteral("up") },
        { ArpeggioType::DOWN,          QStringLiteral("down") },
        { ArpeggioType::BRACKET,       QStringLiteral("bracket") },
        { ArpeggioType::UP_STRAIGHT,   QStringLiteral("up_straight") },
        { ArpeggioType::DOWN_STRAIGHT, QStringLiteral("down_straight") },
    };
    return s_map.value(t, QStringLiteral("normal"));
}

QJsonObject EditudeTestServer::serializePartArpeggios(Part* part)
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
                Arpeggio* arp = chord->arpeggio();
                if (!arp) {
                    continue;
                }
                const QString arpUuid = uuidForElement(arp);
                if (arpUuid.isEmpty()) {
                    continue;
                }
                const QString eventUuid = uuidForChordRest(chord);
                result[arpUuid] = QJsonObject{
                    { "id",        arpUuid },
                    { "event_id",  eventUuid },
                    { "direction", arpeggioDirectionName(arp->arpeggioType()) },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartGraceNotes(Part* part)
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
                for (Chord* gc : chord->graceNotes()) {
                    const QString gnUuid = uuidForElement(gc);
                    if (gnUuid.isEmpty()) {
                        continue;
                    }
                    const QString eventUuid = uuidForChordRest(chord);
                    Note* firstNote = gc->notes().empty() ? nullptr : gc->notes().front();
                    QJsonObject entry;
                    entry["id"]         = gnUuid;
                    entry["event_id"]   = eventUuid;
                    entry["order"]      = static_cast<int>(gc->graceIndex());
                    entry["grace_type"] = testGraceNoteTypeName(gc->noteType());
                    if (firstNote) {
                        entry["pitch"] = pitchJson(firstNote);
                    }
                    result[gnUuid] = entry;
                }
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartBreaths(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->elist()) {
                if (!el || el->type() != ElementType::BREATH) {
                    continue;
                }
                if (el->track() < part->startTrack() || el->track() >= part->endTrack()) {
                    continue;
                }
                Breath* breath = static_cast<Breath*>(el);
                const QString bUuid = uuidForElement(breath);
                if (bUuid.isEmpty()) {
                    continue;
                }
                QJsonObject entry;
                entry["id"]          = bUuid;
                entry["beat"]        = beatJson(seg->tick());
                entry["breath_type"] = breathTypeToString(breath->symId());
                entry["pause"]       = breath->pause();
                result[bUuid] = entry;
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartTremolos(Part* part)
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
                TremoloSingleChord* trem = chord->tremoloSingleChord();
                if (!trem) {
                    continue;
                }
                const QString tremUuid = uuidForElement(trem);
                if (tremUuid.isEmpty()) {
                    continue;
                }
                const QString eventUuid = uuidForChordRest(chord);
                result[tremUuid] = QJsonObject{
                    { "id",           tremUuid },
                    { "event_id",     eventUuid },
                    { "tremolo_type", tremoloTypeToString(trem->tremoloType()) },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartTwoNoteTremolos(Part* part)
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
                TremoloTwoChord* trem = chord->tremoloTwoChord();
                if (!trem) {
                    continue;
                }
                // Serialize only when chord == trem->chord1() to avoid double-counting.
                if (chord != trem->chord1()) {
                    continue;
                }
                const QString tremUuid = uuidForElement(trem);
                if (tremUuid.isEmpty()) {
                    continue;
                }
                const QString startUuid = uuidForChordRest(trem->chord1());
                const QString endUuid   = uuidForChordRest(trem->chord2());
                result[tremUuid] = QJsonObject{
                    { "id",             tremUuid },
                    { "start_event_id", startUuid },
                    { "end_event_id",   endUuid },
                    { "tremolo_type",   tremoloTypeToString(trem->tremoloType()) },
                };
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

QJsonObject EditudeTestServer::serializePartOctaveLines(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isOttava()) {
            continue;
        }
        if (sp->track() < part->startTrack() || sp->track() >= part->endTrack()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        Ottava* ot = toOttava(sp);
        QString kind;
        switch (ot->ottavaType()) {
        case OttavaType::OTTAVA_8VA:  kind = QStringLiteral("8va");  break;
        case OttavaType::OTTAVA_8VB:  kind = QStringLiteral("8vb");  break;
        case OttavaType::OTTAVA_15MA: kind = QStringLiteral("15ma"); break;
        case OttavaType::OTTAVA_15MB: kind = QStringLiteral("15mb"); break;
        default:                      kind = QStringLiteral("8va");  break;
        }
        result[uuid] = QJsonObject{
            { "id",         uuid },
            { "start_beat", beatJson(ot->tick()) },
            { "end_beat",   beatJson(ot->tick2()) },
            { "kind",       kind },
        };
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartGlissandos(Part* part)
{
    // Glissandos are NOTE-anchored spanners stored on Note::spannerFor(),
    // NOT in score->spanner().  Iterate notes in the part to find them.
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
                for (Note* note : chord->notes()) {
                    for (Spanner* sp : note->spannerFor()) {
                        if (!sp->isGlissando()) {
                            continue;
                        }
                        const QString uuid = uuidForElement(sp);
                        if (uuid.isEmpty()) {
                            continue;
                        }
                        Glissando* gl = toGlissando(sp);
                        const QString style = (gl->glissandoType() == GlissandoType::WAVY)
                            ? QStringLiteral("wavy")
                            : QStringLiteral("straight");
                        const QString startUuid = uuidForChordRest(chord);
                        EngravingItem* endEl = sp->endElement();
                        ChordRest* endCR = nullptr;
                        if (endEl && endEl->isNote()) {
                            endCR = toNote(endEl)->chord();
                        }
                        const QString endUuid = endCR ? uuidForChordRest(endCR) : QString();
                        result[uuid] = QJsonObject{
                            { "id",             uuid      },
                            { "start_event_id", startUuid },
                            { "end_event_id",   endUuid   },
                            { "style",          style     },
                        };
                    }
                }
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartPedalLines(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isPedal()) {
            continue;
        }
        if (sp->track() < part->startTrack() || sp->track() >= part->endTrack()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        result[uuid] = QJsonObject{
            { "id",         uuid },
            { "start_beat", beatJson(sp->tick()) },
            { "end_beat",   beatJson(sp->tick2()) },
        };
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartTrillLines(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (auto& kv : score->spanner()) {
        Spanner* sp = kv.second;
        if (!sp->isTrill()) {
            continue;
        }
        if (sp->track() < part->startTrack() || sp->track() >= part->endTrack()) {
            continue;
        }
        const QString uuid = uuidForElement(sp);
        if (uuid.isEmpty()) {
            continue;
        }
        Trill* tr = toTrill(sp);
        QJsonValue accVal = QJsonValue::Null;
        if (tr->accidental()) {
            AccidentalType at = tr->accidental()->accidentalType();
            switch (at) {
            case AccidentalType::FLAT:    accVal = QStringLiteral("flat");         break;
            case AccidentalType::SHARP:   accVal = QStringLiteral("sharp");        break;
            case AccidentalType::NATURAL: accVal = QStringLiteral("natural");      break;
            case AccidentalType::FLAT2:   accVal = QStringLiteral("double-flat");  break;
            case AccidentalType::SHARP2:  accVal = QStringLiteral("double-sharp"); break;
            default:                      break;
            }
        }
        result[uuid] = QJsonObject{
            { "id",         uuid },
            { "start_beat", beatJson(sp->tick()) },
            { "end_beat",   beatJson(sp->tick2()) },
            { "accidental", accVal },
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

QJsonArray EditudeTestServer::serializeMeasureLenOverrides()
{
    Score* score = m_svc->scoreForTest();
    QJsonArray arr;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        if (m->ticks() != m->timesig()) {
            arr.append(QJsonObject{
                { "beat",       beatJson(m->tick()) },
                { "actual_len", beatJson(m->ticks()) },
            });
        }
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Tier 3 action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionSetPartInstrument(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject instr = body["instrument"].toObject();
    if (instr.isEmpty()) {
        return errorResponse(422,
                             "instrument required");
    }
    const QString msId      = instr["musescore_id"].toString();
    const QString longName  = instr["name"].toString();
    const QString shortName = instr["short_name"].toString();

    score->startCmd(TranslatableString("test", "set part instrument"));
    // Modify existing Instrument in-place — same rationale as actionAddPart.
    Instrument* existing = part->instrument();
    if (existing && !msId.isEmpty()) {
        existing->setId(String(msId));
        existing->setLongName(String(longName));
        existing->setShortName(String(shortName));
    }
    part->setPartName(String(longName));
    part->setLongNameAll(String(longName));
    part->setShortNameAll(String(shortName));
    part->undoChangeProperty(Pid::STAFF_LONG_NAME, PropertyValue(String(longName)));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetStringData(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422, "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject sdObj = body["string_data"].toObject();
    if (sdObj.isEmpty()) {
        return errorResponse(422, "string_data required");
    }

    Instrument* inst = part->instrument();
    if (!inst) {
        return errorResponse(422, "part has no instrument");
    }

    const QJsonArray strings = sdObj["strings"].toArray();
    std::vector<instrString> table;
    for (const QJsonValue& v : strings) {
        const QJsonObject s = v.toObject();
        table.emplace_back(s["pitch"].toInt(), s["open"].toBool(false),
                           s["start_fret"].toInt(0));
    }
    StringData sd(sdObj["frets"].toInt(), table);

    score->startCmd(TranslatableString("test", "set string data"));
    inst->setStringData(sd);
    // setStringData() does not go through the undo system, so the translator
    // sees no changes.  Trigger a STAFF_LONG_NAME property change (identity)
    // so Pass 10 fires and emits SetPartInstrument with the updated string_data.
    part->undoChangeProperty(Pid::STAFF_LONG_NAME, PropertyValue(String(part->longName())));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetCapo(const QJsonObject& body)
{
    // Capo is tracked in the Python model.  The C++ test server acknowledges
    // the action without modifying the score — this mirrors the applicator's
    // behaviour and ensures the op round-trips cleanly in e2e tests.
    Q_UNUSED(body);
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetTabNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    const int fret   = body["fret"].toInt(-1);
    const int string = body["string"].toInt(-1);
    if (fret < 0 || string < 0) {
        return errorResponse(422, "fret and string required");
    }

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(422, "event not found");
    }
    Note* note = dynamic_cast<Note*>(obj);
    if (!note) {
        return errorResponse(422, "event is not a Note");
    }

    score->startCmd(TranslatableString("test", "set tab note"));
    note->undoChangeProperty(Pid::FRET, fret);
    note->undoChangeProperty(Pid::STRING, string);
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetDrumset(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422, "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject dsObj = body["drumset"].toObject();
    const QJsonObject instruments = dsObj["instruments"].toObject();

    Drumset* ds = new Drumset();
    for (auto it = instruments.begin(); it != instruments.end(); ++it) {
        const int pitch = it.key().toInt();
        if (pitch < 0 || pitch > 127) {
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        DrumInstrument di;
        di.name          = String::fromQString(entry["name"].toString());
        di.notehead      = noteheadGroupFromString(entry["notehead"].toString());
        di.line          = entry["line"].toInt();
        di.stemDirection = stemDirectionFromString(entry["stem_direction"].toString());
        di.voice         = entry["voice"].toInt();
        di.shortcut      = String::fromQString(entry["shortcut"].toString());

        const QJsonArray varArr = entry["variants"].toArray();
        for (const QJsonValue& vv : varArr) {
            const QJsonObject vObj = vv.toObject();
            DrumInstrumentVariant v;
            v.pitch            = vObj["pitch"].toInt(INVALID_PITCH);
            v.tremolo          = vObj.contains("tremolo_type")
                                     ? tremoloTypeFromString(vObj["tremolo_type"].toString())
                                     : TremoloType::INVALID_TREMOLO;
            v.articulationName = String::fromQString(
                vObj["articulation_name"].toString());
            di.addVariant(v);
        }
        ds->drum(pitch) = di;
    }

    score->startCmd(TranslatableString("test", "set drumset"));
    Instrument* inst = part->instrument();
    inst->setUseDrumset(true);
    inst->setDrumset(ds);
    // Trigger translator Pass 10 via identity property change.
    part->undoChangeProperty(Pid::STAFF_LONG_NAME, part->longName().toQString());
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetNoteHead(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    const QString headStr = body["notehead"].toString();
    if (headStr.isEmpty()) {
        return errorResponse(422, "notehead required");
    }

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(422, "event not found");
    }
    Note* note = dynamic_cast<Note*>(obj);
    if (!note) {
        return errorResponse(422, "event is not a Note");
    }

    const NoteHeadGroup headGroup = noteheadGroupFromString(headStr);

    score->startCmd(TranslatableString("test", "set note head"));
    note->undoChangeProperty(Pid::HEAD_GROUP, int(headGroup));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddArticulation(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    const QString artName = body["articulation"].toString();

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    ChordRest* cr = nullptr;
    if (obj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(obj));
    }
    if (!cr) {
        return errorResponse(422,
                             "event is not a note or chordrest");
    }

    static const QHash<QString, SymId> s_artMap = {
        // --- Standard articulations ---
        { "staccato",                    SymId::articStaccatoAbove                 },
        { "accent",                      SymId::articAccentAbove                   },
        { "tenuto",                      SymId::articTenutoAbove                   },
        { "marcato",                     SymId::articMarcatoAbove                  },
        { "staccatissimo",               SymId::articStaccatissimoAbove            },
        { "staccatissimo_stroke",        SymId::articStaccatissimoStrokeAbove      },
        { "staccatissimo_wedge",         SymId::articStaccatissimoWedgeAbove       },
        { "tenuto_staccato",             SymId::articTenutoStaccatoAbove           },
        { "accent_staccato",             SymId::articAccentStaccatoAbove           },
        { "marcato_staccato",            SymId::articMarcatoStaccatoAbove          },
        { "marcato_tenuto",              SymId::articMarcatoTenutoAbove            },
        { "tenuto_accent",               SymId::articTenutoAccentAbove             },
        { "stress",                      SymId::articStressAbove                   },
        { "unstress",                    SymId::articUnstressAbove                 },
        { "soft_accent",                 SymId::articSoftAccentAbove               },
        { "soft_accent_staccato",        SymId::articSoftAccentStaccatoAbove       },
        { "soft_accent_tenuto",          SymId::articSoftAccentTenutoAbove         },
        { "soft_accent_tenuto_staccato", SymId::articSoftAccentTenutoStaccatoAbove },
        // --- Fermatas ---
        { "fermata",             SymId::fermataAbove          },
        { "fermata_short",       SymId::fermataShortAbove     },
        { "fermata_long",        SymId::fermataLongAbove      },
        { "fermata_very_short",  SymId::fermataVeryShortAbove },
        { "fermata_very_long",   SymId::fermataVeryLongAbove  },
        { "fermata_long_henze",  SymId::fermataLongHenzeAbove },
        { "fermata_short_henze", SymId::fermataShortHenzeAbove },
        // --- Ornaments ---
        { "trill",                 SymId::ornamentTrill                     },
        { "mordent",               SymId::ornamentMordent                   },
        { "turn",                  SymId::ornamentTurn                      },
        { "turn_inverted",         SymId::ornamentTurnInverted              },
        { "turn_slash",            SymId::ornamentTurnSlash                 },
        { "turn_up",               SymId::ornamentTurnUp                    },
        { "short_trill",           SymId::ornamentShortTrill                },
        { "tremblement",           SymId::ornamentTremblement               },
        { "prall_mordent",         SymId::ornamentPrallMordent              },
        { "up_prall",              SymId::ornamentUpPrall                   },
        { "mordent_upper_prefix",  SymId::ornamentPrecompMordentUpperPrefix },
        { "up_mordent",            SymId::ornamentUpMordent                 },
        { "down_mordent",          SymId::ornamentDownMordent               },
        { "prall_down",            SymId::ornamentPrallDown                 },
        { "prall_up",              SymId::ornamentPrallUp                   },
        { "line_prall",            SymId::ornamentLinePrall                 },
        { "precomp_slide",         SymId::ornamentPrecompSlide              },
        { "shake",                 SymId::ornamentShake3                    },
        { "shake_muffat",          SymId::ornamentShakeMuffat1              },
        { "tremblement_couperin",  SymId::ornamentTremblementCouperin       },
        { "pince_couperin",        SymId::ornamentPinceCouperin             },
        { "haydn",                 SymId::ornamentHaydn                     },
        // --- Bowing / string techniques ---
        { "up_bow",               SymId::stringsUpBow              },
        { "down_bow",             SymId::stringsDownBow            },
        { "harmonic",             SymId::stringsHarmonic            },
        { "snap_pizzicato",       SymId::pluckedSnapPizzicatoAbove },
        { "left_hand_pizzicato",  SymId::pluckedLeftHandPizzicato  },
        // --- Brass ---
        { "brass_mute_open",   SymId::brassMuteOpen   },
        { "brass_mute_closed", SymId::brassMuteClosed },
        // --- Guitar ---
        { "guitar_fade_in",      SymId::guitarFadeIn      },
        { "guitar_fade_out",     SymId::guitarFadeOut     },
        { "guitar_volume_swell", SymId::guitarVolumeSwell },
    };
    SymId symId = s_artMap.value(artName, SymId::noSym);
    if (symId == SymId::noSym) {
        // Fallback: try raw SMuFL name (for forward-compat passthrough)
        symId = SymNames::symIdByName(artName.toUtf8().constData());
    }
    if (symId == SymId::noSym) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionRemoveArticulation(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "articulation not found");
    }

    Articulation* art = dynamic_cast<Articulation*>(obj);
    if (!art) {
        return errorResponse(422,
                             "element is not an Articulation");
    }

    score->startCmd(TranslatableString("test", "remove articulation"));
    score->undoRemoveElement(art);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddArpeggio(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString eventId  = body["event_id"].toString();
    const QString dirName  = body["direction"].toString();

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    Chord* chord = nullptr;
    if (obj->isNote()) {
        chord = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChord()) {
        chord = toChord(static_cast<EngravingItem*>(obj));
    }
    if (!chord) {
        return errorResponse(422, "event is not a note or chord");
    }

    static const QHash<QString, ArpeggioType> s_arpMap = {
        { QStringLiteral("normal"),        ArpeggioType::NORMAL },
        { QStringLiteral("up"),            ArpeggioType::UP },
        { QStringLiteral("down"),          ArpeggioType::DOWN },
        { QStringLiteral("bracket"),       ArpeggioType::BRACKET },
        { QStringLiteral("up_straight"),   ArpeggioType::UP_STRAIGHT },
        { QStringLiteral("down_straight"), ArpeggioType::DOWN_STRAIGHT },
    };
    const ArpeggioType arpType = s_arpMap.value(dirName, ArpeggioType::NORMAL);

    score->startCmd(TranslatableString("test", "add arpeggio"));
    Arpeggio* arp = Factory::createArpeggio(chord);
    arp->setArpeggioType(arpType);
    arp->setParent(chord);
    arp->setTrack(chord->track());
    score->undoAddElement(arp);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveArpeggio(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "arpeggio not found");
    }

    Arpeggio* arp = dynamic_cast<Arpeggio*>(obj);
    if (!arp) {
        return errorResponse(422, "element is not an Arpeggio");
    }

    score->startCmd(TranslatableString("test", "remove arpeggio"));
    score->undoRemoveElement(arp);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddTuplet(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const int actualNotes = body["actual_notes"].toInt(0);
    const int normalNotes = body["normal_notes"].toInt(0);
    const QString baseType = body["base_duration"].toString();
    const track_idx_t track = static_cast<track_idx_t>(body.value("track").toInt(0));

    if (actualNotes <= 0 || normalNotes <= 0) {
        return errorResponse(422, "actual_notes and normal_notes must be positive");
    }

    const DurationType dt = ScoreApplicator::parseDurationType(baseType);
    if (dt == DurationType::V_INVALID) {
        return errorResponse(422, QString("invalid base_duration: %1").arg(baseType));
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "no measure at beat");
    }

    ChordRest* cr = nullptr;
    for (Segment* seg = measure->first(SegmentType::ChordRest); seg;
         seg = seg->next(SegmentType::ChordRest)) {
        if (seg->tick() == tick) {
            cr = toChordRest(seg->element(track));
            break;
        }
    }
    if (!cr) {
        return errorResponse(422, "no chordrest at beat/track");
    }

    const TDuration baseLen(dt);
    const Fraction totalTicks = baseLen.fraction() * normalNotes;

    score->startCmd(TranslatableString("test", "add tuplet"));
    Tuplet* tuplet = Factory::createTuplet(measure);
    tuplet->setRatio(Fraction(actualNotes, normalNotes));
    tuplet->setTicks(totalTicks);
    tuplet->setBaseLen(baseLen);
    tuplet->setTrack(track);
    tuplet->setTick(tick);
    tuplet->setParent(measure);
    score->cmdCreateTuplet(cr, tuplet);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveTuplet(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const track_idx_t track = static_cast<track_idx_t>(body.value("track").toInt(0));

    // Find the tuplet at the given beat/track by looking for a ChordRest inside one.
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "no measure at beat");
    }

    Tuplet* tup = nullptr;
    for (Segment* seg = measure->first(SegmentType::ChordRest); seg;
         seg = seg->next(SegmentType::ChordRest)) {
        if (seg->tick() >= tick) {
            EngravingItem* el = seg->element(track);
            if (el && el->isChordRest()) {
                tup = toChordRest(el)->tuplet();
                if (tup && tup->tick() == tick) {
                    break;
                }
                tup = nullptr;
            }
        }
    }
    if (!tup) {
        return errorResponse(422, "no tuplet at beat/track");
    }

    score->startCmd(TranslatableString("test", "remove tuplet"));
    score->cmdDeleteTuplet(tup, /*replaceWithRest=*/true);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddGraceNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString eventId   = body["event_id"].toString();
    const int order         = body["order"].toInt(0);
    const QString typeName  = body["grace_type"].toString();
    const QJsonObject pitch = body["pitch"].toObject();

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    Chord* parentChord = nullptr;
    if (obj->isNote()) {
        parentChord = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChord()) {
        parentChord = toChord(static_cast<EngravingItem*>(obj));
    }
    if (!parentChord) {
        return errorResponse(422, "event is not a note or chord");
    }

    static const QHash<QString, NoteType> s_gnMap = {
        { QStringLiteral("acciaccatura"),  NoteType::ACCIACCATURA },
        { QStringLiteral("appoggiatura"),  NoteType::APPOGGIATURA },
        { QStringLiteral("grace4"),        NoteType::GRACE4 },
        { QStringLiteral("grace16"),       NoteType::GRACE16 },
        { QStringLiteral("grace32"),       NoteType::GRACE32 },
        { QStringLiteral("grace8_after"),  NoteType::GRACE8_AFTER },
        { QStringLiteral("grace16_after"), NoteType::GRACE16_AFTER },
        { QStringLiteral("grace32_after"), NoteType::GRACE32_AFTER },
    };
    const NoteType nt = s_gnMap.value(typeName, NoteType::ACCIACCATURA);

    const int midi = ScoreApplicator::pitchToMidi(
        pitch["step"].toString(), pitch["octave"].toInt(),
        pitch["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(422, "invalid pitch");
    }

    // Determine duration type from the NoteType.
    DurationType dt = DurationType::V_EIGHTH;
    if (nt == NoteType::GRACE4)                              dt = DurationType::V_QUARTER;
    else if (nt == NoteType::GRACE16 || nt == NoteType::GRACE16_AFTER) dt = DurationType::V_16TH;
    else if (nt == NoteType::GRACE32 || nt == NoteType::GRACE32_AFTER) dt = DurationType::V_32ND;

    score->startCmd(TranslatableString("test", "add grace note"));

    Chord* graceChord = Factory::createChord(parentChord->segment());
    graceChord->setNoteType(nt);
    graceChord->setGraceIndex(static_cast<size_t>(order));
    graceChord->setTrack(parentChord->track());
    graceChord->setParent(parentChord);

    TDuration dur(dt);
    graceChord->setDurationType(dur);
    graceChord->setTicks(dur.ticks());

    Note* note = Factory::createNote(graceChord);
    note->setPitch(midi);
    note->setTpc1(note->tpc1default(midi));
    note->setTpc2(note->tpc2default(midi));
    note->setTrack(parentChord->track());
    graceChord->add(note);

    score->undoAddElement(graceChord);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveGraceNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "grace note not found");
    }

    Chord* graceChord = dynamic_cast<Chord*>(obj);
    if (!graceChord || !graceChord->isGrace()) {
        return errorResponse(422, "element is not a grace chord");
    }

    score->startCmd(TranslatableString("test", "remove grace note"));
    score->undoRemoveElement(graceChord);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddBreathMark(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422, "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString typeName = body["breath_type"].toString();
    const double pause = body["pause"].toDouble(0.0);

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "beat not found in score");
    }

    score->startCmd(TranslatableString("test", "add breath mark"));
    Segment* seg = measure->undoGetSegment(SegmentType::Breath, tick);
    Breath* breath = Factory::createBreath(seg);
    breath->setParent(seg);
    breath->setTrack(part->startTrack());
    breath->setSymId(breathTypeFromString(typeName));
    breath->setPause(pause);
    score->undoAddElement(breath);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveBreathMark(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "breath mark not found");
    }

    Breath* breath = dynamic_cast<Breath*>(obj);
    if (!breath) {
        return errorResponse(422, "element is not a Breath");
    }

    score->startCmd(TranslatableString("test", "remove breath mark"));
    score->undoRemoveElement(breath);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Tremolo actions (single-note)
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddTremolo(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString eventId  = body["event_id"].toString();
    const QString typeName = body["tremolo_type"].toString();

    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    Chord* chord = nullptr;
    if (obj->isNote()) {
        chord = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChord()) {
        chord = toChord(static_cast<EngravingItem*>(obj));
    }
    if (!chord) {
        return errorResponse(422, "event is not a note or chord");
    }

    score->startCmd(TranslatableString("test", "add tremolo"));
    TremoloSingleChord* trem = Factory::createTremoloSingleChord(chord);
    trem->setTremoloType(tremoloTypeFromString(typeName));
    trem->setParent(chord);
    trem->setTrack(chord->track());
    score->undoAddElement(trem);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveTremolo(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "tremolo not found");
    }

    TremoloSingleChord* trem = dynamic_cast<TremoloSingleChord*>(obj);
    if (!trem) {
        return errorResponse(422, "element is not a TremoloSingleChord");
    }

    score->startCmd(TranslatableString("test", "remove tremolo"));
    score->undoRemoveElement(trem);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Two-note tremolo actions
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddTwoNoteTremolo(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString startId = body["start_event_id"].toString();
    const QString endId   = body["end_event_id"].toString();
    const QString typeName = body["tremolo_type"].toString();

    EngravingObject* startObj = findByUuid(startId);
    EngravingObject* endObj   = findByUuid(endId);
    if (!startObj || !endObj) {
        return errorResponse(404, "event not found");
    }

    Chord* chord1 = nullptr;
    Chord* chord2 = nullptr;
    if (startObj->isNote()) {
        chord1 = toNote(static_cast<EngravingItem*>(startObj))->chord();
    } else if (startObj->isChord()) {
        chord1 = toChord(static_cast<EngravingItem*>(startObj));
    }
    if (endObj->isNote()) {
        chord2 = toNote(static_cast<EngravingItem*>(endObj))->chord();
    } else if (endObj->isChord()) {
        chord2 = toChord(static_cast<EngravingItem*>(endObj));
    }
    if (!chord1 || !chord2) {
        return errorResponse(422, "two-note tremolo requires chord endpoints");
    }

    score->startCmd(TranslatableString("test", "add two-note tremolo"));
    TremoloTwoChord* trem = Factory::createTremoloTwoChord(chord1);
    trem->setTremoloType(tremoloTypeFromString(typeName));
    trem->setChords(chord1, chord2);
    trem->setTrack(chord1->track());
    score->undoAddElement(trem);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveTwoNoteTremolo(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "two-note tremolo not found");
    }

    TremoloTwoChord* trem = dynamic_cast<TremoloTwoChord*>(obj);
    if (!trem) {
        return errorResponse(422, "element is not a TremoloTwoChord");
    }

    score->startCmd(TranslatableString("test", "remove two-note tremolo"));
    score->undoRemoveElement(trem);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString kind = body["kind"].toString();

    static const QHash<QString, DynamicType> s_dynMap = {
        { "pppppp", DynamicType::PPPPPP }, { "ppppp", DynamicType::PPPPP },
        { "pppp", DynamicType::PPPP },     { "ppp", DynamicType::PPP },
        { "pp",  DynamicType::PP  },       { "p",   DynamicType::P   },
        { "mp",  DynamicType::MP  },       { "mf",  DynamicType::MF  },
        { "f",   DynamicType::F   },       { "ff",  DynamicType::FF  },
        { "fff", DynamicType::FFF },       { "ffff", DynamicType::FFFF },
        { "fffff", DynamicType::FFFFF },   { "ffffff", DynamicType::FFFFFF },
        { "fp",  DynamicType::FP  },       { "pf",  DynamicType::PF  },
        { "sf",  DynamicType::SF  },       { "sfz", DynamicType::SFZ },
        { "sff", DynamicType::SFF },       { "sffz", DynamicType::SFFZ },
        { "sfff", DynamicType::SFFF },     { "sfffz", DynamicType::SFFFZ },
        { "sfp", DynamicType::SFP },       { "sfpp", DynamicType::SFPP },
        { "rfz", DynamicType::RFZ },       { "rf",  DynamicType::RF  },
        { "fz",  DynamicType::FZ  },
    };
    const DynamicType dt = s_dynMap.value(kind, DynamicType::OTHER);
    if (dt == DynamicType::OTHER) {
        return errorResponse(422,
                             "unknown dynamic kind");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionSetDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "dynamic not found");
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(obj);
    if (!dyn) {
        return errorResponse(422,
                             "element is not a Dynamic");
    }

    static const QHash<QString, DynamicType> s_dynMap = {
        { "pppppp", DynamicType::PPPPPP }, { "ppppp", DynamicType::PPPPP },
        { "pppp", DynamicType::PPPP },     { "ppp", DynamicType::PPP },
        { "pp",  DynamicType::PP  },       { "p",   DynamicType::P   },
        { "mp",  DynamicType::MP  },       { "mf",  DynamicType::MF  },
        { "f",   DynamicType::F   },       { "ff",  DynamicType::FF  },
        { "fff", DynamicType::FFF },       { "ffff", DynamicType::FFFF },
        { "fffff", DynamicType::FFFFF },   { "ffffff", DynamicType::FFFFFF },
        { "fp",  DynamicType::FP  },       { "pf",  DynamicType::PF  },
        { "sf",  DynamicType::SF  },       { "sfz", DynamicType::SFZ },
        { "sff", DynamicType::SFF },       { "sffz", DynamicType::SFFZ },
        { "sfff", DynamicType::SFFF },     { "sfffz", DynamicType::SFFFZ },
        { "sfp", DynamicType::SFP },       { "sfpp", DynamicType::SFPP },
        { "rfz", DynamicType::RFZ },       { "rf",  DynamicType::RF  },
        { "fz",  DynamicType::FZ  },
    };
    const QString kind = body["kind"].toString();
    const DynamicType dt = s_dynMap.value(kind, DynamicType::OTHER);
    if (dt == DynamicType::OTHER) {
        return errorResponse(422,
                             "unknown dynamic kind");
    }

    score->startCmd(TranslatableString("test", "set dynamic"));
    dyn->undoChangeProperty(Pid::DYNAMIC_TYPE, PropertyValue(dt));
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveDynamic(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "dynamic not found");
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(obj);
    if (!dyn) {
        return errorResponse(422,
                             "element is not a Dynamic");
    }

    score->startCmd(TranslatableString("test", "remove dynamic"));
    score->undoRemoveElement(dyn);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddSlur(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString startId = body["start_event_id"].toString();
    const QString endId   = body["end_event_id"].toString();

    EngravingObject* startObj = findByUuid(startId);
    EngravingObject* endObj   = findByUuid(endId);
    if (!startObj || !endObj) {
        return errorResponse(404, "event not found");
    }

    auto toCR = [](EngravingObject* o) -> ChordRest* {
        if (o->isNote()) return toNote(static_cast<EngravingItem*>(o))->chord();
        if (o->isChordRest()) return toChordRest(static_cast<EngravingItem*>(o));
        return nullptr;
    };
    ChordRest* startCR = toCR(startObj);
    ChordRest* endCR   = toCR(endObj);
    if (!startCR || !endCR) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionRemoveSlur(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "slur not found");
    }

    Slur* slur = dynamic_cast<Slur*>(obj);
    if (!slur) {
        return errorResponse(422,
                             "element is not a Slur");
    }

    score->startCmd(TranslatableString("test", "remove slur"));
    score->undoRemoveElement(slur);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddHairpin(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionRemoveHairpin(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "hairpin not found");
    }

    Hairpin* hp = dynamic_cast<Hairpin*>(obj);
    if (!hp) {
        return errorResponse(422,
                             "element is not a Hairpin");
    }

    score->startCmd(TranslatableString("test", "remove hairpin"));
    score->undoRemoveElement(hp);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Advanced spanners — octave lines
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddOctaveLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject sb = body["start_beat"].toObject();
    const QJsonObject eb = body["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());

    const QString kindStr = body["kind"].toString();
    OttavaType otType = OttavaType::OTTAVA_8VA;
    if (kindStr == QLatin1String("8vb"))       otType = OttavaType::OTTAVA_8VB;
    else if (kindStr == QLatin1String("15ma")) otType = OttavaType::OTTAVA_15MA;
    else if (kindStr == QLatin1String("15mb")) otType = OttavaType::OTTAVA_15MB;

    score->startCmd(TranslatableString("test", "add octave line"));
    Ottava* ottava = Factory::createOttava(score->dummy());
    ottava->setOttavaType(otType);
    ottava->setTrack(part->startTrack());
    ottava->setTick(startTick);
    ottava->setTick2(endTick);
    score->undoAddElement(ottava);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveOctaveLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "octave line not found");
    }

    Ottava* ottava = dynamic_cast<Ottava*>(obj);
    if (!ottava) {
        return errorResponse(422,
                             "element is not an Ottava");
    }

    score->startCmd(TranslatableString("test", "remove octave line"));
    score->undoRemoveElement(ottava);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Advanced spanners — glissandos
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddGlissando(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString startId = body["start_event_id"].toString();
    const QString endId   = body["end_event_id"].toString();

    EngravingObject* startObj = findByUuid(startId);
    EngravingObject* endObj   = findByUuid(endId);
    if (!startObj || !endObj) {
        return errorResponse(404, "event not found");
    }

    auto toCR = [](EngravingObject* o) -> ChordRest* {
        if (o->isNote()) return toNote(static_cast<EngravingItem*>(o))->chord();
        if (o->isChordRest()) return toChordRest(static_cast<EngravingItem*>(o));
        return nullptr;
    };
    ChordRest* startCR = toCR(startObj);
    ChordRest* endCR   = toCR(endObj);
    if (!startCR || !endCR) {
        return errorResponse(422,
                             "event is not a note or chordrest");
    }

    // Glissandos anchor to Notes, not ChordRests.
    Note* startNote = startCR->isChord() ? toChord(startCR)->upNote() : nullptr;
    Note* endNote   = endCR->isChord()   ? toChord(endCR)->upNote()   : nullptr;
    if (!startNote || !endNote) {
        return errorResponse(422, "glissando requires chord (note) endpoints");
    }

    const bool isWavy = (body["style"].toString() == QStringLiteral("wavy"));
    const GlissandoType glType = isWavy ? GlissandoType::WAVY : GlissandoType::STRAIGHT;

    score->startCmd(TranslatableString("test", "add glissando"));
    Glissando* gliss = Factory::createGlissando(score->dummy());
    gliss->setGlissandoType(glType);
    gliss->setAnchor(Spanner::Anchor::NOTE);
    gliss->setTrack(startNote->track());
    gliss->setTrack2(endNote->track());
    gliss->setTick(startNote->tick());
    gliss->setTick2(endNote->tick());
    gliss->setStartElement(startNote);
    gliss->setEndElement(endNote);
    gliss->setParent(startNote);
    score->undoAddElement(gliss);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveGlissando(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "glissando not found");
    }

    Glissando* gliss = dynamic_cast<Glissando*>(obj);
    if (!gliss) {
        return errorResponse(422,
                             "element is not a Glissando");
    }

    score->startCmd(TranslatableString("test", "remove glissando"));
    score->undoRemoveElement(gliss);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Advanced spanners — pedal lines
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddPedalLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject sb = body["start_beat"].toObject();
    const QJsonObject eb = body["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());

    score->startCmd(TranslatableString("test", "add pedal line"));
    Pedal* pedal = Factory::createPedal(score->dummy());
    pedal->setTrack(part->startTrack());
    pedal->setTick(startTick);
    pedal->setTick2(endTick);
    score->undoAddElement(pedal);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemovePedalLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "pedal line not found");
    }

    Pedal* pedal = dynamic_cast<Pedal*>(obj);
    if (!pedal) {
        return errorResponse(422,
                             "element is not a Pedal");
    }

    score->startCmd(TranslatableString("test", "remove pedal line"));
    score->undoRemoveElement(pedal);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Advanced spanners — trill lines
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddTrillLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject sb = body["start_beat"].toObject();
    const QJsonObject eb = body["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());

    score->startCmd(TranslatableString("test", "add trill line"));
    Trill* trill = Factory::createTrill(score->dummy());
    trill->setTrillType(TrillType::TRILL_LINE);
    trill->setTrack(part->startTrack());
    trill->setTick(startTick);
    trill->setTick2(endTick);

    // Set accidental on the Ornament (created by setTrillType) so that
    // layout propagates it to trill->m_accidental during endCmd().
    const QJsonValue accVal = body.value("accidental");
    if (accVal.isString()) {
        const QString accStr = accVal.toString();
        AccidentalType accType = AccidentalType::NONE;
        if (accStr == QLatin1String("flat"))              accType = AccidentalType::FLAT;
        else if (accStr == QLatin1String("sharp"))        accType = AccidentalType::SHARP;
        else if (accStr == QLatin1String("natural"))      accType = AccidentalType::NATURAL;
        else if (accStr == QLatin1String("double-flat"))  accType = AccidentalType::FLAT2;
        else if (accStr == QLatin1String("double-sharp")) accType = AccidentalType::SHARP2;
        if (accType != AccidentalType::NONE && trill->ornament()) {
            Accidental* acc = Factory::createAccidental(trill->ornament());
            acc->setAccidentalType(accType);
            trill->ornament()->setAccidentalAbove(acc);
        }
    }

    score->undoAddElement(trill);

    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveTrillLine(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "trill line not found");
    }

    Trill* trill = dynamic_cast<Trill*>(obj);
    if (!trill) {
        return errorResponse(422,
                             "element is not a Trill");
    }

    score->startCmd(TranslatableString("test", "remove trill line"));
    score->undoRemoveElement(trill);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    ChordRest* cr = nullptr;
    if (obj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(obj))->chord();
    } else if (obj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(obj));
    }
    if (!cr) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionSetLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "lyric not found");
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(obj);
    if (!lyric) {
        return errorResponse(422,
                             "element is not Lyrics");
    }

    const QString text = body["text"].toString();
    score->startCmd(TranslatableString("test", "set lyric"));
    lyric->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveLyric(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "lyric not found");
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(obj);
    if (!lyric) {
        return errorResponse(422,
                             "element is not Lyrics");
    }

    score->startCmd(TranslatableString("test", "remove lyric"));
    score->undoRemoveElement(lyric);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Staff text action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddStaffText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(0);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422, "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString text = body["text"].toString();
    if (text.isEmpty()) {
        return errorResponse(422, "text required");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "beat not found in score");
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);
    score->startCmd(TranslatableString("test", "add staff text"));
    StaffText* st = Factory::createStaffText(seg, TextStyleType::STAFF);
    st->setParent(seg);
    st->setTrack(part->startTrack());
    st->setPlainText(String(text));
    score->undoAddElement(st);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetStaffText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "staff text not found");
    }

    StaffText* st = dynamic_cast<StaffText*>(obj);
    if (!st) {
        return errorResponse(422, "element is not StaffText");
    }

    const QString text = body["text"].toString();
    score->startCmd(TranslatableString("test", "set staff text"));
    st->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveStaffText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "staff text not found");
    }

    StaffText* st = dynamic_cast<StaffText*>(obj);
    if (!st) {
        return errorResponse(422, "element is not StaffText");
    }

    score->startCmd(TranslatableString("test", "remove staff text"));
    score->undoRemoveElement(st);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// System text action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddSystemText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString text = body["text"].toString();
    if (text.isEmpty()) {
        return errorResponse(422, "text required");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "beat not found in score");
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);
    score->startCmd(TranslatableString("test", "add system text"));
    SystemText* st = Factory::createSystemText(seg, TextStyleType::SYSTEM);
    st->setParent(seg);
    st->setTrack(0);
    st->setPlainText(String(text));
    score->undoAddElement(st);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetSystemText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "system text not found");
    }

    SystemText* st = dynamic_cast<SystemText*>(obj);
    if (!st) {
        return errorResponse(422, "element is not SystemText");
    }

    const QString text = body["text"].toString();
    score->startCmd(TranslatableString("test", "set system text"));
    st->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveSystemText(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "system text not found");
    }

    SystemText* st = dynamic_cast<SystemText*>(obj);
    if (!st) {
        return errorResponse(422, "element is not SystemText");
    }

    score->startCmd(TranslatableString("test", "remove system text"));
    score->undoRemoveElement(st);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Rehearsal mark action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddRehearsalMark(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString text = body["text"].toString();
    if (text.isEmpty()) {
        return errorResponse(422, "text required");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422, "beat not found in score");
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);
    score->startCmd(TranslatableString("test", "add rehearsal mark"));
    RehearsalMark* rm = Factory::createRehearsalMark(seg);
    rm->setParent(seg);
    rm->setTrack(0);
    rm->setPlainText(String(text));
    score->undoAddElement(rm);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetRehearsalMark(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "rehearsal mark not found");
    }

    RehearsalMark* rm = dynamic_cast<RehearsalMark*>(obj);
    if (!rm) {
        return errorResponse(422, "element is not RehearsalMark");
    }

    const QString text = body["text"].toString();
    score->startCmd(TranslatableString("test", "set rehearsal mark"));
    rm->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveRehearsalMark(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "rehearsal mark not found");
    }

    RehearsalMark* rm = dynamic_cast<RehearsalMark*>(obj);
    if (!rm) {
        return errorResponse(422, "element is not RehearsalMark");
    }

    score->startCmd(TranslatableString("test", "remove rehearsal mark"));
    score->undoRemoveElement(rm);
    score->endCmd();

    return okResponse();
}

// ---------------------------------------------------------------------------
// Tier 4 action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionInsertVolta(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
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
        : errorResponse(500,
                        "insert_volta failed");
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveVolta(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "volta not found");
    }

    Volta* volta = dynamic_cast<Volta*>(obj);
    if (!volta) {
        return errorResponse(422,
                             "element is not a Volta");
    }

    score->startCmd(TranslatableString("test", "remove volta"));
    score->undoRemoveElement(volta);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionInsertMarker(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
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
        : errorResponse(500,
                        "insert_marker failed");
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveMarker(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "marker not found");
    }

    Marker* marker = dynamic_cast<Marker*>(obj);
    if (!marker) {
        return errorResponse(422,
                             "element is not a Marker");
    }

    score->startCmd(TranslatableString("test", "remove marker"));
    score->undoRemoveElement(marker);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionInsertJump(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
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
        : errorResponse(500,
                        "insert_jump failed");
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveJump(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404, "jump not found");
    }

    Jump* jump = dynamic_cast<Jump*>(obj);
    if (!jump) {
        return errorResponse(422,
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

EditudeTestServer::Reply EditudeTestServer::actionSetMeasureLen(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503, "score not ready");
    }

    const int beatNum = body["beat_num"].toInt(0);
    const int beatDen = body["beat_den"].toInt(1);
    const int lenNum  = body["len_num"].toInt(0);
    const int lenDen  = body["len_den"].toInt(1);

    const Fraction tick(beatNum, beatDen);
    const Fraction newLen(lenNum, lenDen);

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(400, QString("no measure at tick %1/%2").arg(beatNum).arg(beatDen));
    }

    if (newLen <= Fraction(0, 1)) {
        return errorResponse(400, "actual_len must be positive");
    }

    score->startCmd(TranslatableString("test", "set measure length"));
    measure->adjustToLen(newLen, /*appendRestsIfNecessary=*/true);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionInsertBeats(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "InsertBeats";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(500,
                        "insert_beats failed");
}

EditudeTestServer::Reply EditudeTestServer::actionDeleteBeats(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "DeleteBeats";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(500,
                        "delete_beats failed");
}

EditudeTestServer::Reply EditudeTestServer::actionSetScoreMetadata(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    QJsonObject op = body;
    op["type"] = "SetScoreMetadata";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(500,
                        "set_score_metadata failed");
}

// ---------------------------------------------------------------------------
// Tier 1 extended action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionInsertChord(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QJsonArray pitches  = body["pitches"].toArray();
    const QJsonObject durObj  = body["duration"].toObject();
    const track_idx_t track   = static_cast<track_idx_t>(body.value("track").toInt(0));

    if (pitches.isEmpty()) {
        return errorResponse(422,
                             "pitches array required");
    }

    const DurationType dt = ScoreApplicator::parseDurationType(durObj["type"].toString());
    if (dt == DurationType::V_INVALID) {
        return errorResponse(422,
                             "invalid duration type");
    }
    const int dots = durObj["dots"].toInt(0);

    QVector<int> midiPitches;
    for (const QJsonValue& v : pitches) {
        const QJsonObject p = v.toObject();
        const int midi = ScoreApplicator::pitchToMidi(
            p["step"].toString(), p["octave"].toInt(), p["accidental"].toString());
        if (midi < 0 || midi > 127) {
            return errorResponse(422,
                                 "invalid pitch in pitches array");
        }
        midiPitches.append(midi);
    }

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        return errorResponse(422,
                             "beat not found in score");
    }

    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("test", "insert chord"));
    score->setNoteRest(seg, track, NoteVal(midiPitches[0]), dur.ticks());

    // Locate the chord just created and add remaining pitches.
    Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
    Chord* chord  = nullptr;
    if (seg2) {
        EngravingItem* el = seg2->element(track);
        if (el && el->type() == ElementType::CHORD) {
            chord = toChord(el);
        }
    }
    if (chord) {
        for (int i = 1; i < midiPitches.size(); ++i) {
            score->addNote(chord, NoteVal(midiPitches[i]));
        }
    }
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionAddChordNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj  = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "chord not found");
    }

    Chord* chord = nullptr;
    if (obj->isChord()) {
        chord = toChord(static_cast<EngravingItem*>(obj));
    } else if (obj->isNote()) {
        chord = toNote(static_cast<EngravingItem*>(obj))->chord();
    }
    if (!chord) {
        return errorResponse(422,
                             "event is not a chord");
    }

    const QJsonObject pitchObj = body["pitch"].toObject();
    const int midi = ScoreApplicator::pitchToMidi(
        pitchObj["step"].toString(), pitchObj["octave"].toInt(),
        pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(422,
                             "invalid pitch");
    }

    score->startCmd(TranslatableString("test", "add chord note"));
    score->addNote(chord, NoteVal(midi));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveChordNote(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj  = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "chord not found");
    }

    Chord* chord = nullptr;
    if (obj->isChord()) {
        chord = toChord(static_cast<EngravingItem*>(obj));
    } else if (obj->isNote()) {
        chord = toNote(static_cast<EngravingItem*>(obj))->chord();
    }
    if (!chord) {
        return errorResponse(422,
                             "event is not a chord");
    }

    const QJsonObject pitchObj = body["pitch"].toObject();
    const int midi = ScoreApplicator::pitchToMidi(
        pitchObj["step"].toString(), pitchObj["octave"].toInt(),
        pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        return errorResponse(422,
                             "invalid pitch");
    }

    Note* target = nullptr;
    for (Note* n : chord->notes()) {
        if (n->pitch() == midi) {
            target = n;
            break;
        }
    }
    if (!target) {
        return errorResponse(404,
                             "pitch not found in chord");
    }

    score->startCmd(TranslatableString("test", "remove chord note"));
    score->deleteItem(target);
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetTie(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj  = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    Note* note = dynamic_cast<Note*>(obj);
    if (!note) {
        return errorResponse(422,
                             "element is not a Note");
    }

    const QJsonValue tieVal = body["tie"];
    const bool wantTie = !tieVal.isNull() && !tieVal.isUndefined()
                         && tieVal.toString() != QStringLiteral("stop");

    score->startCmd(TranslatableString("test", "set tie"));
    if (wantTie) {
        if (!note->tieFor()) {
            // Find the next chord/rest in the same track.
            Note* endNote = nullptr;
            for (Segment* s = note->chord()->segment()->next(SegmentType::ChordRest);
                 s; s = s->next(SegmentType::ChordRest)) {
                EngravingItem* el = s->element(note->track());
                if (!el || !el->isChord()) {
                    break;
                }
                Chord* nextChord = toChord(el);
                for (Note* n : nextChord->notes()) {
                    if (n->pitch() == note->pitch()) {
                        endNote = n;
                        break;
                    }
                }
                if (endNote) break;
            }
            Tie* tie = Factory::createTie(note);
            tie->setStartNote(note);
            tie->setTrack(note->track());
            tie->setTick(note->chord()->segment()->tick());
            if (endNote) {
                if (endNote->tieBack()) {
                    score->undoRemoveElement(endNote->tieBack());
                }
                tie->setEndNote(endNote);
                tie->setTicks(endNote->chord()->segment()->tick()
                              - note->chord()->segment()->tick());
            }
            score->undoAddElement(tie);
        }
    } else {
        if (note->tieFor()) {
            score->undoRemoveElement(note->tieFor());
        }
    }
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetTrack(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString eventId = body["event_id"].toString();
    EngravingObject* obj  = findByUuid(eventId);
    if (!obj) {
        return errorResponse(404, "event not found");
    }

    auto* item = dynamic_cast<EngravingItem*>(obj);
    if (!item) {
        return errorResponse(422,
                             "element is not an EngravingItem");
    }

    const track_idx_t newTrack = static_cast<track_idx_t>(body["track"].toInt(0));
    score->startCmd(TranslatableString("test", "set track"));
    item->undoChangeProperty(Pid::TRACK, newTrack);
    score->endCmd();
    return okResponse();
}

// ---------------------------------------------------------------------------
// Tier 2 — score directive action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionSetTimeSignature(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }
    QJsonObject op = body;
    op["type"] = "SetTimeSignature";
    ScoreApplicator applicator;
    return applicator.apply(score, op)
        ? okResponse()
        : errorResponse(500,
                        "set_time_signature failed");
}

EditudeTestServer::Reply EditudeTestServer::actionSetTempo(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QJsonObject tempoObj = body["tempo"].toObject();
    const double bpm = tempoObj["bpm"].toDouble(0.0);
    if (bpm <= 0.0) {
        return errorResponse(422,
                             "invalid bpm");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422,
                             "beat not found in score");
    }
    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("test", "set tempo"));
    TempoText* tt = Factory::createTempoText(seg);
    tt->setTempo(BeatsPerSecond(bpm / 60.0));
    tt->setParent(seg);
    tt->setTrack(0);
    score->undoAddElement(tt);
    score->endCmd();

    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetKeySignature(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    const QJsonObject ksSigObj = body["key_signature"].toObject();
    const int sharps = ksSigObj["sharps"].toInt(0);
    if (sharps < -7 || sharps > 7) {
        return errorResponse(422,
                             "sharps out of range (-7..7)");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422,
                             "beat not found in score");
    }

    const staff_idx_t firstStaff = part->startTrack() / VOICES;
    const staff_idx_t nStaves    = static_cast<staff_idx_t>(part->nstaves());
    const Key key = static_cast<Key>(sharps);

    score->startCmd(TranslatableString("test", "set key signature"));
    Segment* seg = measure->undoGetSegment(SegmentType::KeySig, tick);
    for (staff_idx_t i = 0; i < nStaves; ++i) {
        const track_idx_t track = (firstStaff + i) * VOICES;
        KeySig* ks = Factory::createKeySig(seg);
        ks->setTrack(track);
        ks->setKey(key);
        ks->setParent(seg);
        score->undoAddElement(ks);
    }
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetClef(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const int partIndex = body.value("part_index").toInt(-1);
    if (partIndex < 0 || partIndex >= static_cast<int>(score->parts().size())) {
        return errorResponse(422,
                             "part_index out of range");
    }
    Part* part = score->parts().at(static_cast<size_t>(partIndex));

    const QJsonObject beatObj  = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const int staffIdx         = body.value("staff").toInt(0);
    const QJsonObject clefObj  = body["clef"].toObject();
    const QString clefName     = clefObj["name"].toString();

    static const QHash<QString, ClefType> s_clefMap = {
        { "treble",         ClefType::G      },
        { "treble_8vb",     ClefType::G8_VB  },
        { "treble_8va",     ClefType::G8_VA  },
        { "treble_15mb",    ClefType::G15_MB },
        { "treble_15ma",    ClefType::G15_MA },
        { "bass",           ClefType::F      },
        { "bass_8vb",       ClefType::F8_VB  },
        { "bass_8va",       ClefType::F_8VA  },
        { "bass_15mb",      ClefType::F15_MB },
        { "bass_15ma",      ClefType::F_15MA },
        { "alto",           ClefType::C3     },
        { "tenor",          ClefType::C4     },
        { "mezzo_soprano",  ClefType::C2     },
        { "soprano",        ClefType::C1     },
        { "baritone",       ClefType::C5     },
        { "percussion",     ClefType::PERC   },
        { "tab",            ClefType::TAB    },
        { "tab4",           ClefType::TAB4   },
    };
    const ClefType ct = s_clefMap.value(clefName, ClefType::INVALID);
    if (ct == ClefType::INVALID) {
        return errorResponse(422,
                             "unknown clef name");
    }

    if (staffIdx < 0 || staffIdx >= static_cast<int>(part->nstaves())) {
        return errorResponse(422,
                             "staff index out of range");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422,
                             "beat not found in score");
    }

    const track_idx_t track = (part->startTrack() / VOICES + staffIdx) * VOICES;
    Segment* seg = measure->undoGetSegment(SegmentType::Clef, tick);

    score->startCmd(TranslatableString("test", "set clef"));
    Clef* clef = Factory::createClef(score->dummy()->segment());
    clef->setClefType(ct);
    clef->setTrack(track);
    clef->setParent(seg);
    score->doUndoAddElement(clef);
    score->endCmd();
    return okResponse();
}

// ---------------------------------------------------------------------------
// Tier 3 — chord symbol action handlers
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::actionAddChordSymbol(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QJsonObject beatObj = body["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());
    const QString name = body["name"].toString();
    if (name.isEmpty()) {
        return errorResponse(422,
                             "name required");
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        return errorResponse(422,
                             "beat not found in score");
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("test", "add chord symbol"));
    Harmony* harmony = Factory::createHarmony(seg);
    harmony->setTrack(0);
    harmony->setParent(seg);
    harmony->setHarmonyType(HarmonyType::STANDARD);
    harmony->setHarmony(String(name));
    score->undoAddElement(harmony);
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionSetChordSymbol(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404,
                             "chord symbol not found");
    }

    Harmony* harmony = dynamic_cast<Harmony*>(obj);
    if (!harmony) {
        return errorResponse(422,
                             "element is not a Harmony");
    }

    const QString name = body["name"].toString();
    score->startCmd(TranslatableString("test", "set chord symbol"));
    harmony->undoChangeProperty(Pid::TEXT, PropertyValue(String(name)));
    score->endCmd();
    return okResponse();
}

EditudeTestServer::Reply EditudeTestServer::actionRemoveChordSymbol(const QJsonObject& body)
{
    Score* score = m_svc->scoreForTest();
    if (!score) {
        return errorResponse(503,
                             "score not ready");
    }

    const QString id = body["id"].toString();
    EngravingObject* obj = findByUuid(id);
    if (!obj) {
        return errorResponse(404,
                             "chord symbol not found");
    }

    Harmony* harmony = dynamic_cast<Harmony*>(obj);
    if (!harmony) {
        return errorResponse(422,
                             "element is not a Harmony");
    }

    score->startCmd(TranslatableString("test", "remove chord symbol"));
    score->undoRemoveElement(harmony);
    score->endCmd();
    return okResponse();
}

// ---------------------------------------------------------------------------
// New serialization helpers
// ---------------------------------------------------------------------------

QJsonArray EditudeTestServer::serializeMetricGrid()
{
    Score* score = m_svc->scoreForTest();
    QJsonArray result;
    // One entry per beat position; sample only track 0 to avoid duplicates.
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::TimeSig); seg;
             seg = seg->next(SegmentType::TimeSig)) {
            EngravingItem* el = seg->element(0);
            if (!el || !el->isTimeSig()) {
                continue;
            }
            TimeSig* ts = toTimeSig(el);
            const Fraction sig = ts->sig();
            result.append(QJsonObject{
                { "beat",           beatJson(seg->tick()) },
                { "time_signature", QJsonObject{
                    { "numerator",   sig.numerator()   },
                    { "denominator", sig.denominator() },
                }},
            });
        }
    }
    return result;
}

QJsonArray EditudeTestServer::serializeTempoMap()
{
    Score* score = m_svc->scoreForTest();
    QJsonArray result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || el->type() != ElementType::TEMPO_TEXT) {
                    continue;
                }
                TempoText* tt = static_cast<TempoText*>(el);
                const double bpm = tt->tempo().toBPM().val;
                result.append(QJsonObject{
                    { "beat",  beatJson(seg->tick()) },
                    { "tempo", QJsonObject{
                        { "bpm",      bpm },
                        { "referent", QJsonObject{
                            { "type", QStringLiteral("quarter") },
                            { "dots", 0 },
                        }},
                        { "text", QJsonValue::Null },
                    }},
                });
            }
        }
    }
    return result;
}

QJsonArray EditudeTestServer::serializePartKeyChanges(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonArray result;
    const track_idx_t startTrack = part->startTrack();
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::KeySig); seg;
             seg = seg->next(SegmentType::KeySig)) {
            EngravingItem* el = seg->element(startTrack);
            if (!el || !el->isKeySig()) {
                continue;
            }
            KeySig* ks = toKeySig(el);
            result.append(QJsonObject{
                { "beat",          beatJson(seg->tick()) },
                { "key_signature", QJsonObject{
                    { "sharps", static_cast<int>(ks->key()) },
                }},
            });
        }
    }
    return result;
}

QJsonArray EditudeTestServer::serializePartClefChanges(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonArray result;
    const staff_idx_t firstStaff = part->startTrack() / VOICES;
    const staff_idx_t nStaves    = static_cast<staff_idx_t>(part->nstaves());

    static const QHash<ClefType, QString> s_clefNames = {
        { ClefType::G,       QStringLiteral("treble")       },
        { ClefType::G8_VB,   QStringLiteral("treble_8vb")   },
        { ClefType::G8_VA,   QStringLiteral("treble_8va")   },
        { ClefType::G15_MB,  QStringLiteral("treble_15mb")  },
        { ClefType::G15_MA,  QStringLiteral("treble_15ma")  },
        { ClefType::F,       QStringLiteral("bass")         },
        { ClefType::F8_VB,   QStringLiteral("bass_8vb")     },
        { ClefType::F_8VA,   QStringLiteral("bass_8va")     },
        { ClefType::F15_MB,  QStringLiteral("bass_15mb")    },
        { ClefType::F_15MA,  QStringLiteral("bass_15ma")    },
        { ClefType::C3,      QStringLiteral("alto")         },
        { ClefType::C4,      QStringLiteral("tenor")        },
        { ClefType::C2,      QStringLiteral("mezzo_soprano") },
        { ClefType::C1,      QStringLiteral("soprano")      },
        { ClefType::C5,      QStringLiteral("baritone")     },
        { ClefType::PERC,    QStringLiteral("percussion")   },
        { ClefType::TAB,     QStringLiteral("tab")          },
        { ClefType::TAB4,    QStringLiteral("tab4")         },
    };

    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(SegmentType::Clef); seg;
             seg = seg->next(SegmentType::Clef)) {
            for (staff_idx_t i = 0; i < nStaves; ++i) {
                const track_idx_t track = (firstStaff + i) * VOICES;
                EngravingItem* el = seg->element(track);
                if (!el || !el->isClef()) {
                    continue;
                }
                Clef* clef = toClef(el);
                const QString clefName = s_clefNames.value(
                    clef->clefType(), QStringLiteral("treble"));
                result.append(QJsonObject{
                    { "beat",  beatJson(seg->tick()) },
                    { "clef",  QJsonObject{ { "name", clefName } } },
                    { "staff", static_cast<int>(i) },
                });
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializeScoreChordSymbols()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || el->type() != ElementType::HARMONY) {
                    continue;
                }
                Harmony* harmony = static_cast<Harmony*>(el);
                const QString uuid = uuidForElement(harmony);
                if (uuid.isEmpty()) {
                    continue;
                }
                result[uuid] = QJsonObject{
                    { "id",   uuid },
                    { "beat", beatJson(seg->tick()) },
                    { "name", harmony->harmonyName().toQString() },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializePartStaffTexts(Part* part)
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || el->type() != ElementType::STAFF_TEXT) {
                    continue;
                }
                if (el->track() < part->startTrack() || el->track() >= part->endTrack()) {
                    continue;
                }
                StaffText* st = static_cast<StaffText*>(el);
                const QString uuid = uuidForElement(st);
                if (uuid.isEmpty()) {
                    continue;
                }
                result[uuid] = QJsonObject{
                    { "id",   uuid },
                    { "beat", beatJson(seg->tick()) },
                    { "text", st->plainText().toQString() },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializeScoreSystemTexts()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || el->type() != ElementType::SYSTEM_TEXT) {
                    continue;
                }
                SystemText* st = static_cast<SystemText*>(el);
                const QString uuid = uuidForElement(st);
                if (uuid.isEmpty()) {
                    continue;
                }
                result[uuid] = QJsonObject{
                    { "id",   uuid },
                    { "beat", beatJson(seg->tick()) },
                    { "text", st->plainText().toQString() },
                };
            }
        }
    }
    return result;
}

QJsonObject EditudeTestServer::serializeScoreRehearsalMarks()
{
    Score* score = m_svc->scoreForTest();
    QJsonObject result;
    for (Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        for (Segment* seg = m->first(); seg; seg = seg->next()) {
            for (EngravingItem* el : seg->annotations()) {
                if (!el || el->type() != ElementType::REHEARSAL_MARK) {
                    continue;
                }
                RehearsalMark* rm = static_cast<RehearsalMark*>(el);
                const QString uuid = uuidForElement(rm);
                if (uuid.isEmpty()) {
                    continue;
                }
                result[uuid] = QJsonObject{
                    { "id",   uuid },
                    { "beat", beatJson(seg->tick()) },
                    { "text", rm->plainText().toQString() },
                };
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helper responses
// ---------------------------------------------------------------------------

EditudeTestServer::Reply EditudeTestServer::errorResponse(int status, const QString& msg)
{
    return { status, QJsonDocument(QJsonObject{ { "error", msg } }).toJson(QJsonDocument::Compact) };
}

EditudeTestServer::Reply EditudeTestServer::okResponse()
{
    return { 200, QJsonDocument(QJsonObject{ { "ok", true } }).toJson(QJsonDocument::Compact) };
}

#endif // MUE_BUILD_EDITUDE_TEST_SERVER
