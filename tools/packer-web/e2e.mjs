// The browser packer end to end in a real browser: serves site/ on localhost, opens packer.html in
// headless Chrome, gives it the game folder through the page's folder input (as a person choosing it),
// presses Start and saves the zip through Chrome's download, as the page's button does.
//
//   node tools/packer-web/e2e.mjs "<Silent Hill 2 folder>" <download folder> [--only=collision,james] [--best] [--shots=dir]
//     [--url=https://ankush.one/sh23ds/packer.html]   the deployed page instead of site/ (analytics blocked)
//     [--nosimd]     ffmpeg's build without WebAssembly SIMD (packer.html?simd=0), as on a processor without SSE4.1
//     [--savefile]   the zip written into a picked file (the picker answered with a file in the site's storage,
//                    which is then downloaded), not built in the storage and saved with the page's button
//
// Needs Google Chrome (CHROME=<path> elsewhere), `npm install` here, and site/packer/sh2conv.zip
// (bundle.py). Prints the page's progress as it goes; exits 0 when the zip was saved.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.join(HERE, '..', '..', 'site');
const args = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const only = (process.argv.find((a) => a.startsWith('--only=')) || '').slice(7);
const best = process.argv.includes('--best');
const shots = (process.argv.find((a) => a.startsWith('--shots=')) || '').slice(8);   // a screenshot of the progress panel every poll
const url = (process.argv.find((a) => a.startsWith('--url=')) || '').slice(6);
const nosimd = process.argv.includes('--nosimd');
const savefile = process.argv.includes('--savefile');
const [gameDir, outDir] = args.map((a) => path.resolve(a));
fs.mkdirSync(outDir, { recursive: true });

const PORT = 8000 + Math.floor(Math.random() * 900);
const server = url ? null : spawn('python3', ['-m', 'http.server', String(PORT), '--bind', '127.0.0.1', '--directory', SITE],
	{ stdio: 'ignore' });
if (server) await new Promise((r) => setTimeout(r, 800));
const browser = await puppeteer.launch({
	executablePath: process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
	headless: true,
	// the deployed page's analytics host doesn't resolve, so test runs stay out of the site's numbers
	args: ['--no-first-run', '--no-default-browser-check', ...(url ? ['--host-resolver-rules=MAP p1.ankush.one ~NOTFOUND'] : [])],
});
let code = 1;
try {
	const page = await browser.newPage();
	const cdp = await page.createCDPSession();
	await cdp.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: outDir, eventsEnabled: true });
	page.on('pageerror', (e) => console.log('page error:', e.message));
	const query = new URLSearchParams();
	if (only) query.set('only', only);
	if (nosimd) query.set('simd', '0');
	// the pickers are dialogs headless Chrome can't answer: before the page's scripts run, take the save
	// picker away (the zip is then built in the storage) or answer it with a file in the site's storage
	await page.evaluateOnNewDocument((save) => {
		window.showSaveFilePicker = save
			? async () => (await navigator.storage.getDirectory()).getFileHandle('picked.zip', { create: true })
			: undefined;
	}, savefile);
	await page.goto(`${url || `http://127.0.0.1:${PORT}/packer.html`}${query.size ? '?' + query : ''}`);
	// the folder input: Chrome expands a folder path for webkitdirectory inputs, with relative paths
	await page.evaluate(() => { window.showDirectoryPicker = undefined; });
	const input = await page.$('[data-pick-input]');
	await input.uploadFile(gameDir);
	await page.waitForFunction(() => !document.querySelector('[data-start]').disabled, { timeout: 120000 });
	console.log(await page.$eval('[data-found]', (e) => e.textContent));
	if (best) await page.click('[data-option="quality"] [data-step="1"]');
	const t0 = Date.now();
	await page.click('[data-start]');
	let last = '';
	for (;;) {
		await new Promise((r) => setTimeout(r, 5000));
		const s = await page.evaluate(() => ({
			status: document.querySelector('[data-status]').textContent,
			count: document.querySelector('[data-count]').textContent,
			log: document.querySelector('[data-log]').textContent.split('\n').slice(-1)[0],
		}));
		if (shots) {
			fs.mkdirSync(shots, { recursive: true });
			const el = await page.$('[data-run]');
			await el.screenshot({ path: path.join(shots, `run-${String(Math.round((Date.now() - t0) / 1000)).padStart(5, '0')}.png`) });
		}
		const line = `${s.status} ${s.count} | ${s.log}`;
		if (line !== last) console.log(`${Math.round((Date.now() - t0) / 1000)} s  ${line}`);
		last = line;
		if (s.status === 'Done' || s.status === 'Stopped') break;
		if (Date.now() - t0 > (Number(process.env.E2E_TIMEOUT) || 7200) * 1000) { console.log('timed out'); break; }
	}
	if (last.startsWith('Done')) {
		console.log(await page.$eval('[data-done-text]', (e) => e.textContent));
		const done = new Promise((resolve) => cdp.on('Browser.downloadProgress', (e) => {
			if (e.state === 'completed' || e.state === 'canceled') resolve(e.state);
		}));
		if (savefile) {
			// the picked file, downloaded through a link of its own
			await page.evaluate(async () => {
				const f = await (await (await navigator.storage.getDirectory()).getFileHandle('picked.zip')).getFile();
				const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(f), download: 'sh23ds-sd.zip' });
				document.body.appendChild(a);
				a.click();
			});
		} else {
			await page.click('[data-download]');
		}
		const st = await done;
		const name = 'sh23ds-sd.zip';
		console.log('download', st, path.join(outDir, name), fs.existsSync(path.join(outDir, name)) ? fs.statSync(path.join(outDir, name)).size : 0);
		code = st === 'completed' ? 0 : 1;
	} else {
		console.log(await page.$eval('[data-log]', (e) => e.textContent.split('\n').slice(-30).join('\n')));
	}
} finally {
	await browser.close();
	server?.kill();
}
process.exit(code);
