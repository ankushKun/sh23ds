// The packer's browser check (the home page's step 2): before the player picks anything, find out whether this browser can
// run the packer, and say what to do when it can't, in that browser's own terms. page.js shows the result
// and keeps the buttons off when something stops the packer.
//
// SH2Check.run() resolves to {browser, items, zip, dir}: items are {level, id, text} with level 'stop'
// (the packer can't run here), 'warn' (it can, with a catch) or 'note'; zip and dir say which outputs
// work. The page-level checks are here; what the packing workers and the writer use is tried inside a
// module worker like theirs (probe.js), so a check passes only where the real thing would.
(function (root) {
	'use strict';

	const NEED_BYTES = 1.6e9;            // the zip while it is built (writer.js), with some room
	const PROBE_MS = 45000;

	function browser() {
		const ua = navigator.userAgent;
		const brands = ((navigator.userAgentData && navigator.userAgentData.brands) || []).map((b) => b.brand);
		const has = (b) => brands.includes(b);
		if (navigator.brave || has('Brave')) return 'Brave';
		if (has('Microsoft Edge') || /\bEdg\//.test(ua)) return 'Edge';
		if (has('Opera') || /\bOPR\//.test(ua)) return 'Opera';
		if (/\bVivaldi\//.test(ua)) return 'Vivaldi';
		if (/\bSamsungBrowser\//.test(ua)) return 'Samsung Internet';
		if (/\b(Firefox|FxiOS)\//.test(ua)) return 'Firefox';
		if (has('Google Chrome') || /\b(Chrome|CriOS)\//.test(ua)) return 'Chrome';
		if (/\bSafari\//.test(ua)) return 'Safari';
		return '';
	}

	function mobile() {
		if (navigator.userAgentData && navigator.userAgentData.mobile) return true;
		if (/\b(Android|iPhone|iPad|iPod|Mobile)\b/.test(navigator.userAgent)) return true;
		// iPadOS asks for the desktop site and says it is a Mac
		return navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1;
	}

	// probe.js's answer, or {error} when the worker can't start or never answers
	function probe() {
		return new Promise((resolve) => {
			let w;
			const done = (r) => {
				clearTimeout(timer);
				if (w) w.terminate();
				resolve(r);
			};
			const timer = setTimeout(() => done({ error: 'no answer in ' + PROBE_MS / 1000 + ' s' }), PROBE_MS);
			try {
				w = new Worker('packer/probe.js', { type: 'module' });
			} catch (e) {
				done({ error: e.message || e.name });
				return;
			}
			w.onmessage = (e) => done(e.data);
			w.onerror = (e) => {
				e.preventDefault();
				done({ error: e.message || 'the worker didn’t start' });
			};
			w.postMessage('check');
		});
	}

	const gb = (n) => (n / 1e9).toFixed(1) + ' GB';
	// the browser's own words, without the URLs some of them quote, and without a closing period
	const why = (r) => {
		const d = (r.detail || '').replace(/:?\s*https?:\/\/\S+/g, '').replace(/\.$/, '').trim();
		return d ? ' (' + d + ')' : '';
	};

	async function run() {
		const name = browser();
		const it = name || 'this browser';
		const items = [];
		const add = (level, id, text) => items.push({ level, id, text });
		const dir = typeof window.showDirectoryPicker === 'function';
		// where it can, the zip is written into a file the player picks, not the browser's storage (writer.js)
		const saveFile = dir && typeof window.showSaveFilePicker === 'function';
		const pick = dir || 'webkitdirectory' in document.createElement('input');
		let zip = true;

		if (mobile()) {
			add('stop', 'mobile', 'The packer needs a computer. Phones and tablets can’t give a web page a whole ' +
				'folder, and packing takes more memory than they allow one tab. Open this page in Chrome, Edge, ' +
				'Firefox or Safari on Windows, macOS or Linux.');
			return { browser: name, items, zip: false, dir: false };
		}
		if (!window.isSecureContext) {
			add('stop', 'insecure', 'This copy of the page isn’t served over https, so the browser turns off the ' +
				'storage and workers the packer uses. Open it from the site’s own address.');
			return { browser: name, items, zip: false, dir: false };
		}
		if (!pick) {
			add('stop', 'folder', 'This browser can’t hand a folder to a web page. Use a current Chrome, Edge, ' +
				'Firefox or Safari.');
		}

		const p = await probe();
		if (p.error) {
			add('stop', 'workers', 'The browser couldn’t start the packer’s background workers (' + p.error + '). ' +
				'They are module workers, which need Chrome 80, Firefox 114, Safari 15 or newer. Update ' + it +
				' and reload.');
			return { browser: name, items, zip: false, dir };
		}

		if (!p.wasm.ok && p.wasm.why === 'off') {
			const how = {
				Safari: 'Lockdown Mode does that. Open Safari’s Settings for This Website, turn Lockdown Mode off ' +
					'for this site and reload.',
				Edge: 'Edge’s enhanced security mode can do that. Add this site as an exception under ' +
					'edge://settings/privacy (“Enhance your security on the web”) and reload.',
				Firefox: 'Check javascript.options.wasm in about:config, and any security extension.',
			}[name] || 'A security setting or an extension does that; allow it for this site and reload.';
			add('stop', 'wasm', 'WebAssembly is turned off in ' + it + ', and the packer runs on it. ' + how);
		} else if (!p.wasm.ok) {
			add('stop', 'wasm', 'The WebAssembly in ' + it + ' is too old for the Python the packer runs. ' +
				'Update to Chrome or Edge 95, Firefox 100 or Safari 15.2, or anything newer.');
		}
		if (!p.filereader.ok) {
			add('stop', 'filereader', 'The workers in ' + it + ' can’t read files the way the packer reads the ' +
				'game (FileReaderSync). Update the browser.');
		}
		if (!p.cdn.ok) {
			const blocker = name === 'Brave' ? 'Brave Shields' : 'An ad or content blocker';
			add('stop', 'cdn', 'The packer’s tools (Python and ffmpeg, about 60 MB) come from cdn.jsdelivr.net, ' +
				'and the browser couldn’t load them' + why(p.cdn) + '. ' + blocker + ', a firewall or a network ' +
				'that blocks jsDelivr can cause that. Allow cdn.jsdelivr.net for this page, check the connection ' +
				'and reload.');
		}

		if (!saveFile && !p.opfs.ok) {
			zip = false;
			const priv = p.opfs.why === 'private';
			const cause = priv
				? (name === 'Firefox' || name === 'Safari' ? name + ' gives private windows no storage for sites'
					: 'Private windows and settings that block site data do that')
				: it + ' can’t write files into its own storage';
			if (dir) {
				add('warn', 'storage', 'This window can’t build the zip in the browser’s storage' + why(p.opfs) + '. ' +
					cause + '. Save as is set to Folder instead, which writes straight into a folder you pick.');
			} else {
				add('stop', 'storage', 'This window can’t build the zip in the browser’s storage' + why(p.opfs) + '. ' +
					cause + '. ' + (priv ? 'Open this page in a normal window, or allow site data for it.'
						: 'Update ' + it + ' to its current version.'));
			}
		} else if (!saveFile && navigator.storage && navigator.storage.estimate) {
			try {
				const est = await navigator.storage.estimate();
				const free = (est.quota || 0) - (est.usage || 0);
				if (est.quota && free < NEED_BYTES) {
					add('warn', 'quota', it.charAt(0).toUpperCase() + it.slice(1) + ' offers this site ' + gb(free) +
						' of storage and the zip needs about 1.5 GB while it’s made. A private or incognito window ' +
						'gets much less than a normal one. ' + (dir ? 'Set Save as to Folder to skip the zip, or free'
							: 'Free') + ' up disk space and reload.');
				}
			} catch (e) { /* no estimate: the writer still says when the storage is full */ }
		}

		if (name === 'Brave' && !dir) {
			add('note', 'brave-fsa', 'Brave turns off direct folder access, so you pick the folder with the ' +
				'ordinary dialog, which works just as well. Saving straight to a folder needs ' +
				'brave://flags/#file-system-access-api.');
		}
		if (!('wakeLock' in navigator)) {
			add('note', 'wakelock', 'Keep the computer awake while it packs; ' + it + ' can’t keep it from sleeping ' +
				'for the page.');
		}
		return { browser: name, items, zip, dir };
	}

	root.SH2Check = { run };
})(self);
