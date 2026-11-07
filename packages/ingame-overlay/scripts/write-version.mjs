// Writes the tosu version into a packed overlay directory.
// The updater (packages/ingame-overlay-updater) reinstalls the overlay when this
// file is missing or does not match the running tosu version.
//
//   node scripts/write-version.mjs <pack dir>
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const target = process.argv[2];
if (!target) {
    console.error('usage: node scripts/write-version.mjs <pack dir>');
    process.exit(1);
}

const root = path.dirname(fileURLToPath(import.meta.url));
const tosuPackage = path.join(root, '../../tosu/package.json');
const { version } = JSON.parse(readFileSync(tosuPackage, 'utf8'));

writeFileSync(path.join(target, 'version'), `${version}\n`);
console.log(`wrote version ${version} to ${target}`);
