import fs from 'node:fs';
import path from 'node:path';
import {deflateSync} from 'node:zlib';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
function png(size, rgba) {
  const chunk = (type, data) => {
    const tag = Buffer.from(type);
    let crc = 0xFFFFFFFF;
    for (const byte of Buffer.concat([tag, data])) {
      crc ^= byte;
      for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    const result = Buffer.alloc(12 + data.length);
    result.writeUInt32BE(data.length); tag.copy(result, 4); data.copy(result, 8);
    result.writeUInt32BE((crc ^ 0xFFFFFFFF) >>> 0, 8 + data.length);
    return result;
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(size); header.writeUInt32BE(size, 4); header[8] = 8; header[9] = 6;
  const scanlines = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; ++y) rgba.copy(scanlines, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk('IHDR', header),
    chunk('IDAT', deflateSync(scanlines)), chunk('IEND', Buffer.alloc(0))]);
}
const images = sizes.map(size => {
  if (size >= 64) {
    const rgba = Buffer.alloc(size * size * 4);
    for (let y = 0; y < size; ++y) for (let x = 0; x < size; ++x) {
      let insideSamples = 0, whiteSamples = 0;
      for (let sy = 0; sy < 4; ++sy) for (let sx = 0; sx < 4; ++sx) {
        const px = (x + (sx + 0.5) / 4) * 32 / size, py = (y + (sy + 0.5) / 4) * 32 / size;
        const cx = Math.max(6 - px, px - 26, 0), cy = Math.max(6 - py, py - 26, 0);
        if (cx * cx + cy * cy > 36) continue;
        ++insideSamples;
        const letter = (px >= 7 && px < 11 && py >= 7 && py < 25) ||
          (px >= 7 && px < 20 && py >= 21 && py < 25);
        const arrow = (px >= 16 && px < 27 && py >= 12 && py < 15) ||
          (px >= 22 && px < 28 && Math.abs(Math.abs(py - 13.5) - (27 - px)) < 1);
        if (letter || arrow) ++whiteSamples;
      }
      const index = (y * size + x) * 4;
      for (const [channel, background] of [36, 94, 115].entries())
        rgba[index + channel] = insideSamples ? Math.round((255 * whiteSamples + background * (insideSamples - whiteSamples)) / insideSamples) : 0;
      rgba[index + 3] = Math.round(255 * insideSamples / 16);
    }
    return png(size, rgba);
  }
  const maskRow = Math.ceil(size / 32) * 4;
  const bitmap = Buffer.alloc(40 + size * size * 4 + maskRow * size);
  bitmap.writeUInt32LE(40, 0); bitmap.writeInt32LE(size, 4); bitmap.writeInt32LE(size * 2, 8);
  bitmap.writeUInt16LE(1, 12); bitmap.writeUInt16LE(32, 14);
  bitmap.writeUInt32LE(size * size * 4, 20);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const px = Math.floor(x * 32 / size), py = Math.floor(y * 32 / size);
    const cx = px < 6 ? 6 - px : px > 25 ? px - 25 : 0;
    const cy = py < 6 ? 6 - py : py > 25 ? py - 25 : 0;
    const inside = cx * cx + cy * cy <= 36;
    const letter = (px >= 7 && px <= 10 && py >= 7 && py <= 24) ||
                   (px >= 7 && px <= 19 && py >= 21 && py <= 24);
    const arrow = (px >= 16 && px <= 26 && py >= 12 && py <= 14) ||
                  (px >= 22 && px <= 26 && (py === 13 - (26 - px) || py === 13 + (26 - px)));
    const color = inside ? (letter || arrow ? 0xFFFFFFFF : 0xFF245E73) : 0;
    bitmap.writeUInt32LE(color >>> 0, 40 + ((size - 1 - y) * size + x) * 4);
    if (!inside) bitmap[40 + size * size * 4 + (size - 1 - y) * maskRow + Math.floor(x / 8)] |= 0x80 >> (x % 8);
  }
  return bitmap;
});
const header = Buffer.alloc(6 + 16 * sizes.length);
header.writeUInt16LE(1, 2); header.writeUInt16LE(sizes.length, 4);
let offset = header.length;
for (let i = 0; i < sizes.length; i++) {
  const entry = 6 + i * 16;
  header[entry] = header[entry + 1] = sizes[i] === 256 ? 0 : sizes[i];
  header.writeUInt16LE(1, entry + 4); header.writeUInt16LE(32, entry + 6);
  header.writeUInt32LE(images[i].length, entry + 8); header.writeUInt32LE(offset, entry + 12);
  offset += images[i].length;
}
const data = Buffer.concat([header, ...images]);
const target = path.join(root, 'resources/app.ico');
if (!fs.existsSync(target) || !fs.readFileSync(target).equals(data)) fs.writeFileSync(target, data);
const logo = path.join(root, 'resources/logo.png');
if (!fs.existsSync(logo) || !fs.readFileSync(logo).equals(images.at(-1))) fs.writeFileSync(logo, images.at(-1));
