
# [editude] _load, _startAudioProcessing, _addSoundFont are defined in webapi_export.cpp
# with EMSCRIPTEN_KEEPALIVE, but also listed here for explicitness.
# _editudeTest* functions are defined in editudetestbridge.cpp (WASM test bridge).
set(EXPORTED_FUNCTIONS "SHELL:-s EXPORTED_FUNCTIONS=[_main,_malloc,_free,_load,_startAudioProcessing,_addSoundFont,_editudeTestAction,_editudeTestScore,_editudeTestHealth,_editudeTestStatus,_editudeTestConnect,_editudeTestGetRevision,_editudeSetAudioOutputLatency]")
# [/editude]
