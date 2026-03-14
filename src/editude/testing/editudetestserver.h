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
    QString uuidForElement(mu::engraving::EngravingObject* obj) const;
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
