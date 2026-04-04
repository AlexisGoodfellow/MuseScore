

async function URLFromFiles(files) {
    const promises = files.map(file =>
        fetch(file).then(response => response.text())
    );

    const texts = await Promise.all(promises);
    const text = texts.join("");
    const blob = new Blob([text], { type: "application/javascript" });
    return URL.createObjectURL(blob);
}

let AudioDriver = (function () {

    let audioContext = null;
    let processor = null;

    var api = {

        inited: false,
        onInited: null,
        audioContext: null, // [editude] exposed for direct resume in user gesture context

        setup: async function(config, rpcPort, audio_context) {

            if (audio_context) {
                audioContext = audio_context
            } else {
                // [editure] Use 'interactive' latency hint to minimise the
                // AudioContext pipeline delay.  Without this, WKWebView may
                // choose a conservative buffer size (100 ms+), causing the
                // playback cursor to visibly lead the audible output.
                audioContext = new AudioContext({latencyHint: 'interactive'})
            }
            api.audioContext = audioContext;

            try {

                const code = await URLFromFiles([
                    './MuseAudio.js',
                    './distr/audio_worklet_processor.js'
                ]);

                await audioContext.audioWorklet.addModule(code)
                processor = new AudioWorkletNode(audioContext, 'musedriver-processor', {
                                numberOfInputs: 0,
                                numberOfOutputs: 1,
                                outputChannelCount: [2],
                                });
                console.log("[audiodriver] AudioWorklet module loaded, AudioWorkletNode created")

            } catch (error) {
                console.error("[audiodriver] AudioWorklet setup error:", error)
            }

            // [editude] Guard: if AudioWorklet failed to load (e.g. MuseAudio.js
            // missing), processor is null. Audio won't work but app continues.
            if (!processor) {
                console.warn("[audiodriver] AudioWorklet init failed — audio disabled");
                return;
            }

            // driver (processor) -> main
            // [editure] Audio sample batching for the native audio bridge.
            // The AudioWorklet posts 128-sample quanta; we accumulate them
            // into 512-sample batches, base64-encode, and forward to Swift.
            var audioBatch = [];
            var audioBatchSamples = 0;
            var AUDIO_BATCH_TARGET = 512; // samples per channel

            processor.port.onmessage = function(event) {
                // [editure] Fast path for audio samples — no logging.
                if (event.data.type === 'audio') {
                    audioBatch.push(new Float32Array(event.data.data));
                    audioBatchSamples += 128;
                    if (audioBatchSamples >= AUDIO_BATCH_TARGET) {
                        var merged = new Float32Array(audioBatchSamples * 2);
                        var offset = 0;
                        for (var i = 0; i < audioBatch.length; i++) {
                            merged.set(audioBatch[i], offset);
                            offset += audioBatch[i].length;
                        }
                        var bytes = new Uint8Array(merged.buffer);
                        window.webkit.messageHandlers.audio.postMessage(
                            btoa(String.fromCharCode.apply(null, bytes))
                        );
                        audioBatch = [];
                        audioBatchSamples = 0;
                    }
                    return;
                }

                console.log("[processor]", event.data)

                if (event.data.type == "DRIVER_INITED") {
                    api.inited = true;
                    if (api.onInited) {
                        api.onInited();
                    }
                }
            }

            processor.port.postMessage({
                type: 'INITIALIZE_DRIVER',
                config: config,
                rpcPort: rpcPort,
                options: {}
            }, [rpcPort]);
        },

        outputSpec: function() {
            // [editure] Clamp buffer size: baseLatency on WKWebView can be
            // very large (100 ms+), producing oversized buffers that bloat
            // memory reservation without improving timing (the AudioWorklet
            // render quantum is always 128 samples).
            var samples = Math.round(audioContext.baseLatency * audioContext.sampleRate);
            samples = Math.max(samples, 128);
            samples = Math.min(samples, 2048);
            return {
                sampleRate: audioContext.sampleRate,
                samplesPerChannel: samples,
                audioChannelCount: audioContext.destination.channelCount
            }
        },

        open: function() {
            console.log("[audiodriver] open — connecting processor, context state:", audioContext.state)
            // [editure] When native audio bridge is active, mute the Web
            // Audio output via a zero-gain node.  The AudioWorklet stays
            // alive (still connected to the graph) and the C++ Mixer clock
            // keeps ticking, but no audible output goes through Web Audio.
            // Audio reaches speakers via AVAudioEngine instead.
            if (window._editudeNativeAudio) {
                var muteGain = audioContext.createGain();
                muteGain.gain.value = 0;
                processor.connect(muteGain);
                muteGain.connect(audioContext.destination);
                console.log("[audiodriver] native audio active — Web Audio muted");
            } else {
                processor.connect(audioContext.destination)
            }
            // resume() here is best-effort (may lack user gesture on iOS)
            audioContext.resume()
        },

        resume: function() {
            console.log("[audiodriver] resume — context state:", audioContext.state)
            audioContext.resume()
        },

        suspend: function() {
            console.log("[driver]", "suspend")
            audioContext.suspend();
        },

        close: function() {
            console.log("[driver]", "close")
            processor.disconnect()
        },

        // [editure] Tell the AudioWorklet to start forwarding rendered
        // samples to the main thread for the native audio bridge.
        enableNativeAudio: function() {
            if (processor) {
                processor.port.postMessage({type: 'ENABLE_NATIVE_AUDIO'});
                console.log("[audiodriver] native audio bridge enabled");
            }
        },

        // [editure] Measure the real output pipeline latency so C++ can
        // compensate the playback cursor.  Prefer getOutputTimestamp()
        // (dynamic, signal-based) over the static outputLatency property.
        // Returns the total delay between audio being rendered in the
        // AudioWorklet and exiting the speakers.
        getOutputLatencySec: function() {
            if (!audioContext) return 0;
            var pipelineLatency = 0;
            // getOutputTimestamp() returns the context-time of audio
            // currently exiting the speakers.  The gap between that and
            // currentTime is the output pipeline delay.
            if (typeof audioContext.getOutputTimestamp === 'function') {
                var ts = audioContext.getOutputTimestamp();
                if (ts && ts.contextTime > 0) {
                    pipelineLatency = audioContext.currentTime - ts.contextTime;
                }
            }
            // Fallback: outputLatency property (may be 0 in WKWebView).
            if (pipelineLatency <= 0 && audioContext.outputLatency > 0) {
                pipelineLatency = audioContext.outputLatency;
            }
            // Fallback: baseLatency (render-quantum latency only).
            if (pipelineLatency <= 0) {
                pipelineLatency = audioContext.baseLatency || 0;
            }
            // The native Swift shell injects the hardware output latency
            // via window._editudeNativeOutputLatency (AVAudioEngine
            // outputNode.presentationLatency + latency).  This measures
            // the CoreAudio → speaker segment of the pipeline, which is
            // downstream of WebKit's internal audio buffering (captured
            // by outputLatency above).  The two are additive, not
            // overlapping — audio flows through WebKit buffers first,
            // then through the CoreAudio hardware chain.
            var nativeLat = window._editudeNativeOutputLatency || 0;
            pipelineLatency += nativeLat;
            return pipelineLatency;
        }
    }

    return api;

})();

export default AudioDriver;