
# [editude] _load, _startAudioProcessing, _addSoundFont are defined in webapi_export.cpp
# with EMSCRIPTEN_KEEPALIVE, but also listed here for explicitness.
set(EXPORTED_FUNCTIONS "SHELL:-s EXPORTED_FUNCTIONS=[_main,_malloc,_free,_load,_startAudioProcessing,_addSoundFont]")
# [/editude]
