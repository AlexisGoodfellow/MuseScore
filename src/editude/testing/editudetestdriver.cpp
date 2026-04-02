// SPDX-License-Identifier: GPL-3.0-only

#ifdef MUE_BUILD_EDITUDE_TEST_DRIVER

#include "editudetestdriver.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "global/log.h"

#include "internal/editudeservice.h"

using namespace mu::editude::internal;

EditudeTestDriver::EditudeTestDriver(EditudeService* svc, quint16 port, QObject* parent)
    : QObject(parent)
    , m_actions(svc)
    , m_port(port)
{
}

void EditudeTestDriver::start()
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &EditudeTestDriver::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, m_port)) {
        LOGW() << "[EditudeTestDriver] failed to listen on port" << m_port;
    } else {
        LOGD() << "[EditudeTestDriver] listening on port" << m_port;
    }
}

void EditudeTestDriver::onNewConnection()
{
    QTcpSocket* socket = m_server->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_buffers.remove(socket);
        socket->deleteLater();
    });
}

void EditudeTestDriver::onReadyRead(QTcpSocket* socket)
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
    EditudeTestActions::Reply reply;
    if      (method == "GET"  && path == "/health")        reply = m_actions.health();
    else if (method == "GET"  && path == "/score") {
        // Flush any pending deferred applies so the score DOM is consistent.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QJsonObject scoreJson = m_actions.serializeScore();
        reply = { 200, QJsonDocument(scoreJson).toJson(QJsonDocument::Compact) };
    }
    else if (method == "GET"  && path == "/status")        reply = m_actions.status();
    else if (method == "POST" && path == "/wait_revision") {
        const int minRevision = bodyObj.value("min_revision").toInt();
        const int timeoutMs   = bodyObj.value("timeout_ms").toInt(5000);
        reply = m_actions.waitRevision(minRevision, timeoutMs);
    }
    else if (method == "POST" && path == "/action") {
        // Defer score mutation to the next event-loop iteration.  Action
        // handlers call startCmd -> endCmd -> doLayoutRange.  Running that
        // layout pass inside QTcpSocket::readyRead corrupts score-DOM
        // pointers because Qt's scene-graph still holds live references
        // to the pre-edit layout data.
        QEventLoop loop;
        QTimer::singleShot(0, this, [&]() {
            reply = m_actions.dispatchAction(bodyObj);
            // The test driver calls Score::startCmd/endCmd directly,
            // bypassing NotationInteraction.  MuseScore's paint view
            // only redraws in response to notationChanged() — which is
            // fired by NotationInteraction, not by Score::endCmd().
            // Without this explicit notification the paint view never
            // learns about the mutation and the score doesn't visually
            // update until the user scrolls.
            m_actions.notifyPaintView();
            loop.quit();
        });
        loop.exec();
        // Flush pending events so WebSocket data is written before the
        // HTTP response reaches the Python harness.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    else if (method == "POST" && path == "/connect")       reply = m_actions.connect(bodyObj);
    else                                                   reply = m_actions.errorResponse(404, "not found");

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

#endif // MUE_BUILD_EDITUDE_TEST_DRIVER
