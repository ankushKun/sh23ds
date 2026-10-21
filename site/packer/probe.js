// The browser check's worker (check.js starts it as a module worker, as the page starts the packing
// workers and the writer). It tries what those use, inside a worker, and answers one message:
//   {wasm, filereader, opfs, cdn}, each {ok, why, detail}
// why says what kind of failure it was, for check.js to word the message: 'off' (turned off), 'old'
// (missing from this browser), 'private' (refused, as a private window does), 'blocked' (couldn't load).
import { PYODIDE_URL, FFMPEG_URL } from './cdn.js';

// One function with a legacy try/catch: Pyodide is built with that form of WebAssembly exception
// handling and won't load without it (pyodide.asm.wasm fails to validate in V8 with legacy EH off and
// validates with exnref off). Safari 15.2, Firefox 100 and Chrome 95 have it.
const LEGACY_EH = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 13, 3, 1, 0, 0,
	10, 9, 1, 7, 0, 6, 64, 7, 0, 11, 11]);
const EMPTY = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);

const ok = { ok: true };
const no = (why, e) => ({ ok: false, why, detail: e ? String(e.message || e.name || e) : '' });

function timeout(ms, what) {
	return new Promise((_, reject) => setTimeout(() => reject(new Error(what + ' took over ' + ms / 1000 + ' s')), ms));
}

async function wasm() {
	if (typeof WebAssembly !== 'object') return no('off');
	try {
		await WebAssembly.instantiate(EMPTY);
	} catch (e) {
		return no('off', e);
	}
	return WebAssembly.validate(LEGACY_EH) ? ok : no('old');
}

async function opfs() {
	// what writer.js does for the zip: a file in the site's private storage, written through a sync handle
	if (!navigator.storage || !navigator.storage.getDirectory) return no('old');
	let root;
	try {
		root = await navigator.storage.getDirectory();
	} catch (e) {
		return no('private', e);
	}
	const name = 'sh23ds-check-' + Date.now().toString(36);
	try {
		const fh = await root.getFileHandle(name, { create: true });
		if (!fh.createSyncAccessHandle) return no('old');
		const h = await fh.createSyncAccessHandle();
		const n = h.write(new Uint8Array(1), { at: 0 });
		h.close();
		return n === 1 ? ok : no('private');
	} catch (e) {
		return no('private', e);
	} finally {
		try { await root.removeEntry(name); } catch (e) { /* never made */ }
	}
}

async function cdn(withWasm) {
	// the loader, imported as the packing workers do (only fetched when WebAssembly is off: it uses it
	// as it loads), then the two big files the workers fetch later
	try {
		if (withWasm) {
			const m = await Promise.race([import(PYODIDE_URL + 'pyodide.mjs'), timeout(20000, 'jsDelivr')]);
			if (typeof m.loadPyodide !== 'function') return no('blocked', 'pyodide.mjs has no loadPyodide');
		}
		const urls = [PYODIDE_URL + 'pyodide.asm.wasm', FFMPEG_URL + 'ffmpeg-core.wasm'];
		if (!withWasm) urls.unshift(PYODIDE_URL + 'pyodide.mjs');
		for (const url of urls) {
			const r = await Promise.race([fetch(url, { method: 'HEAD' }), timeout(20000, 'jsDelivr')]);
			if (!r.ok) return no('blocked', url.split('/').pop() + ': HTTP ' + r.status);
		}
	} catch (e) {
		return no('blocked', e);
	}
	return ok;
}

self.onmessage = async () => {
	const w = await wasm();
	const [o, c] = await Promise.all([opfs(), cdn(w.ok)]);
	self.postMessage({ wasm: w, filereader: typeof FileReaderSync === 'function' ? ok : no('old'), opfs: o, cdn: c });
};
