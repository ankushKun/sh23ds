// The browser packer's writer (a module worker that page.js starts): the one place the output is
// written, so several packing workers can run at once. Each packing worker sends its files here over its
// own MessageChannel; this worker adds them, in the order each one sent them, to a zip: written straight
// into a file the player picked where the browser can (Chrome, Edge), else built in the browser's private
// storage (OPFS) for the page to save; or to a folder the player picked.
//
// Messages in:  {type: 'open', out: 'zip' | 'file' | 'dir', dir, file, lanes}   then one {type: 'port', port} per worker
//               {type: 'add', rel, data}                           a file from the page itself
//               {type: 'finish'}                                   after every worker sent its 'end'
//               {type: 'abort'}                                    a failed run: close the zip's handle
// Messages out: {type: 'ready'} {type: 'done', name, size, files, bytes} {type: 'aborted'} {type: 'error', message}
import './sinks.js';    // self.SH2Sinks

// A sync access handle is exclusive, so each run writes a zip of its own name: a handle a run left open
// (a tab that stopped, a worker the browser hasn't torn down yet) can't block the next one. The page
// saves it as sh23ds-sd.zip whatever it is called here.
const ZIP_PREFIX = 'sh23ds-sd';
const ZIP_NAME = ZIP_PREFIX + '-' + Date.now().toString(36) + '.zip';
const post = (m) => self.postMessage(m);
let sink = null, handle = null, writable = null, queue = Promise.resolve(), files = 0, bytes = 0;
let lanes = 0, ended = 0, finishing = null;
const listed = new Set();   // what files.txt lists: every file but the inspection extras

// pack.py write_list: files.txt, the pack's own file list, added last. sh23ds names the files an SD
// card is missing from it (port/source/nodata.c). The extras are left out as pack.py is_extra does.
function fileList() {
	const extra = (rel) => rel.split('/').slice(0, -1).includes('png') || rel.endsWith('.json');
	const rels = [...listed].filter((rel) => !extra(rel)).sort();
	return new TextEncoder().encode('# sh23ds: every file of this pack. sh23ds names the ones an SD card is missing.\n' +
		rels.map((rel) => rel + '\n').join(''));
}

async function zipSink() {
	let root;
	try {
		root = await navigator.storage.getDirectory();
		await root.getFileHandle('sh23ds-probe', { create: true });
		await root.removeEntry('sh23ds-probe');
	} catch (e) {
		// a private window (Safari's in particular) has no storage for sites
		throw new Error('This browser window can’t store the zip (' + (e.message || e.name) + '). ' +
			'A private window does that: use a normal window, or in Chrome and Edge save straight to a folder.');
	}
	// the zips earlier runs left behind; one still open somewhere refuses and stays
	for await (const name of root.keys()) {
		if (name.startsWith(ZIP_PREFIX) && name.endsWith('.zip')) {
			try { await root.removeEntry(name); } catch (e) { /* in use */ }
		}
	}
	const fh = await root.getFileHandle(ZIP_NAME, { create: true });
	handle = await fh.createSyncAccessHandle();
	const z = new SH2Sinks.ZipSink((u8, at) => {
		let n;
		try {
			n = handle.write(u8, { at });
		} catch (e) {
			n = -1;
		}
		if (n !== u8.length) throw new Error(FULL);
	}, '3ds/sh23ds/');
	const finish = z.finish.bind(z);
	z.finish = async () => {
		const size = await finish();
		handle.flush();
		handle.close();
		handle = null;
		return size;
	};
	return z;
}

const FULL = 'The browser ran out of room for the zip in its own storage. Free up some disk space and start ' +
	'again, or pack in Chrome or Edge, which write the zip straight to where you pick.';

// the zip straight into the file the player picked: no copy in the browser's storage, and nothing to save
// at the end. The browser writes it beside the file as name.crswap and puts it in place when it's done.
async function fileSink(fh) {
	writable = await fh.createWritable();
	const z = new SH2Sinks.ZipSink(async (u8, at) => {
		try {
			await writable.write({ type: 'write', position: at, data: u8 });
		} catch (e) {
			throw new Error(e.name === 'QuotaExceededError' ? 'The disk ran out of room for the zip. Free up some space, or pick another disk, and start again.'
				: 'Writing the zip failed (' + (e.message || e.name) + ').');
		}
	}, '3ds/sh23ds/');
	const finish = z.finish.bind(z);
	z.finish = async () => {
		const size = await finish();
		await writable.close();
		writable = null;
		return size;
	};
	return z;
}

function add(rel, data) {
	queue = queue.then(async () => {
		await sink.add(rel, data);
		listed.add(rel);
		files++;
		bytes += data.size !== undefined ? data.size : data.length;
	}).catch((err) => post({ type: 'error', message: String(err && err.message || err) }));
}

function maybeFinish() {
	if (!finishing || ended < lanes) return;
	queue.then(async () => {
		const list = fileList();
		await sink.add('files.txt', list);
		files++;
		bytes += list.length;
		const size = await sink.finish();
		post({ type: 'done', name: ZIP_NAME, size, files, bytes });
	}).catch((err) => post({ type: 'error', message: String(err && err.message || err) }));
}

self.onmessage = async (e) => {
	const m = e.data;
	try {
		if (m.type === 'open') {
			lanes = m.lanes;
			sink = m.out === 'dir' ? new SH2Sinks.DirSink(m.dir, '3ds/sh23ds/')
				: m.out === 'file' ? await fileSink(m.file) : await zipSink();
			post({ type: 'ready' });
		} else if (m.type === 'port') {
			m.port.onmessage = (ev) => {
				if (ev.data.type === 'end') {
					ended++;
					maybeFinish();
				} else {
					add(ev.data.rel, ev.data.data);
				}
			};
		} else if (m.type === 'add') {
			add(m.rel, m.data);
		} else if (m.type === 'finish') {
			finishing = true;
			maybeFinish();
		} else if (m.type === 'abort') {
			if (handle) {
				try { handle.close(); } catch (e) { /* already closed */ }
				handle = null;
			}
			if (writable) {
				// leaves the picked file as it was, with no half-written zip
				try { await writable.abort(); } catch (e) { /* already closed */ }
				writable = null;
			}
			post({ type: 'aborted' });
		}
	} catch (err) {
		post({ type: 'error', message: String(err && err.message || err) });
	}
};
