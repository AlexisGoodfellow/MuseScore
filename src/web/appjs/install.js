const fs = require('fs');

console.log("install js api step")

var args = process.argv.slice(2);
console.log("args:", args)
console.log("__dirname:", __dirname);

const HERE=__dirname
const ROOT=HERE+"/../.."
const OUTPUT_DIR = args.length > 0 ? args[0] : "./out"
const MUSE_MODULE_AUDIO_WORKER = "OFF"

function copyFile(src, dst) {
    try {
      fs.copyFileSync(src, dst);
      console.info("success: coped " + src + " => " + dst)
    } catch (err) {
      console.error("error: failed coped " + src + " => " + dst)
    }
}

// [editude] Recursively copy a directory tree.
function copyDirRecursive(src, dst) {
    fs.mkdirSync(dst, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
        const srcPath = src + "/" + entry.name;
        const dstPath = dst + "/" + entry.name;
        if (entry.isDirectory()) {
            copyDirRecursive(srcPath, dstPath);
        } else {
            copyFile(srcPath, dstPath);
        }
    }
}

// [editude] Walk a directory and collect all file paths (relative to base).
function collectFiles(dir, base, out) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = dir + "/" + entry.name;
        if (entry.isDirectory()) {
            collectFiles(full, base, out);
        } else {
            out.push(full.slice(base.length + 1)); // relative to base
        }
    }
    return out;
}
// [/editude]

function replaceAll(str, find, replace) {
  return String(str).replace(new RegExp(find, 'g'), replace);
}

function configure(file, out) {
  var content = fs.readFileSync(file)
  content = replaceAll(content, "{{MUSE_MODULE_AUDIO_WORKER}}", MUSE_MODULE_AUDIO_WORKER);
  fs.writeFileSync(out, content);
}

// Remove Unnecessary Qt files
fs.rmSync(OUTPUT_DIR+"/MuseScoreStudio.html", {force: true})
fs.rmSync(OUTPUT_DIR+"/qtloader.js", {force: true})
fs.rmSync(OUTPUT_DIR+"/qtlogo.svg", {force: true})

// Configure and copy config
fs.mkdirSync(OUTPUT_DIR+"/distr", { recursive: true });
configure(HERE+"/distr/config.js.in", OUTPUT_DIR+"/distr/config.js")

// Copy api 
copyFile(HERE+"/distr/muapi.js", OUTPUT_DIR+"/distr/muapi.js");
copyFile(HERE+"/distr/muimpl.js", OUTPUT_DIR+"/distr/muimpl.js");
copyFile(HERE+"/distr/qtloader.js", OUTPUT_DIR+"/distr/qtloader.js");
copyFile(HERE+"/distr/audioworker.js", OUTPUT_DIR+"/distr/audioworker.js");
copyFile(HERE+"/distr/audiodriver.js", OUTPUT_DIR+"/distr/audiodriver.js"); 
copyFile(HERE+"/distr/audio_worklet_processor.js", OUTPUT_DIR+"/distr/audio_worklet_processor.js");

// Copy viewer
copyFile(HERE+"/viewer/viewer.html", OUTPUT_DIR+"/viewer.html");
copyFile(HERE+"/viewer/index.html", OUTPUT_DIR+"/index.html");
copyFile(HERE+"/viewer/index.html", OUTPUT_DIR+"/MuseScoreStudio.html"); 

// Copy tools
copyFile(HERE+"/viewer/run_server.sh", OUTPUT_DIR+"/run_server.sh");

// Copy SF if need
const SF_SRC=ROOT+"/share/sound/MS Basic.sf3"
const SF_DST=OUTPUT_DIR+"/sound/MS Basic.sf3";
if (!fs.existsSync(SF_DST)) {
  fs.mkdirSync(OUTPUT_DIR+"/sound", { recursive: true });
  copyFile(SF_SRC, SF_DST);
}

// [editude] Copy templates and generate preload manifest for qtloader.js.
// MuseScore's TemplatesRepository discovers .mscx dirs under /files/share/templates.
const TEMPLATES_SRC = ROOT + "/share/templates"
const TEMPLATES_DST = OUTPUT_DIR + "/templates"
if (fs.existsSync(TEMPLATES_SRC) && !fs.existsSync(TEMPLATES_DST)) {
  console.info("Copying templates...")
  copyDirRecursive(TEMPLATES_SRC, TEMPLATES_DST);

  // Generate preload manifest — each entry maps a URL-relative source to an
  // absolute path inside the Emscripten virtual filesystem.
  const files = collectFiles(TEMPLATES_DST, OUTPUT_DIR, []);
  const manifest = files.map(f => ({
    source: f,
    destination: "/files/share/" + f
  }));
  const manifestPath = OUTPUT_DIR + "/templates_preload.json";
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  console.info("Templates preload manifest: " + files.length + " files => " + manifestPath);
}
// [/editude]

