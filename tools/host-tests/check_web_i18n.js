#!/usr/bin/env node
// Consistency check for the language table in data/index.html.
//   node check_web_i18n.js <path/to/index.html>
//
// run.sh calls it so that a key can never drift between the two languages:
// the firmware decides WHICH language the page shows (FW_LANG_CODE, reported
// by /api/status), the table below decides WHAT it says, and a key that is
// missing in one of them would fall back to the raw key name on a button.
//
// Checks, in order:
//   - the page script parses
//   - every data-i18n key in the markup exists in both languages
//   - every t('key') call in the script exists in both languages
//   - no key is defined in only one language, and none is unused
//   - no Swedish text is hard-coded outside the table (markup or script)
//
// Needs nothing but node: the table is evaluated with vm, not with jsdom.
'use strict';
const fs = require('fs');
const vm = require('vm');

const path = process.argv[2];
if (!path) {
  console.error('usage: check_web_i18n.js <path/to/index.html>');
  process.exit(2);
}
const html = fs.readFileSync(path, 'utf8');
let failures = 0;
const fail = (msg) => { failures++; console.log('FAIL ' + msg); };

const script = html.slice(html.lastIndexOf('<script>') + 8, html.lastIndexOf('</script>'));
try {
  new vm.Script(script, { filename: 'index.html#script' });
  console.log('ok   page script parses');
} catch (e) {
  fail('page script does not parse: ' + e.message);
  process.exit(1);
}

// Evaluate the I18N literal for real instead of pattern-matching it.
const start = script.indexOf('const I18N = ');
const end = script.indexOf('\n};', start);
if (start < 0 || end < 0) { fail('could not locate the I18N table'); process.exit(1); }
const sandbox = {};
vm.createContext(sandbox);
try {
  vm.runInContext(script.slice(start, end + 3) + '\nglobalThis.__I18N = I18N;', sandbox);
} catch (e) {
  fail('the I18N table does not evaluate: ' + e.message);
  process.exit(1);
}
const I18N = sandbox.__I18N;
if (!I18N || !I18N.sv || !I18N.en) { fail('expected an sv and an en table'); process.exit(1); }

const markupKeys = new Set();
for (const m of html.matchAll(/data-i18n(?:-html|-placeholder)?="([^"]+)"/g)) markupKeys.add(m[1]);
const callKeys = new Set();
for (const m of script.matchAll(/\bt\('([A-Za-z][A-Za-z0-9]*)'/g)) callKeys.add(m[1]);

for (const key of markupKeys) if (!(key in I18N.sv) || !(key in I18N.en)) fail('markup key missing: ' + key);
for (const key of callKeys) if (!(key in I18N.sv) || !(key in I18N.en)) fail('t() key missing: ' + key);

const allKeys = new Set([...Object.keys(I18N.sv), ...Object.keys(I18N.en)]);
for (const key of allKeys) {
  if (!(key in I18N.sv)) fail('key only in en: ' + key);
  if (!(key in I18N.en)) fail('key only in sv: ' + key);
  if (!markupKeys.has(key) && !callKeys.has(key)) fail('unused key: ' + key);
}
console.log('ok   ' + markupKeys.size + ' markup keys, ' + callKeys.size +
            ' t() keys, ' + allKeys.size + ' table entries, both languages');

// Swedish left in the script? The table itself is of course exempt.
const scriptNoTable = script.slice(0, start) + script.slice(end + 3);
const scriptLeftovers = [...scriptNoTable.matchAll(/[^\n]*[åäöÅÄÖ][^\n]*/g)]
  .map((m) => m[0].trim())
  .filter((line) => !line.startsWith('//'));
if (scriptLeftovers.length) {
  fail('swedish text outside the table:');
  scriptLeftovers.forEach((l) => console.log('     ' + l));
} else {
  console.log('ok   no swedish literals outside the I18N table');
}

// Swedish left in the markup? Walk the tag stack, so text inside a
// data-i18n-html element (which owns its own markup) counts as covered.
const body = html.slice(html.indexOf('<body>'), html.indexOf('<script>'));
const bodyLeftovers = [];
const stack = [];
const tagRe = /<(\/?)([a-zA-Z][^\s/>]*)((?:[^>"']|"[^"]*"|'[^']*')*?)(\/?)>/g;
let cursor = 0, m;
while ((m = tagRe.exec(body)) !== null) {
  const text = body.slice(cursor, m.index).trim();
  if (text && /[åäöÅÄÖ]/.test(text) && !stack.some((on) => on)) bodyLeftovers.push(text);
  cursor = tagRe.lastIndex;
  if (m[1]) stack.pop();
  else if (!m[4]) stack.push(m[3].includes('data-i18n'));
}
if (bodyLeftovers.length) {
  fail('swedish text in the markup without a key:');
  bodyLeftovers.forEach((l) => console.log('     ' + l));
} else {
  console.log('ok   every swedish text node in the markup carries a key');
}

console.log(failures ? failures + ' failures' : 'all web i18n checks passed');
process.exit(failures ? 1 : 0);
