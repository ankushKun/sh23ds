// The packer, step 2 of the install guide on the home page (index.html#pack): picks the game folder, then runs the packer in several workers at once and shows it.
//
// writer.js owns the output. worker.js runs pack.py's tasks (tasks() in tools/sh2conv/pack.py): the
// 'main' ones in order in the first worker, since they read each other's files, and the 'any' ones (the
// rooms by area, the movies) in whichever worker is free, longest first. A task that needs files another
// worker made gets them copied in first; each worker that made rooms writes its area indexes at the end.
(() => {
	'use strict';
	const $ = (s) => document.querySelector(s);
	const canPickDir = typeof window.showDirectoryPicker === 'function';
	// the zip goes straight into a file the player picks where the browser can (writer.js), else it is
	// built in the browser's storage and saved at the end
	const canSaveFile = typeof window.showSaveFilePicker === 'function';
	// analytics (site.js): counts, sizes and options only, never file names or contents
	const track = window.track || (() => {});
	const trackError = window.trackError || (() => {});
	const gb = (bytes) => Math.round(bytes / 1e8) / 10;

	// ?simd=0 in the page's address makes the workers take ffmpeg's build without WebAssembly SIMD (worker.js)
	const WORKER_URL = 'packer/worker.js' + (new URLSearchParams(location.search).get('simd') === '0' ? '?simd=0' : '');

	// workers: each holds its own Python (a few hundred MB), so fewer on a small machine
	const cores = navigator.hardwareConcurrency || 4;
	const autoLanes = navigator.deviceMemory && navigator.deviceMemory < 4 ? 1
		: cores >= 8 ? 3 : cores >= 4 ? 2 : 1;

	const OPTIONS = {
		quality: [
			['fast', 'Fast', 'About 20 times faster than Same as Docker, at a quality you won’t tell apart on the console.'],
			['best', 'Same as Docker', 'The same texture encoding as the Docker packer. Adds an hour or more.'],
		],
		half: [
			['off', 'Off', ''],
			['on', 'On', 'Adds a half-size copy of every movie (75 MB) that the original 3DS plays more easily.'],
		],
		lanes: [
			['auto', `Auto (${autoLanes})`, ''],
			['1', '1', 'One worker: the slowest, and the least memory.'],
			['2', '2', ''],
			['3', '3', ''],
			['4', '4', 'Four workers need about 2 GB of memory.'],
		],
		out: canPickDir
			? [['zip', 'Zip', canSaveFile ? 'A zip you unzip onto the SD card, about 1.5 GB, written where you pick as it’s made.'
				: 'A zip you unzip onto the SD card. Needs about 1.5 GB free while it’s made.'],
				['dir', 'Folder', 'Writes the files straight into a folder you pick, such as the SD card itself.']]
			: [['zip', 'Zip', 'A zip you unzip onto the SD card. Needs about 1.5 GB free while it’s made.']],
	};
	const state = { quality: 0, half: 0, lanes: 0, out: 0 };
	let game = null;          // {files: [{path, file}], name, bytes, ee}
	let running = false;
	let blocked = false;      // the browser check found something that stops the packer here

	function showOptions() {
		const notes = [];
		for (const el of document.querySelectorAll('[data-option]')) {
			const key = el.dataset.option;
			const [, label, note] = OPTIONS[key][state[key]];
			el.querySelector('output').textContent = label;
			for (const b of el.querySelectorAll('button')) b.disabled = OPTIONS[key].length < 2 || running;
			if (note) notes.push(note);
		}
		$('[data-option-note]').textContent = notes.join(' ');
	}
	for (const el of document.querySelectorAll('[data-option]')) {
		el.addEventListener('click', (e) => {
			const b = e.target.closest('button[data-step]');
			if (!b) return;
			const key = el.dataset.option, n = OPTIONS[key].length;
			state[key] = (state[key] + Number(b.dataset.step) + n) % n;
			showOptions();
		});
	}
	showOptions();

	// ---------------------------------------------------------------- the browser check (check.js)
	function showCheck(r) {
		const list = $('[data-check-list]');
		list.textContent = '';
		const items = r.items.length ? r.items
			: [{ level: 'ok', text: `Everything the packer needs works in ${r.browser || 'this browser'}.` }];
		for (const item of items) {
			const li = document.createElement('li');
			li.className = 'is-' + item.level;
			li.innerHTML = '<span class="sq" aria-hidden="true"></span><span></span>';
			li.lastChild.textContent = item.text;
			list.appendChild(li);
		}
		blocked = r.items.some((i) => i.level === 'stop');
		if (!blocked && !r.zip && OPTIONS.out.length > 1) {
			// no room for the zip in this window, but the browser can write into a folder
			OPTIONS.out = OPTIONS.out.filter((o) => o[0] !== 'zip');
			state.out = 0;
		}
		$('[data-pick]').disabled = blocked || running;
		$('[data-start]').disabled = !game || blocked || running;
		showOptions();
		const ids = (level) => r.items.filter((i) => i.level === level).map((i) => i.id).join(',');
		track('packer_browser', { browser: r.browser || 'other', stop: ids('stop'), warn: ids('warn'), note: ids('note') });
	}
	function runCheck() {
		(window.SH2Check ? SH2Check.run() : Promise.reject(new Error('check.js didn’t load')))
			.catch((e) => ({ browser: '', zip: true, items: [{ level: 'warn', id: 'check',
				text: `The browser check itself failed (${e && e.message || e}), so the packer may or may not work here.` }] }))
			.then(showCheck);
	}
	// The check starts a worker and touches the CDN and the site's storage, so it waits until the
	// visitor gets near the packer on the page.
	if ('IntersectionObserver' in window) {
		const io = new IntersectionObserver((entries) => {
			if (!entries.some((e) => e.isIntersecting)) return;
			io.disconnect();
			runCheck();
		}, { rootMargin: '600px 0px' });
		io.observe($('[data-check-list]'));
	} else runCheck();

	// ---------------------------------------------------------------- the game folder
	async function walkHandle(dir, prefix, out) {
		for await (const [name, h] of dir.entries()) {
			if (h.kind === 'directory') await walkHandle(h, prefix + name + '/', out);
			else out.push({ path: prefix + name, file: await h.getFile() });
		}
		return out;
	}

	function useFiles(all, folderName) {
		// the folder that holds sh2pc.exe and data/, wherever it sits in what was picked
		const exe = all.find((f) => /(^|\/)sh2pc\.exe$/i.test(f.path) &&
			all.some((g) => g.path.toLowerCase().startsWith(f.path.slice(0, -9).toLowerCase() + 'data/')));
		const err = $('[data-pick-error]');
		if (!exe) {
			game = null;
			err.textContent = 'That folder has no sh2pc.exe next to a data folder. Pick the Silent Hill 2 folder itself.';
			err.classList.add('is-on');
			$('[data-found]').textContent = 'No folder chosen yet.';
			$('[data-start]').disabled = true;
			track('packer_folder', { ok: false, files: all.length, picker: canPickDir ? 'directory' : 'input' });
			return;
		}
		err.classList.remove('is-on');
		const root = exe.path.slice(0, -9);
		const files = all.filter((f) => f.path.startsWith(root)).map((f) => ({ path: f.path.slice(root.length), file: f.file }));
		const bytes = files.reduce((n, f) => n + f.file.size, 0);
		const ee = files.some((f) => /^sh2e\//i.test(f.path));
		game = { files, bytes, ee, name: root.replace(/\/$/, '').split('/').pop() || folderName };
		$('[data-found]').textContent = `${game.name}: ${files.length.toLocaleString()} files, ${(bytes / 1e9).toFixed(1)} GB` +
			(ee ? ', with Enhanced Edition packs (they get used)' : '') + '.';
		$('[data-start]').disabled = blocked;
		track('packer_folder', { ok: true, files: files.length, gb: gb(bytes), ee, picker: canPickDir ? 'directory' : 'input' });
	}

	$('[data-pick]').addEventListener('click', async () => {
		if (blocked) return;
		if (canPickDir) {
			try {
				const dir = await window.showDirectoryPicker({ id: 'sh2game', mode: 'read' });
				$('[data-found]').textContent = 'Reading the folder…';
				useFiles(await walkHandle(dir, dir.name + '/', []), dir.name);
			} catch (e) {
				if (e.name !== 'AbortError') {
					$('[data-found]').textContent = 'Could not read that folder: ' + e.message;
					track('packer_folder', { ok: false, error: e.name, picker: 'directory' });
				}
			}
		} else {
			$('[data-pick-input]').click();
		}
	});
	$('[data-pick-input]').addEventListener('change', (e) => {
		const all = Array.from(e.target.files, (f) => ({ path: f.webkitRelativePath || f.name, file: f }));
		useFiles(all, all.length ? all[0].path.split('/')[0] : '');
	});

	// ---------------------------------------------------------------- progress display
	const stepsEl = $('[data-steps]'), logEl = $('[data-log]');
	const lines = [];
	let t0 = 0, timer = 0, lock = null;
	let tasks = [];                          // pack.py tasks(), plus their state
	const view = new Map();                  // step id -> {li, tasks: [...], weight}

	function log(text) {
		lines.push(text);
		if (lines.length > 400) lines.splice(0, lines.length - 400);
		logEl.textContent = lines.join('\n');
		logEl.scrollTop = logEl.scrollHeight;
	}
	const fmtTime = (sec) => sec >= 60 ? `${Math.floor(sec / 60)} min ${Math.round(sec % 60)} s` : Math.round(sec) + ' s';
	const pct = (f) => (Math.floor(1000 * f) / 10).toFixed(1) + '%';

	function overall() {
		let w = 0, d = 0;
		for (const t of tasks) {
			w += t.weight;
			d += t.weight * (t.state === 'done' ? 1 : t.state === 'running' ? t.frac : 0);
		}
		return w ? d / w : 0;
	}
	function clock() {
		const s = Math.floor((Date.now() - t0) / 1000);
		let text = `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, '0')} s`;
		const p = overall();
		if (running && p > 0.05 && p < 1) text += ` · about ${Math.max(1, Math.round(s * (1 - p) / p / 60))} min left`;
		$('[data-elapsed]').textContent = text;
	}

	function buildView(steps) {
		stepsEl.textContent = '';
		view.clear();
		for (const [id, title] of steps) {
			const mine = tasks.filter((t) => t.step === id);
			if (!mine.length) continue;
			const li = document.createElement('li');
			li.dataset.id = id;
			li.innerHTML = '<span class="sq"></span><span class="pk-name"><span class="pk-title"></span>' +
				'<span class="pk-count"></span></span>' +
				'<span class="pk-bar" aria-hidden="true"><i></i></span><span class="pk-sec"></span>';
			li.querySelector('.pk-title').textContent = title;
			stepsEl.appendChild(li);
			view.set(id, { li, tasks: mine, weight: mine.reduce((n, t) => n + t.weight, 0) });
		}
	}

	function showStep(id) {
		const v = view.get(id);
		if (!v) return;
		const ts = v.tasks;
		const frac = ts.reduce((n, t) => n + t.weight * (t.state === 'done' ? 1 : t.state === 'running' ? t.frac : 0), 0) / v.weight;
		const live = ts.filter((t) => t.state === 'running');
		const done = ts.filter((t) => t.state === 'done');
		v.li.classList.toggle('is-now', live.length > 0);
		v.li.classList.toggle('is-done', done.length === ts.length);
		v.li.querySelector('.pk-bar i').style.width = (100 * frac).toFixed(1) + '%';
		let count = '';
		if (ts.length > 1) {
			// a step split into tasks (the rooms by area): how many are done
			count = `${done.length} / ${ts.length} ${id === 'rooms' ? 'areas' : 'parts'}` +
				(live.length && live.length === 1 && live[0].count ? ` · ${live[0].count}` : '');
		} else if (ts[0].count) {
			count = ts[0].count;
		}
		if (done.length === ts.length) {
			// the last report comes just before the end
			count = count.replace(/^([\d,.]+) \/ ([\d,.]+)/, '$2 / $2');
			v.li.querySelector('.pk-sec').textContent = fmtTime(Math.max(...ts.map((t) => t.end)) - Math.min(...ts.map((t) => t.start)));
		} else if (live.length || done.length) {
			v.li.querySelector('.pk-sec').textContent = pct(frac);
		}
		v.li.querySelector('.pk-count').textContent = count;
	}

	function showAll() {
		for (const id of view.keys()) showStep(id);
		const p = overall();
		$('[data-total] i').style.width = (100 * p).toFixed(1) + '%';
		$('[data-total]').setAttribute('aria-valuenow', Math.round(100 * p));
		const now = [...view.entries()].filter(([, v]) => v.tasks.some((t) => t.state === 'running'));
		if (now.length) $('[data-status]').textContent = now.map(([, v]) => v.li.querySelector('.pk-title').textContent.split(' (')[0]).join(' · ');
		$('[data-count]').textContent = pct(p);
	}

	function stop() {
		running = false;
		clearInterval(timer);
		if (lock) lock.release().catch(() => {});
		lock = null;
		window.onbeforeunload = null;
		$('[data-start]').disabled = !game || blocked;
		$('[data-pick]').disabled = blocked;
		showOptions();
	}

	// ---------------------------------------------------------------- the run
	// one request to a worker that answers with a message of the given type (and id)
	function ask(w, msg, type, transfer) {
		return new Promise((resolve, reject) => {
			const id = Math.random().toString(36).slice(2);
			w.waiting.set(id, { type, resolve, reject });
			w.worker.postMessage({ ...msg, id }, transfer || []);
		});
	}

	// a failed run ends its workers too; the writer closes the zip first, so the next run can write one
	async function pack(opt) {
		const writer = new Worker('packer/writer.js', { type: 'module' });
		const lanes = [];
		let aborted = null;
		try {
			return await packWith(opt, writer, lanes, (a) => { aborted = a; });
		} catch (err) {
			if (aborted) {
				writer.postMessage({ type: 'abort' });
				await Promise.race([aborted, new Promise((r) => setTimeout(r, 2000))]);
			}
			throw err;
		} finally {
			for (const w of lanes) w.worker.terminate();
			writer.terminate();
		}
	}

	async function packWith(opt, writer, lanes, onAbort) {
		let opened = null, abortedDone = null;
		onAbort(new Promise((r) => { abortedDone = r; }));
		const writerDone = new Promise((resolve, reject) => {
			writer.onmessage = (e) => {
				if (e.data.type === 'done') resolve(e.data);
				else if (e.data.type === 'aborted') abortedDone();
				else if (e.data.type === 'ready') opened.resolve();
				else if (e.data.type === 'error') {
					const err = new Error(e.data.message);
					opened.reject(err);
					reject(err);
				}
			};
			writer.onerror = (e) => {
				const err = new Error(e.message || 'the writer stopped');
				opened.reject(err);
				reject(err);
			};
		});
		writerDone.catch(() => {});
		await new Promise((resolve, reject) => {
			opened = { resolve, reject };
			writer.postMessage({ type: 'open', out: opt.out, dir: opt.dir, file: opt.file, lanes: opt.lanes });
		});

		let failed = null;
		const fail = (err) => { if (!failed) failed = err; };
		writerDone.catch(fail);

		// the workers
		for (let i = 0; i < opt.lanes; i++) {
			const ch = new MessageChannel();
			writer.postMessage({ type: 'port', port: ch.port1 }, [ch.port1]);
			const w = { worker: new Worker(WORKER_URL, { type: 'module' }), waiting: new Map(), index: i, rooms: false };
			w.worker.onmessage = (e) => {
				const m = e.data;
				if (m.type === 'log') {
					if (m.kind === 'status' && i === 0) $('[data-status]').textContent = m.text;
					if (i === 0 || m.kind !== 'status') log(m.text);
				} else if (m.type === 'progress') {
					const t = tasks.find((x) => x.id === m.task);
					if (t) {
						t.frac = m.value;
						if (m.count) t.count = `${m.count.done.toLocaleString()} / ${m.count.total.toLocaleString()} ${m.count.what}`;
						showStep(t.step);
						const p = overall();
						$('[data-total] i').style.width = (100 * p).toFixed(1) + '%';
						$('[data-count]').textContent = pct(p);
					}
				} else if (m.type === 'error') {
					if (m.stack) console.error('packer worker ' + i + ': ' + m.stack);
					fail(new Error(m.message));
					for (const p of w.waiting.values()) p.reject(new Error(m.message));
					w.waiting.clear();
				} else {
					// the answer to ask()
					const p = w.waiting.get(m.req);
					if (p) {
						w.waiting.delete(m.req);
						p.resolve(m);
					}
				}
			};
			w.worker.onerror = (e) => {
				const err = new Error(e.message || 'a packer worker stopped, most likely out of memory');
				fail(err);
				for (const p of w.waiting.values()) p.reject(err);
				w.waiting.clear();
			};
			lanes.push(w);
			w.ready = ask(w, { type: 'init', files: game.files, options: opt, port: ch.port2, first: i === 0 }, 'ready', [ch.port2]);
		}

		// the first worker checks the game and lists the tasks
		const first = await lanes[0].ready;
		tasks = JSON.parse(first.tasks).map((t) => ({ ...t, state: 'wait', frac: 0, count: '' }));
		if (opt.only) tasks = tasks.filter((t) => opt.only.includes(t.step));
		buildView(first.steps);
		showAll();
		await Promise.all(lanes.map((w) => w.ready));

		const isDone = (pat) => pat.endsWith('*')
			? tasks.filter((t) => t.id.startsWith(pat.slice(0, -1))).every((t) => t.state === 'done')
			: tasks.every((t) => t.id !== pat || t.state === 'done');
		const ready = (t) => t.state === 'wait' && (t.after || []).every(isDone);

		function next(w) {
			if (w.index === 0) {
				// the main tasks in their order; while the next one waits, help with the others
				const m = tasks.find((t) => t.lane === 'main' && t.state !== 'done');
				if (m && m.state === 'wait' && ready(m)) return m;
			}
			const free = tasks.filter((t) => (t.lane === 'any' || (opt.lanes === 1 && t.lane === 'main')) && ready(t));
			if (opt.lanes === 1) {
				const m = tasks.find((t) => t.lane === 'main' && t.state !== 'done');
				if (m && m.state === 'wait' && ready(m)) return m;
			}
			free.sort((a, b) => b.weight - a.weight);
			return free[0] || null;
		}

		async function runTask(w, t) {
			t.state = 'running';
			t.start = Date.now() / 1000;
			showAll();
			if (t.needs) {
				// files other workers made: copy them in first
				for (const other of lanes) {
					if (other === w) continue;
					const got = await ask(other, { type: 'collect', globs: t.needs }, 'collected');
					if (got.files.length) await ask(w, { type: 'put', files: got.files }, 'put-done', got.files.map((f) => f.data.buffer));
				}
			}
			await ask(w, { type: 'task', task: t }, 'task-done');
			if (t.needs) await ask(w, { type: 'drop' }, 'dropped');
			if (t.step === 'rooms') w.rooms = true;
			t.state = 'done';
			t.frac = 1;
			t.end = Date.now() / 1000;
			showAll();
		}

		async function laneLoop(w) {
			for (;;) {
				if (failed) return;
				const t = next(w);
				if (t) {
					await runTask(w, t);
					continue;
				}
				// stop when nothing is left that this worker could take; else wait for what's running
				const mine = (x) => x.lane === 'any' || w.index === 0;
				if (!tasks.some((x) => x.state === 'wait' && mine(x))) return;
				await new Promise((r) => setTimeout(r, 300));
			}
		}
		await Promise.all(lanes.map((w) => laneLoop(w).catch(fail)));
		if (failed) throw failed;

		// each worker that made rooms indexes its areas; then the Enhanced Edition logs, merged
		await Promise.all(lanes.filter((w) => w.rooms).map((w) => ask(w, { type: 'task', task: { id: 'areaindex' } }, 'task-done')));
		const ee = new Set();
		for (const w of lanes) {
			const got = await ask(w, { type: 'collect', globs: ['ee_used.txt'] }, 'collected');
			for (const f of got.files) for (const l of new TextDecoder().decode(f.data).split('\n')) if (l) ee.add(l);
		}
		if (ee.size) writer.postMessage({ type: 'add', rel: 'ee_used.txt', data: new TextEncoder().encode([...ee].sort().join('\n') + '\n') });
		for (const w of lanes) await ask(w, { type: 'end' }, 'ended');
		writer.postMessage({ type: 'finish' });
		const r = await writerDone;
		if (failed) throw failed;
		return r;
	}

	$('[data-start]').addEventListener('click', async () => {
		if (!game || running || blocked) return;
		const lanesOpt = OPTIONS.lanes[state.lanes][0];
		const opt = {
			quality: OPTIONS.quality[state.quality][0],
			half: OPTIONS.half[state.half][0] === 'on',
			out: OPTIONS.out[state.out][0],
			lanes: lanesOpt === 'auto' ? autoLanes : Number(lanesOpt),
			// for the tests (tools/packer-web/e2e.mjs): ?only=rooms,james in the address runs just those steps
			only: (new URLSearchParams(location.search).get('only') || '').split(',').filter(Boolean),
		};
		if (!opt.only.length) opt.only = null;
		if (opt.out === 'dir') {
			try {
				opt.dir = await window.showDirectoryPicker({ id: 'sh2out', mode: 'readwrite' });
			} catch (e) {
				return;
			}
		} else if (canSaveFile) {
			try {
				opt.file = await window.showSaveFilePicker({ id: 'sh2zip', suggestedName: 'sh23ds-sd.zip',
					types: [{ description: 'Zip', accept: { 'application/zip': ['.zip'] } }] });
				opt.out = 'file';
			} catch (e) {
				if (e.name === 'AbortError') return;
				// a picker the browser refused: the zip is built in its storage as elsewhere
			}
		}
		if (opt.out === 'zip' && navigator.storage && navigator.storage.estimate) {
			const est = await navigator.storage.estimate();
			if (est.quota && est.quota - (est.usage || 0) < 1.6e9) {
				log('The browser says it has ' + ((est.quota - est.usage) / 1e9).toFixed(1) + ' GB of storage for this site; the zip needs about 1.5 GB.');
			}
		}
		running = true;
		showOptions();
		$('[data-start]').disabled = true;
		$('[data-pick]').disabled = true;
		$('[data-run]').hidden = false;
		$('[data-done]').hidden = true;
		stepsEl.textContent = '';
		lines.length = 0;
		tasks = [];
		showAll();
		log(`Packing ${game.name} in ${opt.lanes} worker${opt.lanes > 1 ? 's' : ''}. The files are read on this computer and never uploaded.`);
		t0 = Date.now();
		timer = setInterval(clock, 1000);
		window.onbeforeunload = () => 'The packer is still running.';
		try { lock = await navigator.wakeLock.request('screen'); } catch (e) { lock = null; }
		const run = { quality: opt.quality, half: opt.half, out: opt.out, lanes: opt.lanes, lanes_auto: lanesOpt === 'auto',
			ee: game.ee, files: game.files.length, gb: gb(game.bytes), partial: !!opt.only };
		const seconds = () => Math.round((Date.now() - t0) / 1000);
		track('packer_started', run);

		try {
			const r = await pack(opt);
			stop();
			clock();
			track('packer_finished', { ...run, seconds: seconds(), output_files: r.files });
			$('[data-status]').textContent = 'Done';
			$('[data-count]').textContent = `${r.files.toLocaleString()} files`;
			$('[data-done]').hidden = false;
			if (opt.out === 'zip') {
				const root = await navigator.storage.getDirectory();
				const file = await (await root.getFileHandle(r.name)).getFile();
				const a = $('[data-download]');
				a.href = URL.createObjectURL(file);
				a.download = 'sh23ds-sd.zip';
				a.hidden = false;
				a.onclick = () => track('packer_zip_saved', { gb: gb(file.size) });
				$('[data-size]').textContent = (file.size / 1e9).toFixed(2) + ' GB';
				$('[data-done-text]').textContent = 'Packed in ' + $('[data-elapsed]').textContent + '. Save the zip, then unzip it.';
			} else if (opt.out === 'file') {
				$('[data-done-text]').textContent = 'Packed in ' + $('[data-elapsed]').textContent + ' into ' + opt.file.name +
					' (' + (r.size / 1e9).toFixed(2) + ' GB). Unzip it.';
			} else {
				$('[data-done-text]').textContent = 'Packed in ' + $('[data-elapsed]').textContent + ', straight into the folder you picked, under 3ds/sh23ds.';
			}
			$('[data-done]').scrollIntoView({ behavior: 'smooth', block: 'center' });
		} catch (err) {
			stop();
			$('[data-status]').textContent = 'Stopped';
			log('Stopped: ' + (err && err.message || err));
			track('packer_failed', { ...run, seconds: seconds(), error: String(err && err.message || err).slice(0, 200) });
			if (err instanceof Error) trackError(err, 'packer');
		}
	});
})();
