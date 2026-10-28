// Runs the browser packer's engine (site/packer/core.js) in Node, the way site/packer/worker.js runs
// it in a page, on a game folder on this disk:
//
//   node tools/packer-web/run.mjs "<Silent Hill 2 folder>" <out folder | out.zip> [--best] [--half] [--extras]
//
// The output is the SD card's sh23ds folder (a zip gets 3ds/sh23ds/ in front). compare.py then checks
// it against make_pack.sh's. Needs `npm install` in tools/packer-web (Pyodide and ffmpeg.wasm at the
// versions worker.js loads) and site/packer/sh2conv.zip (bundle.py).
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.join(HERE, '..', '..', 'site', 'packer');

const args = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const flags = new Set(process.argv.slice(2).filter((a) => a.startsWith('--')));
if (args.length < 2) {
	console.error('usage: run.mjs <game folder> <out folder | out.zip> [--best] [--half] [--extras]');
	process.exit(2);
}
const [gameDir, outPath] = args.map((a) => path.resolve(a));

// ffmpeg-core.js is built for browsers and workers; these are all it needs from one
globalThis.self = globalThis;
globalThis.location = { href: 'file:///' };
// FFMPEG_CORE=<an @ffmpeg/core dist folder>: another build, such as 0.12.0's without SIMD (worker.js)
const FF_DIR = process.env.FFMPEG_CORE || path.join(HERE, 'node_modules/@ffmpeg/core/dist');
const createFFmpegCore = require(path.join(FF_DIR, 'umd/ffmpeg-core.js'));
const ffWasm = fs.readFileSync(path.join(FF_DIR, 'umd/ffmpeg-core.wasm'));
const { loadPyodide } = await import(path.join(HERE, 'node_modules/pyodide/pyodide.mjs'));
const { default: createTexenc } = await import(path.join(SITE, 'texenc.mjs'));
const { createPacker } = require(path.join(SITE, 'core.js'));
const { ZipSink } = require(path.join(SITE, 'sinks.js'));

let sink;
if (outPath.endsWith('.zip')) {
	const fd = fs.openSync(outPath, 'w');
	sink = new ZipSink((u8, at) => { fs.writeSync(fd, u8, 0, u8.length, at); }, '3ds/sh23ds/');
	const finish = sink.finish.bind(sink);
	sink.finish = async () => { const n = await finish(); fs.closeSync(fd); return n; };
} else {
	sink = {
		async add(rel, data) {
			const p = path.join(outPath, rel);
			fs.mkdirSync(path.dirname(p), { recursive: true });
			fs.writeFileSync(p, data instanceof Uint8Array ? data : new Uint8Array(await data.arrayBuffer()));
		},
		async finish() {},
	};
}

const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(0).padStart(5) + ' s ';
const packer = await createPacker({
	loadPyodide: (o) => loadPyodide({
		...o,
		packageBaseUrl: 'https://cdn.jsdelivr.net/pyodide/v314.0.7/full/',
		packageCacheDir: path.join(HERE, 'node_modules', '.pyodide-cache'),   // else it caches under ./https:/
	}),
	createFFmpegCore: () => createFFmpegCore({
		instantiateWasm: (imports, ok) => {
			WebAssembly.instantiate(ffWasm, imports).then((r) => ok(r.instance, r.module));
			return {};
		},
	}),
	createTexenc: () => createTexenc(),
	fetchBytes: async (name) => new Uint8Array(fs.readFileSync(path.join(SITE, name))),
	mountGame(py) {
		py.FS.mkdirTree('/game');
		py.FS.mount(py.FS.filesystems.NODEFS, { root: gameDir }, '/game');
		return {
			blob: () => null,
			read: (p) => new Uint8Array(fs.readFileSync(path.join(gameDir, p.slice('/game/'.length)))),
		};
	},
	sink,
	quality: flags.has('--best') ? 'best' : 'fast',
	half: flags.has('--half'),
	keepExtras: flags.has('--extras'),
	only: process.env.ONLY ? process.env.ONLY.split(',') : null,   // ONLY=rooms,events: just those steps
	log: (m) => {
		if (m.kind === 'status') console.log(stamp() + '-- ' + m.text);
		else console.log(stamp() + m.text);
	},
});
const r = await packer.run((s) => {
	if (s.state === 'start') console.log(stamp() + `== ${s.index + 1}/${s.count} ${s.title}`);
	else console.log(stamp() + `   done in ${s.seconds.toFixed(0)} s`);
});
console.log(`${r.files} files, ${(r.bytes / 1e6).toFixed(0)} MB in ${r.seconds.toFixed(0)} s`);
