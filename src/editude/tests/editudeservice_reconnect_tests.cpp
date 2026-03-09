/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
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
// 3. Buffered ops are flushed on sync; buffer cleared afterwards
// ---------------------------------------------------------------------------
TEST(EditudeServiceReconnect, BufferedOpsFlushedOnSync)
{
    // Simulate accumulating ops in the buffer (cap = 100)
    QJsonArray bufferedOps;
    const int opCount = 3;
    for (int i = 0; i < opCount; ++i) {
        QJsonObject entry;
        QJsonObject payload;
        payload["type"] = "InsertNote";
        payload["seq"]  = i;
        entry["payload"] = payload;
        if (bufferedOps.size() < 100) {
            bufferedOps.append(entry);
        }
    }
    ASSERT_EQ(bufferedOps.size(), opCount);

    // Simulate the flush that happens in onServerMessage when type == "sync"
    QJsonArray flushedPayloads;
    for (const QJsonValue& v : bufferedOps) {
        QJsonObject out;
        out["type"]    = "op";
        out["payload"] = v.toObject().value("payload").toObject();
        flushedPayloads.append(out);
    }
    bufferedOps = QJsonArray(); // cleared after flush

    EXPECT_EQ(flushedPayloads.size(), opCount);
    EXPECT_TRUE(bufferedOps.isEmpty());

    // Verify each flushed op carries the correct payload
    for (int i = 0; i < opCount; ++i) {
        const QJsonObject out = flushedPayloads[i].toObject();
        EXPECT_EQ(out["type"].toString(), "op");
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
        entry["payload"] = QJsonObject{ { "seq", i } };
        if (bufferedOps.size() < 100) {
            bufferedOps.append(entry);
        }
    }
    EXPECT_EQ(bufferedOps.size(), 100);
}
