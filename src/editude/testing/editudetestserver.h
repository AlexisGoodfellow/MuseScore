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
    Reply dispatchAction(const QJsonObject& body);
    Reply handleConnect(const QJsonObject& body);
    Reply handleStatus();

    Reply actionInsertNote(const QJsonObject& body);
    Reply actionInsertRest(const QJsonObject& body);
    Reply actionDeleteNote(const QJsonObject& body);
    Reply actionDeleteRest(const QJsonObject& body);
    Reply actionSetPitch(const QJsonObject& body);
    Reply actionUndo();

    // Tier 1 — extended ops
    Reply actionSetTie(const QJsonObject& body);
    Reply actionSetVoice(const QJsonObject& body);

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
    Reply actionSetStringData(const QJsonObject& body);
    Reply actionSetCapo(const QJsonObject& body);
    Reply actionSetTabNote(const QJsonObject& body);
    Reply actionSetDrumset(const QJsonObject& body);
    Reply actionSetNoteHead(const QJsonObject& body);

    // Tier 3 — chord symbols
    Reply actionAddChordSymbol(const QJsonObject& body);
    Reply actionSetChordSymbol(const QJsonObject& body);
    Reply actionRemoveChordSymbol(const QJsonObject& body);

    // Tier 3 — articulations
    Reply actionAddArticulation(const QJsonObject& body);
    Reply actionRemoveArticulation(const QJsonObject& body);

    // Tier 3 — tuplets
    Reply actionAddTuplet(const QJsonObject& body);
    Reply actionRemoveTuplet(const QJsonObject& body);

    // Tier 3 — arpeggios
    Reply actionAddArpeggio(const QJsonObject& body);
    Reply actionRemoveArpeggio(const QJsonObject& body);

    // Tier 3 — grace notes
    Reply actionAddGraceNote(const QJsonObject& body);
    Reply actionRemoveGraceNote(const QJsonObject& body);

    // Tier 3 — breath marks / caesuras
    Reply actionAddBreathMark(const QJsonObject& body);
    Reply actionRemoveBreathMark(const QJsonObject& body);

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

    // Advanced spanners — octave lines
    Reply actionAddOctaveLine(const QJsonObject& body);
    Reply actionRemoveOctaveLine(const QJsonObject& body);

    // Advanced spanners — glissandos
    Reply actionAddGlissando(const QJsonObject& body);
    Reply actionRemoveGlissando(const QJsonObject& body);

    // Advanced spanners — pedal lines
    Reply actionAddPedalLine(const QJsonObject& body);
    Reply actionRemovePedalLine(const QJsonObject& body);

    // Advanced spanners — trill lines
    Reply actionAddTrillLine(const QJsonObject& body);
    Reply actionRemoveTrillLine(const QJsonObject& body);

    // Tier 3 — lyrics
    Reply actionAddLyric(const QJsonObject& body);
    Reply actionSetLyric(const QJsonObject& body);
    Reply actionRemoveLyric(const QJsonObject& body);

    // Tier 3 — staff text
    Reply actionAddStaffText(const QJsonObject& body);
    Reply actionSetStaffText(const QJsonObject& body);
    Reply actionRemoveStaffText(const QJsonObject& body);

    // Tier 3 — system text
    Reply actionAddSystemText(const QJsonObject& body);
    Reply actionSetSystemText(const QJsonObject& body);
    Reply actionRemoveSystemText(const QJsonObject& body);

    // Tier 3 — rehearsal marks
    Reply actionAddRehearsalMark(const QJsonObject& body);
    Reply actionSetRehearsalMark(const QJsonObject& body);
    Reply actionRemoveRehearsalMark(const QJsonObject& body);

    // Tier 3 — tremolos (single-note)
    Reply actionAddTremolo(const QJsonObject& body);
    Reply actionRemoveTremolo(const QJsonObject& body);

    // Tier 3 — two-note tremolos
    Reply actionAddTwoNoteTremolo(const QJsonObject& body);
    Reply actionRemoveTwoNoteTremolo(const QJsonObject& body);

    // Tier 4 — navigation marks
    Reply actionSetStartRepeat(const QJsonObject& body);
    Reply actionSetEndRepeat(const QJsonObject& body);
    Reply actionInsertVolta(const QJsonObject& body);
    Reply actionRemoveVolta(const QJsonObject& body);
    Reply actionInsertMarker(const QJsonObject& body);
    Reply actionRemoveMarker(const QJsonObject& body);
    Reply actionInsertJump(const QJsonObject& body);
    Reply actionRemoveJump(const QJsonObject& body);

    // Structural ops
    Reply actionSetMeasureLen(const QJsonObject& body);
    Reply actionInsertBeats(const QJsonObject& body);
    Reply actionDeleteBeats(const QJsonObject& body);

    // Metadata
    Reply actionSetScoreMetadata(const QJsonObject& body);

    // Display mode
    Reply actionSetConcertPitch(const QJsonObject& body);

    // Serialization helpers — coordinate-addressed (no UUIDs)
    QJsonObject serializeScore();
    QJsonObject serializePart(mu::engraving::Part* part);
    QJsonArray  serializePartEvents(mu::engraving::Part* part);
    QJsonObject serializeNote(mu::engraving::Note* note,
                              const mu::engraving::Fraction& tick,
                              int voice, int staff);
    QJsonObject serializeRest(mu::engraving::Rest* rest,
                              const mu::engraving::Fraction& tick,
                              int voice, int staff);
    QJsonObject serializePartArticulations(mu::engraving::Part* part);
    QJsonObject serializePartTuplets(mu::engraving::Part* part);
    QJsonObject serializePartArpeggios(mu::engraving::Part* part);
    QJsonObject serializePartGraceNotes(mu::engraving::Part* part);
    QJsonObject serializePartBreaths(mu::engraving::Part* part);
    QJsonObject serializePartTremolos(mu::engraving::Part* part);
    QJsonObject serializePartTwoNoteTremolos(mu::engraving::Part* part);
    QJsonObject serializePartDynamics(mu::engraving::Part* part);
    QJsonObject serializePartSlurs(mu::engraving::Part* part);
    QJsonObject serializePartHairpins(mu::engraving::Part* part);
    QJsonObject serializePartOctaveLines(mu::engraving::Part* part);
    QJsonObject serializePartGlissandos(mu::engraving::Part* part);
    QJsonObject serializePartPedalLines(mu::engraving::Part* part);
    QJsonObject serializePartTrillLines(mu::engraving::Part* part);
    QJsonObject serializePartLyricsMap(mu::engraving::Part* part);
    QJsonArray  serializePartKeyChanges(mu::engraving::Part* part);
    QJsonArray  serializePartClefChanges(mu::engraving::Part* part);
    QJsonArray  serializeMetricGrid();
    QJsonArray  serializeTempoMap();
    QJsonObject serializeScoreChordSymbols();
    QJsonObject serializePartStaffTexts(mu::engraving::Part* part);
    QJsonObject serializeScoreSystemTexts();
    QJsonObject serializeScoreRehearsalMarks();
    QJsonArray  serializeScoreRepeatBarlines();
    QJsonObject serializeScoreVoltas();
    QJsonObject serializeScoreMarkers();
    QJsonObject serializeScoreJumps();
    QJsonArray  serializeMeasureLenOverrides();

    // Part UUID lookup (retained — parts are axes, not points).
    QString uuidForPart(mu::engraving::Part* part) const;
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
