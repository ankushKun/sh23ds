// A browser packer worker (packer.html starts one to four, as module workers: Pyodide needs that).
// Each runs core.js with:
//   - the game folder the player picked, mounted read-only and read only when a task needs a file
//     (WORKERFS over the picked File objects; nothing is copied or sent anywhere)
//   - Pyodide and ffmpeg.wasm from jsDelivr, at fixed versions (ffmpeg's build without SIMD where the
//     processor can't run SIMD, see simd()), and texenc.mjs from this site
//   - its output sent to writer.js over a MessageChannel, so all workers write one zip or folder
// The page hands out the tasks (pack.py tasks()) and moves files between workers when a task needs
// another worker's output.
//
// Messages in:  {type: 'init', files, options, port, first}   {type: 'task', task}
//               {type: 'collect', id, globs}   {type: 'put', id, files}   {type: 'end'}
// Messages out (req: the id of the request they answer): {type: 'ready', steps, tasks}
//               {type: 'task-done', task, seconds}   {type: 'collected', files}   {type: 'put-done'}
//               {type: 'ended'}   and unasked {type: 'log' | 'progress' | 'error', ...}
import './core.js';     // self.SH2Packer
import createTexenc from './texenc.mjs';
import { PYODIDE_URL, FFMPEG_URL, FFMPEG_NOSIMD_URL } from './cdn.js';

// Whether this browser runs WebAssembly SIMD here: a function with two SIMD instructions, compiled and
// called (V8 compiles lazily, and on a processor without SSE4.1 it fails only then, with "Wasm SIMD
// unsupported"). packer.html?simd=0 takes the build without SIMD anyway.
const SIMD_TEST = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 127, 3, 2, 1, 0, 7, 5, 1, 1, 102,
	0, 0, 10, 11, 1, 9, 0, 65, 1, 253, 17, 253, 27, 0, 11]);
function simd() {
	if (new URL(self.location.href).searchParams.get('simd') === '0') return false;
	try {
		return new WebAssembly.Instance(new WebAssembly.Module(SIMD_TEST)).exports.f() === 1;
	} catch (e) {
		return false;
	}
}
const SIMD = simd();
const FF_URL = SIMD ? FFMPEG_URL : FFMPEG_NOSIMD_URL;

// loaded behind a promise: a top-level await would let the page's first message arrive before
// onmessage is set, and be dropped
const libs = Promise.all([import(PYODIDE_URL + 'pyodide.mjs'), import(FF_URL + 'ffmpeg-core.js')]);
const post = (m, t) => self.postMessage(m, t || []);
let engine = null, port = null, current = null, req = null;   // req: the page's request being answered

async function init({ files, options, first }) {
	const [{ loadPyodide }, { default: createFFmpegCore }] = await libs;
	// the picked folder's files by their path inside it: "data/bg/ap/ap01.map" -> File
	const byPath = new Map(files.map((f) => [f.path, f.file]));
	engine = await SH2Packer.createPacker({
		loadPyodide: (o) => loadPyodide({ ...o, indexURL: PYODIDE_URL }),
		createFFmpegCore: () => createFFmpegCore(SIMD ? {
			mainScriptUrlOrBlob: FFMPEG_URL + 'ffmpeg-core.js#' +
				btoa(JSON.stringify({ wasmURL: FFMPEG_URL + 'ffmpeg-core.wasm' })),
		} : { locateFile: (name) => FF_URL + name }),
		createTexenc: () => createTexenc(),
		fetchBytes: async (name) => new Uint8Array(await (await fetch(name, { cache: 'no-cache' })).arrayBuffer()),
		mountGame(py) {
			py.FS.mkdirTree('/game');
			py.FS.mount(py.FS.filesystems.WORKERFS, { blobs: files.map((f) => ({ name: f.path, data: f.file })) }, '/game');
			return {
				blob: (p) => byPath.get(p.slice('/game/'.length)) || null,
				read: (p) => new Uint8Array(new FileReaderSync().readAsArrayBuffer(byPath.get(p.slice('/game/'.length)))),
			};
		},
		sink: {
			// to writer.js; a Uint8Array from Python's file system is a copy, so its buffer can move
			async add(rel, data) {
				if (data instanceof Uint8Array) port.postMessage({ rel, data }, [data.buffer]);
				else port.postMessage({ rel, data });
			},
			async finish() {},
		},
		quality: options.quality,
		half: options.half,
		keepExtras: false,
		skip: ['ee_used.txt'],     // each worker logs its own; the page merges them
		log: (m) => {
			if (m.kind === 'progress') post({ type: 'progress', task: current, value: m.value, count: m.count });
			else post({ type: 'log', ...m });
		},
	});
	if (first && !SIMD) {
		post({ type: 'log', kind: 'out', text: 'This processor can’t run WebAssembly SIMD, so the movies and sounds use an older ffmpeg build, which is a little slower.' });
	}
	engine.begin(first);
	post({ type: 'ready', req, steps: engine.steps, tasks: first ? engine.tasks() : null });
}

self.onmessage = async (e) => {
	const m = e.data;
	req = m.id;
	try {
		if (m.type === 'init') {
			port = m.port;
			await init(m);
		} else if (m.type === 'task') {
			current = m.task.id;
			const seconds = await engine.runTask(m.task);
			current = null;
			post({ type: 'task-done', req: m.id, task: m.task.id, seconds });
		} else if (m.type === 'collect') {
			const files = engine.collect(m.globs);
			post({ type: 'collected', req: m.id, files }, files.map((f) => f.data.buffer));
		} else if (m.type === 'put') {
			engine.put(m.files);
			post({ type: 'put-done', req: m.id });
		} else if (m.type === 'drop') {
			engine.drop();
			post({ type: 'dropped', req: m.id });
		} else if (m.type === 'end') {
			port.postMessage({ type: 'end' });
			post({ type: 'ended', req: m.id });
		}
	} catch (err) {
		post({ type: 'error', message: String(err && err.message || err), stack: String(err && err.stack || '') });
	}
};
