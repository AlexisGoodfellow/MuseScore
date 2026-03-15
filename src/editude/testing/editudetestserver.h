// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

#include <QObject>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QHttpServerRequest>
#include <QJsonArray>
#include <QJsonObject>

#include "engraving/dom/score.h"
#include "engraving/dom/part.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/engravingobject.h"

namespace mu::editude::internal {
class EditudeService;

class EditudeTestServer : public QObject
{
    Q_OBJECT
public:
    explicit EditudeTestServer(EditudeService* svc, quint16 port, QObject* parent = nullptr);
    void start();

private:
    QHttpServerResponse handleHealth();
    QHttpServerResponse handleScore();
    QHttpServerResponse handleWaitRevision(const QHttpServerRequest& req);
    QHttpServerResponse handleAction(const QHttpServerRequest& req);
    QHttpServerResponse handleConnect(const QHttpServerRequest& req);
    QHttpServerResponse handleStatus();

    QHttpServerResponse actionInsertNote(const QJsonObject& body);
    QHttpServerResponse actionInsertRest(const QJsonObject& body);
    QHttpServerResponse actionDeleteEvent(const QJsonObject& body);
    QHttpServerResponse actionSetPitch(const QJsonObject& body);
    QHttpServerResponse actionUndo();

    // Tier 1 — extended ops
    QHttpServerResponse actionInsertChord(const QJsonObject& body);
    QHttpServerResponse actionAddChordNote(const QJsonObject& body);
    QHttpServerResponse actionRemoveChordNote(const QJsonObject& body);
    QHttpServerResponse actionSetTie(const QJsonObject& body);
    QHttpServerResponse actionSetTrack(const QJsonObject& body);

    // Tier 2 — score directives
    QHttpServerResponse actionSetTimeSignature(const QJsonObject& body);
    QHttpServerResponse actionSetTempo(const QJsonObject& body);
    QHttpServerResponse actionSetKeySignature(const QJsonObject& body);
    QHttpServerResponse actionSetClef(const QJsonObject& body);

    // Phase 1 — Part/Staff actions
    QHttpServerResponse actionAddPart(const QJsonObject& body);
    QHttpServerResponse actionRemovePart(const QJsonObject& body);
    QHttpServerResponse actionSetPartName(const QJsonObject& body);
    QHttpServerResponse actionSetStaffCount(const QJsonObject& body);
    QHttpServerResponse actionSetPartInstrument(const QJsonObject& body);

    // Tier 3 — chord symbols
    QHttpServerResponse actionAddChordSymbol(const QJsonObject& body);
    QHttpServerResponse actionSetChordSymbol(const QJsonObject& body);
    QHttpServerResponse actionRemoveChordSymbol(const QJsonObject& body);

    // Tier 3 — articulations
    QHttpServerResponse actionAddArticulation(const QJsonObject& body);
    QHttpServerResponse actionRemoveArticulation(const QJsonObject& body);

    // Tier 3 — dynamics
    QHttpServerResponse actionAddDynamic(const QJsonObject& body);
    QHttpServerResponse actionSetDynamic(const QJsonObject& body);
    QHttpServerResponse actionRemoveDynamic(const QJsonObject& body);

    // Tier 3 — slurs
    QHttpServerResponse actionAddSlur(const QJsonObject& body);
    QHttpServerResponse actionRemoveSlur(const QJsonObject& body);

    // Tier 3 — hairpins
    QHttpServerResponse actionAddHairpin(const QJsonObject& body);
    QHttpServerResponse actionRemoveHairpin(const QJsonObject& body);

    // Tier 3 — lyrics
    QHttpServerResponse actionAddLyric(const QJsonObject& body);
    QHttpServerResponse actionSetLyric(const QJsonObject& body);
    QHttpServerResponse actionRemoveLyric(const QJsonObject& body);

    // Tier 4 — navigation marks
    QHttpServerResponse actionInsertVolta(const QJsonObject& body);
    QHttpServerResponse actionRemoveVolta(const QJsonObject& body);
    QHttpServerResponse actionInsertMarker(const QJsonObject& body);
    QHttpServerResponse actionRemoveMarker(const QJsonObject& body);
    QHttpServerResponse actionInsertJump(const QJsonObject& body);
    QHttpServerResponse actionRemoveJump(const QJsonObject& body);

    // Structural ops
    QHttpServerResponse actionInsertBeats(const QJsonObject& body);
    QHttpServerResponse actionDeleteBeats(const QJsonObject& body);

    // Metadata
    QHttpServerResponse actionSetScoreMetadata(const QJsonObject& body);

    // Serialization helpers
    QJsonObject serializeScore();
    QJsonObject serializePart(mu::engraving::Part* part);
    QJsonArray serializePartEvents(mu::engraving::Part* part);
    QJsonObject serializeNote(mu::engraving::Note* note,
                              const QString& uuid,
                              const mu::engraving::Fraction& tick);
    QJsonObject serializeRest(mu::engraving::Rest* rest,
                              const QString& uuid,
                              const mu::engraving::Fraction& tick);
    QJsonObject serializeChord(mu::engraving::Chord* chord,
                               const QString& uuid,
                               const mu::engraving::Fraction& tick);
    QJsonObject serializePartArticulations(mu::engraving::Part* part);
    QJsonObject serializePartDynamics(mu::engraving::Part* part);
    QJsonObject serializePartSlurs(mu::engraving::Part* part);
    QJsonObject serializePartHairpins(mu::engraving::Part* part);
    QJsonObject serializePartLyricsMap(mu::engraving::Part* part);
    QJsonArray  serializePartKeyChanges(mu::engraving::Part* part);
    QJsonArray  serializePartClefChanges(mu::engraving::Part* part);
    QJsonArray  serializeMetricGrid();
    QJsonArray  serializeTempoMap();
    QJsonObject serializeScoreChordSymbols();
    QJsonObject serializeScoreVoltas();
    QJsonObject serializeScoreMarkers();
    QJsonObject serializeScoreJumps();

    QString uuidForElement(mu::engraving::EngravingObject* obj) const;
    QString uuidForChordRest(mu::engraving::EngravingObject* obj) const;
    mu::engraving::EngravingObject* findByUuid(const QString& uuid) const;
    static QJsonObject beatJson(const mu::engraving::Fraction& tick);
    static QString durationTypeName(mu::engraving::DurationType dt);
    static QJsonObject pitchJson(mu::engraving::Note* note);

    QHttpServerResponse errorResponse(QHttpServerResponse::StatusCode code, const QString& msg);
    QHttpServerResponse okResponse();

    EditudeService* m_svc;
    quint16 m_port;
    QHttpServer* m_server = nullptr;
};

} // namespace mu::editude::internal

#endif // MUE_BUILD_EDITUDE_TEST_SERVER
