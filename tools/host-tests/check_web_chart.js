#!/usr/bin/env node
// Executes the roast graph's time-base logic from data/index.html.
//   node check_web_chart.js <path/to/index.html>
//
// Why it exists: on the real machine a roast that was stopped with "Avbryt och
// kyl" made the whole plot jump back to 0 min. The cause was the firmware's
// roast clock, which drops to 0 the moment a run ends, being used directly as
// the chart's x value - the finished curve was folded back onto the origin.
//
// The fix lives in two small functions in the page (chartTimeFor/chartMaxT).
// This check slices those two functions out of data/index.html with vm and runs
// them against a status sequence shaped like the firmware's, so the behaviour is
// executed rather than argued about - no browser, no DOM.
'use strict';
const fs = require('fs');
const vm = require('vm');

const path = process.argv[2];
if (!path) {
  console.error('usage: check_web_chart.js <path/to/index.html>');
  process.exit(2);
}
const html = fs.readFileSync(path, 'utf8');
const script = html.slice(html.lastIndexOf('<script>') + 8, html.lastIndexOf('</script>'));

let failures = 0;
let checks = 0;
const fail = (msg) => { failures++; console.log('FAIL ' + msg); };
const check = (ok, what, detail) => {
  checks++;
  if (ok) console.log('ok   ' + what + (detail ? ' (' + detail + ')' : ''));
  else fail(what + (detail ? ' (' + detail + ')' : ''));
};

// Pull the two function bodies out of the page and evaluate them together with
// nothing else, so the check cannot accidentally depend on the rest of the UI.
function slice(name) {
  const start = script.indexOf('function ' + name + '(');
  if (start < 0) throw new Error('function ' + name + ' not found in data/index.html');
  let depth = 0;
  let i = script.indexOf('{', start);
  const from = i;
  for (; i < script.length; i++) {
    if (script[i] === '{') depth++;
    else if (script[i] === '}') { depth--; if (depth === 0) break; }
  }
  return script.slice(start, i + 1);
}

const sandbox = {};
const src = slice('chartTimeFor') + '\n' + slice('chartMaxT') + '\nthis.chartTimeFor = chartTimeFor; this.chartMaxT = chartMaxT;';
try {
  vm.createContext(sandbox);
  vm.runInContext(src, sandbox, { filename: 'index.html#chart' });
  console.log('ok   chartTimeFor and chartMaxT are present and parse');
} catch (e) {
  fail('the chart time-base functions do not evaluate: ' + e.message);
  console.log('\n1 checks, 1 failures');
  process.exit(1);
}
const chartTimeFor = sandbox.chartTimeFor;
const chartMaxT = sandbox.chartMaxT;

// The state object as the page declares it (the three fields the logic uses).
const newState = () => ({ chartSpanT: 0, chartWasActive: false, chartNewRoast: false });
const status = (o) => Object.assign({ roastActive: false, elapsedSeconds: 0, coolActive: false }, o);

// 1. A roast running: the sample time is the roast clock, and it advances.
{
  const st = newState();
  const times = [];
  for (let s = 0; s <= 420; s += 60) times.push(chartTimeFor(status({ roastActive: true, elapsedSeconds: s }), st));
  check(times.join(',') === '0,60,120,180,240,300,360,420',
        'a running roast plots at its own clock', times.join(','));
  check(st.chartSpanT === 420, 'and the span follows it to the last sample', 'span ' + st.chartSpanT);
  check(st.chartNewRoast === true, 'the first active poll of a run is a new roast', 'flag ' + st.chartNewRoast);
}

// 2. Stop after 425 s ("Avbryt och kyl"): the clock goes to 0, the samples must
//    NOT. This is the reported bug: every later sample used to plot at x = 0.
{
  const st = newState();
  for (let s = 0; s <= 425; s += 425) chartTimeFor(status({ roastActive: true, elapsedSeconds: s }), st);
  const after = [];
  const cooling = status({ roastActive: false, elapsedSeconds: 0, coolActive: true });
  for (let i = 0; i < 5; i++) after.push(chartTimeFor(cooling, st));
  const idle = status({});
  after.push(chartTimeFor(idle, st));
  check(after.every((t) => t === 425),
        'a stopped roast keeps its time base - cooling and idle samples stay at 425 s',
        after.join(','));
  check(chartMaxT(0, after[after.length - 1], st.chartSpanT) === 425,
        'and the axis still ends at 425 s, not at 0', 'maxT ' + chartMaxT(0, 425, st.chartSpanT));
  check(chartMaxT(0, 0, st.chartSpanT) === 425,
        'even before any sample has been appended', 'maxT ' + chartMaxT(0, 0, st.chartSpanT));
}

// 3. A new roast starts: the span resets (its own time base), and the page is
//    told to clear the history the old curve was drawn from.
{
  const st = newState();
  chartTimeFor(status({ roastActive: true, elapsedSeconds: 300 }), st);
  chartTimeFor(status({ roastActive: false, elapsedSeconds: 0, coolActive: true }), st);
  const t0 = chartTimeFor(status({ roastActive: true, elapsedSeconds: 0 }), st);
  check(t0 === 0 && st.chartSpanT === 0 && st.chartNewRoast === true,
        'a new roast starts from 0 s with its own span',
        't ' + t0 + ', span ' + st.chartSpanT + ', new ' + st.chartNewRoast);
  const later = chartTimeFor(status({ roastActive: true, elapsedSeconds: 45 }), st);
  check(later === 45 && st.chartNewRoast === true,
        'and the new-roast flag survives until the caller clears the history',
        't ' + later);
}

// 4. Paused roast: roastActive stays true and the clock is frozen, so the curve
//    holds still instead of collapsing.
{
  const st = newState();
  chartTimeFor(status({ roastActive: true, elapsedSeconds: 200 }), st);
  const a = chartTimeFor(status({ roastActive: true, elapsedSeconds: 200 }), st);
  const b = chartTimeFor(status({ roastActive: true, elapsedSeconds: 200 }), st);
  check(a === 200 && b === 200 && st.chartSpanT === 200,
        'a paused roast holds its position', a + ',' + b);
}

// 5. The axis never shrinks and never goes below the old 300 s minimum.
{
  check(chartMaxT(0, 0, 0) === 300, 'an empty chart keeps the 300 s minimum', 'maxT ' + chartMaxT(0, 0, 0));
  check(chartMaxT(480, 90, 0) === 480, 'a longer profile sets the axis', 'maxT ' + chartMaxT(480, 90, 0));
  check(chartMaxT(0, 90, 425) === 425, 'the finished roast wins over a short last sample',
        'maxT ' + chartMaxT(0, 90, 425));
}

console.log('\n' + checks + ' checks, ' + failures + ' failures');
process.exit(failures === 0 ? 0 : 1);
