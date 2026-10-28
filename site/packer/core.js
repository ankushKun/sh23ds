// The browser packer's engine: runs tools/sh2conv/pack.py in Pyodide, step by step, and hands every
// file it makes to a sink (a zip, or a folder). worker.js runs it for the page, and
// tools/packer-web/run.mjs runs the same file in Node for the tests.
//
// Nothing here talks to a server: the game files come from the player's own disk (env.mountGame) and
// the output goes to their own disk (env.sink). The libraries come from public CDNs (worker.js).
//
// Python reaches outside itself through tools/sh2conv/host.py, which calls the sh2host object made
// here: run() for ffmpeg / ffprobe (ffmpeg.wasm), copy() for files that go into the pack unchanged,
// texture() for the tex3ds encoders (texenc.mjs).
(function (root) {
	'use strict';

	// Kinds of file no later step opens again (from SH2PACK_TRACE runs of the native packer), so they
	// leave Python's memory once they are in the output: they are cut to 0 bytes rather than deleted,
	// since converters skip a texture whose .t3x already exists. Everything else stays until the end:
	// the area index reads the rooms' .s3m headers and .cam files, the shadow step lists the .s3c files.
	const FREE_AFTER_SINK = /\.(t3x|mpg)$/i;

	function isExtra(rel) {
		// the files the 3DS doesn't read (pack.py is_extra): png folders and .json files
		const parts = rel.split('/');
		return parts.slice(0, -1).includes('png') || rel.endsWith('.json');
	}

	async function createPacker(env) {
		const log = env.log || (() => {});
		const t0 = Date.now();
		const say = (m) => log({ kind: 'status', text: m });

		say('Loading Python');
		const py = await env.loadPyodide({
			stdout: (s) => log({ kind: 'out', text: s }),
			stderr: (s) => log({ kind: 'err', text: s }),
		});
		await py.loadPackage(['numpy', 'pillow'], { messageCallback: () => {} });

		say('Loading the converters');
		const bundle = await env.fetchBytes('sh2conv.zip');
		py.unpackArchive(bundle, 'zip', { extractDir: '/sh23ds' });

		say('Loading the encoders');
		const tex = await env.createTexenc();
		tex._texenc_set_quality(env.quality === 'best' ? 2 : 1);
		// ffmpeg.wasm (32 MB) only in a worker that runs a task needing it (movies, sound, scenes)
		let ff = null;
		const ffLog = { out: [], err: [] };
		async function ensureFFmpeg() {
			if (ff) return;
			ff = await env.createFFmpegCore();
			ff.setLogger(({ type, message }) => (type === 'stdout' ? ffLog.out : ffLog.err).push(message));
		}

		const FS = py.FS;
		const game = env.mountGame(py);      // {blob(path), read(path)} for files under /game
		FS.mkdirTree('/out');

		// ------------------------------------------------------------------ sh2host (host.py)
		const virtual = new Map();           // output path -> game path, for copy()
		let mountN = 0;
		let shown = -1, shownAt = 0, count = null;
		function report(v, done, total, what) {   // the current step's progress, at most 5 times a second
			if (done !== undefined && done !== null) count = { done, total, what };
			const now = Date.now();
			const last = count && count.done === count.total;
			if (v < 1 && !last && now - shownAt < 200 && Math.abs(v - shown) < 0.05) return;
			shown = v;
			shownAt = now;
			log({ kind: 'progress', value: Math.max(0, Math.min(1, v)), count });
		}

		function ffMkdirs(path) {
			const parts = path.split('/').slice(1, -1);
			let p = '';
			for (const d of parts) {
				p += '/' + d;
				try { ff.FS.mkdir(p); } catch (e) { /* exists */ }
			}
		}

		// ffprobe -show_entries format=duration:stream=codec_type,width,height -of json, for ffmpeg's build
		// without SIMD (worker.js), which has no ffprobe: `ffmpeg -i` prints the same facts as it starts
		function probeFromLog(lines) {
			const text = lines.join('\n');
			const d = /Duration: (\d+):(\d+):(\d+(?:\.\d+)?)/.exec(text);
			const streams = [];
			for (const m of text.matchAll(/Stream #\d+:\d+[^:\n]*: (Video|Audio|Subtitle|Data): ([^\n]*)/g)) {
				const s = { codec_type: m[1].toLowerCase() };
				const wh = s.codec_type === 'video' && /(?:^|[ ,])(\d{2,5})x(\d{2,5})(?=[ ,[]|$)/.exec(m[2]);
				if (wh) [s.width, s.height] = [Number(wh[1]), Number(wh[2])];
				streams.push(s);
			}
			if (!d && !streams.length) return null;
			const json = { streams, format: d ? { duration: String(3600 * d[1] + 60 * d[2] + Number(d[3])) } : {} };
			return new TextEncoder().encode(JSON.stringify(json));
		}

		function ffInput(path, mounts) {
			// a game file is mounted into ffmpeg's file system as it is (a Blob in the browser);
			// anything else is copied over
			const blob = game.blob(path);
			if (blob && ff.FS.filesystems.WORKERFS) {
				const dir = '/in' + (mountN++);
				ff.FS.mkdir(dir);
				const name = path.split('/').pop();
				ff.FS.mount(ff.FS.filesystems.WORKERFS, { blobs: [{ name, data: blob }] }, dir);
				mounts.push(dir);
				return dir + '/' + name;
			}
			ffMkdirs(path);
			ff.FS.writeFile(path, FS.readFile(path));
			mounts.push(path);
			return path;
		}

		const sh2host = {
			run(argvJs, inputJs, lo, hi) {
				const argv = Array.from(argvJs);
				const tool = argv[0].split('/').pop();
				if (!ff) return { code: 127, stdout: new Uint8Array(0), stderr: 'ffmpeg.wasm is not loaded in this worker' };
				const args = argv.slice(1);
				const mounts = [], outputs = [];
				let stdoutFile = null;
				for (let i = 0; i < args.length; i++) {
					const a = args[i];
					const isInput = args[i - 1] === '-i' || (tool === 'ffprobe' && i === args.length - 1);
					if (/^(pipe:[01]?|-)$/.test(a)) {
						if (isInput) {
							ff.FS.writeFile('/stdin', inputJs ? new Uint8Array(inputJs) : new Uint8Array(0));
							args[i] = '/stdin';
							mounts.push('/stdin');
						} else {
							stdoutFile = args[i] = '/stdout';
						}
					} else if (a.startsWith('/') && isInput) {
						args[i] = ffInput(a, mounts);
					} else if (a.startsWith('/') && tool === 'ffmpeg' && i === args.length - 1) {
						ffMkdirs(a);
						outputs.push(a);
					}
				}
				ffLog.out.length = ffLog.err.length = 0;
				// ffmpeg's own progress (by the input's duration) fills this run's share of the step
				ff.setProgress(({ progress }) => {
					if (lo !== undefined && progress >= 0 && progress <= 1) report(lo + progress * (hi - lo));
				});
				let probed = null;
				if (tool === 'ffprobe' && !ff.ffprobe) {
					// the log level stays as the last run set it (-v error), so name it
					ff.exec('-hide_banner', '-v', 'info', '-i', args[args.length - 1]);
					probed = probeFromLog(ffLog.err);
				} else if (tool === 'ffprobe') ff.ffprobe(...args);
				else if (tool === 'ffmpeg') ff.exec(...args);
				else return { code: 127, stdout: new Uint8Array(0), stderr: 'no ' + tool + ' in the browser' };
				let code = probed ? 0 : ff.ret;
				ff.reset();
				let stdout;
				if (tool === 'ffprobe' && !ff.ffprobe) {
					stdout = probed || new Uint8Array(0);
					if (!probed) code = 1;
				} else if (stdoutFile) {
					try { stdout = ff.FS.readFile(stdoutFile); ff.FS.unlink(stdoutFile); } catch (e) { stdout = new Uint8Array(0); }
				} else {
					stdout = new TextEncoder().encode(ffLog.out.join('\n') + (ffLog.out.length ? '\n' : ''));
				}
				if (tool === 'ffprobe' && code === -1 && ffLog.out.length) code = 0;   // ffprobe leaves ret unset
				for (const o of outputs) {
					try {
						const data = ff.FS.readFile(o);
						ff.FS.unlink(o);
						FS.mkdirTree(o.slice(0, o.lastIndexOf('/')));
						FS.writeFile(o, data);
					} catch (e) {
						if (code === 0) code = 1;
					}
				}
				for (const m of mounts) {
					try {
						if (m.startsWith('/in')) { ff.FS.unmount(m); ff.FS.rmdir(m); } else ff.FS.unlink(m);
					} catch (e) { /* gone */ }
				}
				return { code, stdout, stderr: ffLog.err.slice(-20).join('\n') };
			},
			copy(src, dst) {
				virtual.set(dst, src);
			},
			progress(v, done, total, what) {
				report(v, done, total, what);
			},
			texture(fmt, px, w, h) {
				const n = tex._texenc_size(fmt, w, h);
				const ip = tex._malloc(px.length), op = tex._malloc(n);
				tex.HEAPU8.set(px, ip);
				const k = tex._texenc_level(fmt, ip, w, h, op);
				const out = tex.HEAPU8.slice(op, op + k);
				tex._free(ip);
				tex._free(op);
				return out;
			},
		};
		root.sh2host = sh2host;            // host.py: from js import sh2host

		// ------------------------------------------------------------------ output
		const sent = new Map();               // rel path -> "size:mtime" when it went to the sink
		let bytesOut = 0, filesOut = 0;

		function walk(dir, out) {
			for (const name of FS.readdir(dir)) {
				if (name === '.' || name === '..') continue;
				const p = dir + '/' + name;
				const st = FS.stat(p);
				if (FS.isDir(st.mode)) walk(p, out);
				else out.push([p, st]);
			}
			return out;
		}

		async function drain(final) {
			for (const [p, st] of walk('/out', [])) {
				const rel = p.slice(5);
				const key = st.size + ':' + Number(st.mtime);
				if (sent.get(rel) === key || (env.skip && env.skip.includes(rel) && !final)) continue;
				sent.set(rel, key);
				if (isExtra(rel) && !env.keepExtras) {
					if (rel.endsWith('.png') || final) FS.unlink(p);
					continue;
				}
				await env.sink.add(rel, FS.readFile(p));
				bytesOut += st.size;
				filesOut++;
				if (final) {
					FS.unlink(p);
				} else if (FREE_AFTER_SINK.test(rel)) {
					FS.truncate(p, 0);
					const t = FS.stat(p);
					sent.set(rel, t.size + ':' + Number(t.mtime));
				}
			}
			for (const [dst, src] of virtual) {
				const rel = dst.slice(5);
				const data = game.blob(src) || game.read(src);
				await env.sink.add(rel, data);
				bytesOut += data.size !== undefined ? data.size : data.length;
				filesOut++;
			}
			virtual.clear();
		}

		// ------------------------------------------------------------------ the run
		py.globals.set('HALF', !!env.half);
		await py.runPythonAsync(`
import os, sys
sys.path.insert(0, '/sh23ds/tools/sh2conv')
os.chdir('/tmp')
import pack
P = pack.Packer('/game', '/out', half=HALF)
STEPS = [(s[0], s[1]) for s in P.steps()]
`);
		// a PyProxy must be destroyed by hand: WebKit's garbage collector would otherwise finalize it
		// later, outside Python, which Pyodide reports as an error (NoGilError)
		const stepsProxy = py.globals.get('STEPS');
		const steps = stepsProxy.toJs();
		stepsProxy.destroy();

		// which release the folder holds (gameversion.py); a wrong one stops with the message for the
		// player alone, not Python's traceback
		function checkGame() {
			say('Checking the game folder');
			const problem = py.runPython('P.problem() or ""');
			if (problem) throw new Error(problem);
		}

		async function run(onStep) {
			await ensureFFmpeg();
			checkGame();
			py.runPython('P.begin()');
			for (let i = 0; i < steps.length; i++) {
				const [id, title] = steps[i];
				if (env.only && !env.only.includes(id)) continue;
				onStep && onStep({ index: i, count: steps.length, id, title, state: 'start' });
				const s0 = Date.now();
				count = null;
				py.globals.set('STEP_ID', id);
				py.runPython('P.run_step(STEP_ID)');
				await drain(false);
				onStep && onStep({ index: i, count: steps.length, id, title, state: 'done', seconds: (Date.now() - s0) / 1000 });
			}
			py.runPython('P.finish()');
			await drain(true);
			await env.sink.finish();
			return { files: filesOut, bytes: bytesOut, seconds: (Date.now() - t0) / 1000 };
		}

		// ---------------------------------------------------------- tasks, for several workers at once
		const foreign = new Set();
		function globRe(g) {
			return new RegExp('^' + g.split('*').map((x) => x.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('[^/]*') + '$');
		}

		const api = {
			steps,
			run,
			py,
			// the first worker checks the exe and prints the Enhanced Edition report; the others only set up
			begin(first) {
				if (first) checkGame();
				py.globals.set('FIRST', !!first);
				py.runPython('P.begin(report=FIRST)');
			},
			tasks() {
				return py.runPython('import json; json.dumps(P.tasks())');
			},
			async runTask(task) {
				if (task.ffmpeg) await ensureFFmpeg();
				const s0 = Date.now();
				count = null;
				shown = -1;
				py.globals.set('TASK_ID', task.id);
				py.runPython('P.run_task(TASK_ID)');
				await drain(false);
				return (Date.now() - s0) / 1000;
			},
			// pack files matching globs ("bg/*/rooms.txt"), as [{rel, data}], for another worker
			collect(globs) {
				const res = globs.map(globRe);
				const out = [];
				for (const [p] of walk('/out', [])) {
					const rel = p.slice(5);
					if (res.some((r) => r.test(rel))) out.push({ rel, data: FS.readFile(p) });
				}
				return out;
			},
			// files another worker made, put where the converters look for them for the next task
			put(files) {
				for (const { rel, data } of files) {
					const p = '/out/' + rel;
					FS.mkdirTree(p.slice(0, p.lastIndexOf('/')));
					FS.writeFile(p, data);
					const st = FS.stat(p);
					sent.set(rel, st.size + ':' + Number(st.mtime));    // not this worker's to send
					foreign.add(rel);
				}
			},
			// and away again once that task is done, so this worker's later tasks don't take them as its
			// own (the area index would index rooms whose files are in another worker)
			drop() {
				for (const rel of foreign) {
					try { FS.unlink('/out/' + rel); } catch (e) { /* gone */ }
				}
				foreign.clear();
			},
			stats: () => ({ files: filesOut, bytes: bytesOut }),
		};
		return api;
	}

	root.SH2Packer = { createPacker, isExtra };
	if (typeof module !== 'undefined') module.exports = root.SH2Packer;
})(typeof self !== 'undefined' ? self : globalThis);
