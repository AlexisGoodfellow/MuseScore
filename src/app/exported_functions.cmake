
# [editure] Removed _load, _startAudioProcessing, _addSoundFont — these are from
# webapi_export.cpp which is excluded in editude WASM builds.
set(EXPORTED_FUNCTIONS "SHELL:-s EXPORTED_FUNCTIONS=[_main,_malloc,_free]")
# [/editure]
