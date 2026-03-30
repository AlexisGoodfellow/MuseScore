

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
                audioContext = new AudioContext()
            }
            api.audioContext = audioContext;
            console.log("[audiodriver] AudioContext created, state:", audioContext.state);

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
            processor.port.onmessage = function(event) {
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
            return {
                sampleRate: audioContext.sampleRate,
                samplesPerChannel: Math.max(Math.round(audioContext.baseLatency * audioContext.sampleRate), 128),
                audioChannelCount: audioContext.destination.channelCount
            }
        },

        open: function() {
            console.log("[audiodriver] open — connecting processor, context state:", audioContext.state)
            processor.connect(audioContext.destination)
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
        }
    }

    return api;

})();

export default AudioDriver;