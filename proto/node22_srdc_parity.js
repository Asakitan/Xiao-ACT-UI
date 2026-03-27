'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

function parseArgs(argv) {
  const args = {};
  for (let i = 2; i < argv.length; i += 1) {
    const key = argv[i];
    const next = argv[i + 1];
    if (key === '--file' && next) {
      args.file = next;
      i += 1;
    } else if (key === '--base64' && next) {
      args.base64 = next;
      i += 1;
    }
  }
  return args;
}

function loadInput(args) {
  if (args.file) {
    return fs.readFileSync(path.resolve(args.file));
  }
  if (args.base64) {
    return Buffer.from(args.base64, 'base64');
  }
  throw new Error('Pass --file <path> or --base64 <payload>.');
}

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function main() {
  const args = parseArgs(process.argv);
  const input = loadInput(args);
  if (typeof zlib.zstdDecompressSync !== 'function') {
    throw new Error('Node 22 zstdDecompressSync is not available.');
  }
  const output = zlib.zstdDecompressSync(input);
  process.stdout.write(JSON.stringify({
    ok: true,
    input_size: input.length,
    decompressed_size: output.length,
    sha256: sha256(output),
  }));
}

main();
