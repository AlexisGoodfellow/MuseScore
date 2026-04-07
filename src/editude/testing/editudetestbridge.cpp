// SPDX-License-Identifier: GPL-3.0-only

#ifdef Q_OS_WASM

#include "editudetestbridge.h"
#include "editudetestactions.h"

#include <emscripten.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "global/log.h"
#include "internal/editudeservice.h"

// ---------------------------------------------------------------------------
// Module-level singleton — set once by initTestBridge(), used by all
// exported C functions below.
// ---------------------------------------------------------------------------

static mu::editude::internal::EditudeTestActions* s_actions = nullptr;

namespace mu::editude::internal {

void initTestBridge(EditudeService* svc)
{
    static EditudeTestActions actions(svc);
    s_actions = &actions;

    // Register window.editudeTest on the JS global scope.  The Swift
    // TestDriverServer calls these methods via evaluateJavaScript().
    // Each method is a thin JS wrapper around Module.ccall().
    //
    // Uses emscripten_run_script() instead of EM_ASM because the JS code
    // contains commas that the C preprocessor misinterprets as macro
    // argument separators.
    emscripten_run_script(R"JS(
        window.editudeTest = {
            action: function(jsonString) {
                var result = Module.ccall(
                    'editudeTestAction', 'string', ['string'], [jsonString]);
                return result;
            },
            score: function() {
                return Module.ccall('editudeTestScore', 'string', [], []);
            },
            health: function() {
                return Module.ccall('editudeTestHealth', 'string', [], []);
            },
            status: function() {
                return Module.ccall('editudeTestStatus', 'string', [], []);
            },
            connect: function(jsonString) {
                return Module.ccall(
                    'editudeTestConnect', 'string', ['string'], [jsonString]);
            },
            getRevision: function() {
                return Module.ccall('editudeTestGetRevision', 'number', [], []);
            },
            waitRevision: function(minRev, timeoutMs) {
                return new Promise(function(resolve, reject) {
                    var deadline = Date.now() + (timeoutMs || 5000);
                    function poll() {
                        var rev = window.editudeTest.getRevision();
                        if (rev >= minRev) {
                            resolve(JSON.stringify({revision: rev}));
                        } else if (Date.now() > deadline) {
                            reject(JSON.stringify(
                                {error: "timeout", revision: rev}));
                        } else {
                            setTimeout(poll, 20);
                        }
                    }
                    poll();
                });
            }
        };
        console.log("[EditudeTestBridge] window.editudeTest registered");
    )JS");

    LOGI() << "[EditudeTestBridge] initialised";
}

} // namespace mu::editude::internal

// ---------------------------------------------------------------------------
// Exported C functions — called by window.editudeTest via Module.ccall()
// ---------------------------------------------------------------------------

extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* editudeTestAction(const char* jsonStr)
{
    if (!s_actions) return "{}";
    QJsonObject body = QJsonDocument::fromJson(QByteArray(jsonStr)).object();
    // Defer score mutation to the next event-loop iteration, matching the
    // native test driver's QTimer::singleShot pattern.  Without this, a
    // second action (e.g. set_key_signature) can run before the translator
    // has processed the previous action's changesChannel notification
    // (e.g. add_part), so m_knownPartUuids is stale.
    mu::editude::internal::EditudeTestActions::Reply reply;
    QEventLoop loop;
    QTimer::singleShot(0, [&]() {
        reply = s_actions->dispatchAction(body);
        // The WASM test bridge calls Score::startCmd/endCmd directly, bypassing
        // NotationInteraction.  The paint view only redraws in response to
        // notationChanged() — fire it explicitly (matches native test driver).
        s_actions->notifyPaintView();
        loop.quit();
    });
    loop.exec();
    // Process events so that WebSocket data from changesChannel is flushed.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    static QByteArray buf;
    buf = reply.body;
    return buf.constData();
}

EMSCRIPTEN_KEEPALIVE
const char* editudeTestScore()
{
    if (!s_actions) return "{}";
    // Flush pending deferred applies before serialising.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QJsonObject scoreJson = s_actions->serializeScore();
    static QByteArray buf;
    buf = QJsonDocument(scoreJson).toJson(QJsonDocument::Compact);
    return buf.constData();
}

EMSCRIPTEN_KEEPALIVE
const char* editudeTestHealth()
{
    if (!s_actions) return "{\"error\":\"not initialised\"}";
    auto reply = s_actions->health();
    static QByteArray buf;
    buf = reply.body;
    return buf.constData();
}

EMSCRIPTEN_KEEPALIVE
const char* editudeTestStatus()
{
    if (!s_actions) return "{}";
    auto reply = s_actions->status();
    static QByteArray buf;
    buf = reply.body;
    return buf.constData();
}

EMSCRIPTEN_KEEPALIVE
const char* editudeTestConnect(const char* jsonStr)
{
    if (!s_actions) return "{}";
    QJsonObject body = QJsonDocument::fromJson(QByteArray(jsonStr)).object();
    auto reply = s_actions->connect(body);
    static QByteArray buf;
    buf = reply.body;
    return buf.constData();
}

EMSCRIPTEN_KEEPALIVE
int editudeTestGetRevision()
{
    if (!s_actions) return -1;
    return s_actions->serverRevision();
}

} // extern "C"

#endif // Q_OS_WASM
