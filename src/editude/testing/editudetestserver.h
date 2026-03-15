// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef MUE_BUILD_EDITUDE_TEST_SERVER

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"

namespace mu::editude::internal {
class EditudeService;

class EditudeTestServer : public QObject
{
    Q_OBJECT

    // Minimal HTTP reply value — status code + JSON body bytes.
    struct Reply { int status; QByteArray body; };

public:
    explicit EditudeTestServer(EditudeService* svc, quint16 port, QObject* parent = nullptr);
    void start();

private slots:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);

private:
    Reply handleHealth();
    Reply handleScore();
    Reply handleWaitRevision(const QJsonObject& body);
    Reply handleAction(const QJsonObject& body);
    Reply handleConnect(const QJsonObject& body);
    Reply handleStatus();

    Reply actionInsertNote(const QJsonObject& body);
    Reply actionInsertRest(const QJsonObject& body);
    Reply actionDeleteEvent(const QJsonObject& body);
    Reply actionSetPitch(const QJsonObject& body);
    Reply actionUndo();

    // Tier 1 — extended ops
    Reply actionInsertChord(const QJsonObject& body);
    Reply actionAddChordNote(const QJsonObject& body);
    Reply actionRemoveChordNote(const QJsonObject& body);
    Reply actionSetTie(const QJsonObject& body);
    Reply actionSetTrack(const QJsonObject& body);

    // Tier 2 — score directives
    Reply actionSetTimeSignature(const QJsonObject& body);
    Reply actionSetTempo(const QJsonObject& body);
    Reply actionSetKeySignature(const QJsonObject& body);
    Reply actionSetClef(const QJsonObject& body);

    // Part / Staff actions
    Reply actionAddPart(const QJsonObject& body);
    Reply actionRemovePart(const QJsonObject& body);
    Reply actionSetPartName(const QJsonObject& body);
    Reply actionSetStaffCount(const QJsonObject& body);
    Reply actionSetPartInstrument(const QJsonObject& body);

    // Tier 3 — chord symbols
    Reply actionAddChordSymbol(const QJsonObject& body);
    Reply actionSetChordSymbol(const QJsonObject& body);
    Reply actionRemoveChordSymbol(const QJsonObject& body);

    // Tier 3 — articulations
    Reply actionAddArticulation(const QJsonObject& body);
    Reply actionRemoveArticulation(const QJsonObject& body);

    // Tier 3 — dynamics
    Reply actionAddDynamic(const QJsonObject& body);
    Reply actionSetDynamic(const QJsonObject& body);
    Reply actionRemoveDynamic(const QJsonObject& body);

    // Tier 3 — slurs
    Reply actionAddSlur(const QJsonObject& body);
    Reply actionRemoveSlur(const QJsonObject& body);

    // Tier 3 — hairpins
    Reply actionAddHairpin(const QJsonObject& body);
    Reply actionRemoveHairpin(const QJsonObject& body);

    // Tier 3 — lyrics
    Reply actionAddLyric(const QJsonObject& body);
    Reply actionSetLyric(const QJsonObject& body);
    Reply actionRemoveLyric(const QJsonObject& body);

    // Tier 4 — navigation marks
    Reply actionInsertVolta(const QJsonObject& body);
    Reply actionRemoveVolta(const QJsonObject& body);
    Reply actionInsertMarker(const QJsonObject& body);
    Reply actionRemoveMarker(const QJsonObject& body);
    Reply actionInsertJump(const QJsonObject& body);
    Reply actionRemoveJump(const QJsonObject& body);

    // Structural ops
    Reply actionInsertBeats(const QJsonObject& body);
    Reply actionDeleteBeats(const QJsonObject& body);

    // Metadata
    Reply actionSetScoreMetadata(const QJsonObject& body);

    // Serialization helpers
    QJsonObject serializeScore();
    QJsonObject serializePart(mu::engraving::Part* part);
    QJsonArray  serializePartEvents(mu::engraving::Part* part);
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

    Reply errorResponse(int status, const QString& msg);
    Reply okResponse();

    EditudeService* m_svc;
    quint16 m_port;
    QTcpServer* m_server = nullptr;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

} // namespace mu::editude::internal

#endif // MUE_BUILD_EDITUDE_TEST_SERVER
