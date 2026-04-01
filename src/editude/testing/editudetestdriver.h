// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef MUE_BUILD_EDITUDE_TEST_DRIVER

#include <QHash>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "editudetestactions.h"

namespace mu::editude::internal {
class EditudeService;

/// HTTP wrapper around EditudeTestActions.  Listens on a local TCP port
/// and translates incoming HTTP requests to EditudeTestActions calls.
/// Only compiled into test builds (MUE_BUILD_EDITUDE_TEST_DRIVER).
class EditudeTestDriver : public QObject
{
    Q_OBJECT

public:
    explicit EditudeTestDriver(EditudeService* svc, quint16 port, QObject* parent = nullptr);
    void start();

private slots:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);

private:
    EditudeTestActions m_actions;
    quint16 m_port;
    QTcpServer* m_server = nullptr;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

} // namespace mu::editude::internal

#endif // MUE_BUILD_EDITUDE_TEST_DRIVER
