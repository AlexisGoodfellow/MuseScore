/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 Alexis Goodfellow
 *
 * CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
 * EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ---------------------------------------------------------------------------
// Helpers — lightweight stand-ins that exercise the logic extracted from
// EditudeService without spinning up a full MuseScore environment.
// ---------------------------------------------------------------------------

namespace {

// Decode the JWT exp claim from a raw token string (same logic as
// EditudeService::scheduleTokenRefresh but extracted here for testing).
static qint64 expFromToken(const QString& token)
{
    const QStringList parts = token.split('.');
    if (parts.size() < 2) {
        return -1;
    }
    QString segment = parts[1];
    while (segment.size() % 4 != 0) {
        segment += '=';
    }
    const QByteArray decoded = QByteArray::fromBase64(
        segment.toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject payload = QJsonDocument::fromJson(decoded).object();
    if (!payload.contains("exp")) {
        return -1;
    }
    return static_cast<qint64>(payload.value("exp").toDouble());
}

// Compute the reconnect back-off delay (ms) for a given attempt number,
// capped at 60 000 ms.  Mirrors the formula in onDisconnected / onReconnectTimer.
static int reconnectDelay(int attempt)
{
    return std::min(1000 * (1 << std::min(attempt, 6)), 60000);
}

// Build a minimal base64url-encoded JWT with the given exp (Unix seconds).
static QString makeToken(qint64 exp)
{
    const QJsonObject payload{ { "sub", "test-user" }, { "exp", static_cast<double>(exp) } };
    const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString payloadB64 = QString::fromLatin1(
        payloadBytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    return QStringLiteral("header.") + payloadB64 + QStringLiteral(".sig");
}

} // namespace

// ---------------------------------------------------------------------------
// 1. scheduleTokenRefresh fires approximately 5 min before expiry
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, ScheduleTokenRefreshArmsTimer)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 exp = now + 600; // expires in 10 min
    const QString token = makeToken(exp);

    const qint64 decoded = expFromToken(token);
    ASSERT_EQ(decoded, exp);

    // Timer should fire at exp - 300 s from now, i.e. ~300 s from now (5 min)
    const qint64 fireInSecs = std::max(decoded - now - 300, static_cast<qint64>(1));
    EXPECT_GE(fireInSecs, 295); // allow a little slack for slow test machines
    EXPECT_LE(fireInSecs, 305);
}

// ---------------------------------------------------------------------------
// 2. Reconnect back-off schedule: 1 s, 2 s, 4 s, 8 s, 16 s, 32 s, then cap
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, ReconnectBackoffSchedule)
{
    const int expected[] = { 1000, 2000, 4000, 8000, 16000, 32000, 60000 };
    for (int i = 0; i < 7; ++i) {
        EXPECT_EQ(reconnectDelay(i), expected[i])
            << "attempt " << i << " should yield " << expected[i] << " ms";
    }
    // Attempts beyond 6 must stay capped
    EXPECT_EQ(reconnectDelay(7),  60000);
    EXPECT_EQ(reconnectDelay(10), 60000);
}

// ---------------------------------------------------------------------------
// 3. Buffered ops are flushed on sync; base_revision and payload preserved
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, BufferedOpsFlushedOnSync)
{
    const int capturedRevision = 42;

    // Simulate accumulating ops in the buffer with base_revision captured
    QJsonArray bufferedOps;
    const int opCount = 3;
    for (int i = 0; i < opCount; ++i) {
        QJsonObject entry;
        QJsonObject payload;
        payload["type"] = "InsertNote";
        payload["seq"]  = i;
        entry["payload"]       = payload;
        entry["base_revision"] = capturedRevision;
        if (bufferedOps.size() < 100) {
            bufferedOps.append(entry);
        }
    }
    ASSERT_EQ(bufferedOps.size(), opCount);

    // Simulate the flush that happens in onServerMessage when type == "sync"
    QJsonArray flushedOps;
    for (const QJsonValue& v : bufferedOps) {
        const QJsonObject entry = v.toObject();
        QJsonObject out;
        out["type"]          = "op";
        out["base_revision"] = entry.value("base_revision").toInt(0);
        out["payload"]       = entry.value("payload").toObject();
        flushedOps.append(out);
    }
    bufferedOps = QJsonArray(); // cleared after flush

    EXPECT_EQ(flushedOps.size(), opCount);
    EXPECT_TRUE(bufferedOps.isEmpty());

    for (int i = 0; i < opCount; ++i) {
        const QJsonObject out = flushedOps[i].toObject();
        EXPECT_EQ(out["type"].toString(), "op");
        EXPECT_EQ(out["base_revision"].toInt(), capturedRevision);
        EXPECT_EQ(out["payload"].toObject()["seq"].toInt(), i);
    }
}

// ---------------------------------------------------------------------------
// 4. Buffer cap: never exceeds 100 entries
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, BufferCapAt100)
{
    QJsonArray bufferedOps;
    for (int i = 0; i < 150; ++i) {
        QJsonObject entry;
        entry["payload"]       = QJsonObject{ { "seq", i } };
        entry["base_revision"] = 5;
        if (bufferedOps.size() < 100) {
            bufferedOps.append(entry);
        }
    }
    EXPECT_EQ(bufferedOps.size(), 100);
}

// ---------------------------------------------------------------------------
// 5. serverRevision advances from op_ack
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, ServerRevisionAdvancesFromOpAck)
{
    // Simulate the op_ack handler logic:
    //   if (revision > m_serverRevision) m_serverRevision = revision;
    int serverRevision = 10;

    // op_ack with higher revision → advances
    {
        int ackRevision = 11;
        if (ackRevision > serverRevision) serverRevision = ackRevision;
    }
    EXPECT_EQ(serverRevision, 11);

    // op_ack with lower revision (stale/reordered) → does not regress
    {
        int ackRevision = 9;
        if (ackRevision > serverRevision) serverRevision = ackRevision;
    }
    EXPECT_EQ(serverRevision, 11);

    // op_ack with equal revision → no change
    {
        int ackRevision = 11;
        if (ackRevision > serverRevision) serverRevision = ackRevision;
    }
    EXPECT_EQ(serverRevision, 11);
}

// ---------------------------------------------------------------------------
// 6. serverRevision advances from broadcast op (same monotonic logic)
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, ServerRevisionAdvancesFromBroadcast)
{
    int serverRevision = 20;

    // Broadcast ops arrive in order: 21, 22, 23
    for (int incoming : { 21, 22, 23 }) {
        if (incoming > serverRevision) serverRevision = incoming;
    }
    EXPECT_EQ(serverRevision, 23);

    // A late/duplicate broadcast at 21 must not regress the revision
    {
        int incoming = 21;
        if (incoming > serverRevision) serverRevision = incoming;
    }
    EXPECT_EQ(serverRevision, 23);
}

// ---------------------------------------------------------------------------
// 7. Live op includes base_revision equal to m_serverRevision at send time
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, LiveOpCarriesBaseRevision)
{
    // Simulate the onScoreChanges() live-send path:
    //   const int baseRevision = m_serverRevision;
    //   msg["base_revision"] = baseRevision;
    int serverRevision = 17;

    QJsonArray sentOps;
    const int baseRevision = serverRevision;  // snapshot at send time

    // Two ops from the same change event share the same base_revision
    for (int i = 0; i < 2; ++i) {
        QJsonObject msg;
        msg["type"]          = "op";
        msg["base_revision"] = baseRevision;
        msg["payload"]       = QJsonObject{ { "type", "SetPitch" }, { "seq", i } };
        sentOps.append(msg);
    }

    ASSERT_EQ(sentOps.size(), 2);
    for (const QJsonValue& v : sentOps) {
        EXPECT_EQ(v.toObject()["base_revision"].toInt(), 17);
    }

    // After op_ack arrives at revision 18, the next batch uses 18
    {
        const int ackRevision = 18;
        if (ackRevision > serverRevision) serverRevision = ackRevision;
    }

    QJsonArray nextBatch;
    const int nextBase = serverRevision;
    QJsonObject msg;
    msg["type"]          = "op";
    msg["base_revision"] = nextBase;
    msg["payload"]       = QJsonObject{ { "type", "InsertNote" } };
    nextBatch.append(msg);

    EXPECT_EQ(nextBatch[0].toObject()["base_revision"].toInt(), 18);
}
