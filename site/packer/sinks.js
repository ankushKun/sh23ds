// Where the browser packer's output goes. Each sink has add(path, data) and finish(); data is a
// Uint8Array or a Blob. A path added twice keeps the last copy.
//
// ZipSink writes a stored (uncompressed) zip through a write(bytes, offset) function, so the same code
// writes to the browser's private storage (worker.js) or a file (the Node tests). The pack's files are
// already compressed or small, so deflate would cost minutes for little. Offsets past 4 GB get zip64
// records. DirSink writes the files into a folder the player picked (Chromium's File System Access).
(function (root) {
	'use strict';

	const CRC = new Uint32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		CRC[n] = c >>> 0;
	}
	function crc32(u8) {
		let c = 0xffffffff;
		for (let i = 0; i < u8.length; i++) c = CRC[(c ^ u8[i]) & 0xff] ^ (c >>> 8);
		return (c ^ 0xffffffff) >>> 0;
	}

	async function bytesOf(data) {
		return data instanceof Uint8Array ? data : new Uint8Array(await data.arrayBuffer());
	}

	function dosTime(d) {
		const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
		const date = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
		return [time, date];
	}

	class ZipSink {
		constructor(write, prefix) {
			this.write = write;              // write(Uint8Array, offset), may return a promise
			this.prefix = prefix || '';      // e.g. "3ds/sh23ds/"
			this.offset = 0;
			this.entries = new Map();        // name -> {crc, size, offset}
			[this.time, this.date] = dosTime(new Date());
		}

		async put(u8) {
			await this.write(u8, this.offset);
			this.offset += u8.length;
		}

		async add(path, data) {
			const u8 = await bytesOf(data);
			const name = new TextEncoder().encode(this.prefix + path);
			const crc = crc32(u8);
			const big = this.offset > 0xfffffffe;
			const h = new DataView(new ArrayBuffer(30));
			h.setUint32(0, 0x04034b50, true);
			h.setUint16(4, big ? 45 : 20, true);
			h.setUint16(6, 0x0800, true);    // UTF-8 names
			h.setUint16(8, 0, true);         // stored
			h.setUint16(10, this.time, true);
			h.setUint16(12, this.date, true);
			h.setUint32(14, crc, true);
			h.setUint32(18, u8.length, true);
			h.setUint32(22, u8.length, true);
			h.setUint16(26, name.length, true);
			h.setUint16(28, 0, true);
			this.entries.set(this.prefix + path, { name, crc, size: u8.length, offset: this.offset });
			await this.put(new Uint8Array(h.buffer));
			await this.put(name);
			await this.put(u8);
		}

		async finish() {
			const cdStart = this.offset;
			let count = 0;
			for (const e of this.entries.values()) {
				const z64 = e.offset > 0xfffffffe;
				const extra = z64 ? 12 : 0;
				const h = new DataView(new ArrayBuffer(46 + extra));
				h.setUint32(0, 0x02014b50, true);
				h.setUint16(4, 0x031e, true);  // made by: Unix, 3.0
				h.setUint16(6, z64 ? 45 : 20, true);
				h.setUint16(8, 0x0800, true);
				h.setUint16(10, 0, true);
				h.setUint16(12, this.time, true);
				h.setUint16(14, this.date, true);
				h.setUint32(16, e.crc, true);
				h.setUint32(20, e.size, true);
				h.setUint32(24, e.size, true);
				h.setUint16(28, e.name.length, true);
				h.setUint16(30, extra, true);
				h.setUint32(38, 0o100644 << 16, true);   // a regular file, rw-r--r--
				h.setUint32(42, z64 ? 0xffffffff : e.offset, true);
				await this.put(new Uint8Array(h.buffer, 0, 46));
				await this.put(e.name);
				if (z64) {
					const x = new DataView(new ArrayBuffer(12));
					x.setUint16(0, 1, true);
					x.setUint16(2, 8, true);
					x.setBigUint64(4, BigInt(e.offset), true);
					await this.put(new Uint8Array(x.buffer));
				}
				count++;
			}
			const cdSize = this.offset - cdStart;
			if (cdStart > 0xfffffffe || count > 0xfffe) {
				const z = new DataView(new ArrayBuffer(56 + 20));
				const at = this.offset;
				z.setUint32(0, 0x06064b50, true);
				z.setBigUint64(4, 44n, true);
				z.setUint16(12, 45, true);
				z.setUint16(14, 45, true);
				z.setBigUint64(24, BigInt(count), true);
				z.setBigUint64(32, BigInt(count), true);
				z.setBigUint64(40, BigInt(cdSize), true);
				z.setBigUint64(48, BigInt(cdStart), true);
				z.setUint32(56, 0x07064b50, true);
				z.setBigUint64(64, BigInt(at), true);
				z.setUint32(72, 1, true);
				await this.put(new Uint8Array(z.buffer));
			}
			const end = new DataView(new ArrayBuffer(22));
			end.setUint32(0, 0x06054b50, true);
			end.setUint16(8, Math.min(count, 0xffff), true);
			end.setUint16(10, Math.min(count, 0xffff), true);
			end.setUint32(12, Math.min(cdSize, 0xffffffff), true);
			end.setUint32(16, Math.min(cdStart, 0xffffffff), true);
			await this.put(new Uint8Array(end.buffer));
			return this.offset;
		}
	}

	class DirSink {
		constructor(dirHandle, prefix) {
			this.root = dirHandle;
			this.prefix = prefix || '';
			this.dirs = new Map();
		}

		async dir(parts) {
			let d = this.root, key = '';
			for (const p of parts) {
				key += '/' + p;
				if (!this.dirs.has(key)) this.dirs.set(key, await d.getDirectoryHandle(p, { create: true }));
				d = this.dirs.get(key);
			}
			return d;
		}

		async add(path, data) {
			const parts = (this.prefix + path).split('/');
			const d = await this.dir(parts.slice(0, -1));
			const fh = await d.getFileHandle(parts[parts.length - 1], { create: true });
			const w = await fh.createWritable();
			await w.write(data);
			await w.close();
		}

		async finish() {}
	}

	root.SH2Sinks = { ZipSink, DirSink, crc32 };
	if (typeof module !== 'undefined') module.exports = root.SH2Sinks;
})(typeof self !== 'undefined' ? self : globalThis);
