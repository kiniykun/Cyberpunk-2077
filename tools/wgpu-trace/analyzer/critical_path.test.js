import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { analyze, analyzeFrame, indexRecords, schedule, T, WK, SK } from './critical_path.js';

const R = (type, frameId, a, b, c, t0, t1) => ({ type, frameId, a, b, c, tid: 0, t0, t1 });
const near = (x, y, eps = 1e-6) => assert.ok(Math.abs(x - y) < eps, `expected ${x} ≈ ${y}`);
const sum = (p) => p.reduce((s, e) => s + e.ms, 0);
const ids = (p) => p.map((e) => e.id);
const assertNoDuplicates = (p) => {
  const seen = new Set();
  for (const e of p) { assert.ok(!seen.has(e.id), `duplicate path entry ${e.id}`); seen.add(e.id); }
};
const frameOf = (records, frameId) => {
  const ix = indexRecords(records);
  return analyzeFrame(ix, ix.frames.get(frameId), {});
};

test('parallel jobs: the longer branch is on the path, the other has slack, no double counting', () => {
  const recs = [
    R(T.FRAME, 1, 0, 0, 0, 0, 30),
    R(T.JOB_RUN, 1, 1, 0, 0, 0, 30),
    R(T.JOB_SPAWN, 1, 2, 1, 0, 1, 0),
    R(T.JOB_SPAWN, 1, 3, 1, 0, 1.5, 0),
    R(T.JOB_RUN, 1, 2, 1, 0, 2, 20),
    R(T.JOB_RUN, 1, 3, 2, 0, 2, 12),
    R(T.WAIT, 1, 1, WK.JOB, 2, 2, 21),
    R(T.WAIT, 1, 1, WK.JOB, 3, 21, 22),
  ];
  const r = frameOf(recs, 1);
  near(sum(r.path), 30);
  near(r.pathMs, r.frameMs);
  assertNoDuplicates(r.path);
  assert.deepEqual(r.path.map((e) => e.cat), ['run:w0', 'sched', 'run:w1', 'wake:job', 'wake:job', 'run:w0']);
  near(r.path[2].ms, 18);
  assert.ok(!ids(r.path).includes('run:3:0'));
  const s = schedule(r.graph);
  near(s.ls.get('run:3:0') - s.es.get('run:3:0'), 9);
  near(s.ls.get('run:2:0') - s.es.get('run:2:0'), 0);
  near(s.end, 30);
  assert.equal(r.unresolvedMs, 0);
  assert.equal(r.inconsistencies.length, 0);
});

test('diamond: critical branch has zero slack, shortening it is bounded by the other branch', () => {
  const recs = [
    R(T.FRAME, 2, 0, 0, 0, 100, 120),
    R(T.JOB_RUN, 2, 11, 0, 0, 100, 105),
    R(T.JOB_RUN, 2, 12, 0, 0, 105, 115),
    R(T.JOB_RUN, 2, 13, 1, 0, 105, 110),
    R(T.JOB_RUN, 2, 14, 0, 0, 115, 120),
    R(T.JOB_DEP, 2, 12, 11, 0, 0, 0),
    R(T.JOB_DEP, 2, 13, 11, 0, 0, 0),
    R(T.JOB_DEP, 2, 14, 12, 0, 0, 0),
    R(T.JOB_DEP, 2, 14, 13, 0, 0, 0),
  ];
  const r = frameOf(recs, 2);
  near(sum(r.path), 20);
  assert.deepEqual(ids(r.path), ['run:11:0', 'run:12:0', 'run:14:0']);
  const base = schedule(r.graph);
  near(base.ls.get('run:13:0') - base.es.get('run:13:0'), 5);
  near(base.ls.get('run:12:0') - base.es.get('run:12:0'), 0);
  const shortened = schedule(r.graph, new Map([['run:12:0', 0]]));
  near(base.end - shortened.end, 5);
  const shortenedLess = schedule(r.graph, new Map([['run:12:0', 7]]));
  near(base.end - shortenedLess.end, 3);
});

test('out-of-order callbacks: a wait is attributed to its own scope settle, not the earlier-settling one', () => {
  const recs = [
    R(T.FRAME, 3, 0, 0, 0, 0, 40),
    R(T.JOB_RUN, 3, 21, 0, 0, 0, 40),
    R(T.WAIT, 3, 21, WK.PROXY, 10, 1, 3),
    R(T.PROXY_DEQ, 3, 10, 0, 0, 0, 1.5),
    R(T.PROXY_CALL, 3, 10, 1, 0, 1.6, 2.6),
    R(T.SCOPE, 3, 100, 7, 10, 1.7, 2.5),
    R(T.WAIT, 3, 21, WK.PROXY, 11, 4, 6),
    R(T.PROXY_DEQ, 3, 11, 0, 0, 0, 4.2),
    R(T.PROXY_CALL, 3, 11, 1, 0, 4.3, 5.3),
    R(T.SCOPE, 3, 101, 8, 11, 4.4, 5.2),
    R(T.SETTLE, 3, 101, SK.SCOPE, 0, 20, 0),
    R(T.SETTLE, 3, 100, SK.SCOPE, 0, 30, 0),
    R(T.WAIT, 3, 21, WK.SCOPE, 100, 8, 31),
  ];
  const r = frameOf(recs, 3);
  near(sum(r.path), 40);
  assertNoDuplicates(r.path);
  const settle = r.path.find((e) => e.cat === 'scope_settle');
  assert.equal(settle.id, 'settle:scope:100');
  near(settle.ms, 27.5);
  assert.ok(!ids(r.path).includes('settle:scope:101'));
  const call = r.path.find((e) => e.id === 'call:10');
  near(call.ms, 0.9);
  assert.deepEqual(r.path.map((e) => e.cat), ['run:w0', 'proxy_delivery', 'proxy_dispatch', 'main_call:kind1', 'scope_settle', 'wake:scope', 'run:w0']);
  assert.equal(r.unresolvedMs, 0);
  const c = r.critical.find((e) => e.id === 'settle:scope:100');
  near(c.slack, 0);
  near(c.improvementIfZero, 22);
});

test('overlapping waits on two workers: only the binding chain is counted, GPU durations stay off the CPU timeline', () => {
  const recs = [
    R(T.FRAME, 4, 0, 0, 0, 0, 31),
    R(T.JOB_RUN, 4, 31, 0, 0, 0, 31),
    R(T.WAIT, 4, 31, WK.PROXY, 20, 2, 12),
    R(T.PROXY_DEQ, 4, 20, 0, 0, 0, 2.2),
    R(T.PROXY_CALL, 4, 20, 2, 0, 2.5, 3.5),
    R(T.SCOPE, 4, 200, 9, 20, 2.6, 3.4),
    R(T.SETTLE, 4, 200, SK.SCOPE, 0, 11, 0),
    R(T.WAIT, 4, 31, WK.JOB, 32, 14, 30.5),
    R(T.JOB_RUN, 4, 32, 1, 0, 0, 30),
    R(T.WAIT, 4, 32, WK.PROXY, 21, 3.8, 4.6),
    R(T.PROXY_DEQ, 4, 21, 0, 0, 0, 3.85),
    R(T.PROXY_CALL, 4, 21, 3, 0, 3.9, 4.6),
    R(T.SUBMIT, 4, 300, 21, 1, 4.0, 4.5),
    R(T.SUBMIT_ENC, 4, 300, 77, 0, 0, 0),
    R(T.WAIT, 4, 32, WK.SUBMIT_DONE, 300, 5, 25),
    R(T.SETTLE, 4, 300, SK.SUBMIT, 0, 24, 0),
    R(T.GPU_PASS, 4, 300, 0, 0, 3.0, 0),
  ];
  const r = frameOf(recs, 4);
  near(sum(r.path), 31);
  assertNoDuplicates(r.path);
  assert.deepEqual(r.path.map((e) => e.cat), [
    'run:w1', 'proxy_delivery', 'proxy_dispatch', 'main_call:kind3', 'main_submit', 'gpu_completion',
    'wake:submit_done', 'run:w1', 'wake:job', 'run:w0',
  ]);
  const gpu = r.path.find((e) => e.cat === 'gpu_completion');
  near(gpu.ms, 19.5);
  near(gpu.meta.gpuMs, 3.0);
  near(r.gpuExecMs, 3.0);
  assert.ok(!ids(r.path).some((id) => id.startsWith('wait:31:0')));
  assert.ok(!ids(r.path).includes('settle:scope:200'));
  const sub = r.graph.get('submit:300');
  assert.deepEqual(sub.meta.encoders, [77]);
  assert.equal(r.unresolvedMs, 0);
});

test('unresolved attribution is reported explicitly and still sums to the frame', () => {
  const recs = [
    R(T.FRAME, 5, 0, 0, 0, 0, 10),
    R(T.JOB_RUN, 5, 41, 0, 0, 0, 10),
    R(T.WAIT, 5, 41, WK.SCOPE, 999, 2, 8),
    R(T.FRAME, 6, 0, 0, 0, 20, 30),
    R(T.JOB_RUN, 6, 42, 0, 0, 20, 30),
    R(T.WAIT, 6, 42, WK.JOB, 43, 22, 29.5),
    R(T.JOB_RUN, 6, 43, 1, 0, 24, 29),
  ];
  const out = analyze(recs);
  const f5 = out.frames[0], f6 = out.frames[1];
  near(sum(f5.path), 10);
  near(f5.unresolvedMs, 6);
  assert.equal(f5.unresolved[0].what, 'scope_without_settle');
  assert.equal(f5.unresolved[0].target, 999);
  assert.ok(ids(f5.path).includes('wait:41:0'));
  near(sum(f6.path), 10);
  near(f6.unresolvedMs, 4);
  assert.equal(f6.path[0].cat, 'unresolved:root_gap');
  near(f6.path[0].ms, 4);
  assert.deepEqual(f6.path.map((e) => e.cat), ['unresolved:root_gap', 'run:w1', 'wake:job', 'run:w0']);
  near(out.summary.unresolvedMsMean, 5);
  near(out.summary.pathCoverage, 1);
});

test('a prerequisite that ends after its dependent starts is flagged, not corrected, and the sum stays exact', () => {
  const recs = [
    R(T.FRAME, 7, 0, 0, 0, 0, 10),
    R(T.JOB_RUN, 7, 51, 0, 0, 0, 6),
    R(T.JOB_RUN, 7, 52, 1, 0, 5, 10),
    R(T.JOB_DEP, 7, 52, 51, 0, 0, 0),
  ];
  const r = frameOf(recs, 7);
  near(sum(r.path), 10);
  const inc = r.inconsistencies.find((i) => i.kind === 'pred_after_start');
  assert.ok(inc);
  near(inc.ms, 1);
  assert.deepEqual(ids(r.path), ['run:51:0', 'run:52:0']);
  near(r.path[0].ms, 5);
});

test('slack and predicted improvement differ: a zero-slack wait may still yield less than its length', () => {
  const recs = [
    R(T.FRAME, 8, 0, 0, 0, 0, 20),
    R(T.JOB_RUN, 8, 61, 0, 0, 0, 20),
    R(T.JOB_SPAWN, 8, 62, 61, 0, 1, 0),
    R(T.JOB_SPAWN, 8, 63, 61, 0, 1, 0),
    R(T.JOB_RUN, 8, 62, 1, 0, 2, 11),
    R(T.JOB_RUN, 8, 63, 2, 0, 2, 11.5),
    R(T.WAIT, 8, 61, WK.JOB, 62, 2, 12),
    R(T.WAIT, 8, 61, WK.JOB, 63, 12, 12.2),
  ];
  const r = frameOf(recs, 8);
  near(sum(r.path), 20);
  const wake = r.critical.find((e) => e.id === 'wait:61:0:wake');
  near(wake.ms, 1);
  near(wake.slack, 0);
  near(wake.improvementIfZero, 0.5);
  const sched = r.critical.find((e) => e.cat === 'sched');
  near(sched.ms, 1);
  near(sched.slack, 0);
  near(sched.improvementIfZero, 0.5);
});

test('scope settled before the wait began counts as wake overhead on the worker, not as settle latency', () => {
  const recs = [
    R(T.FRAME, 9, 0, 0, 0, 0, 10),
    R(T.JOB_RUN, 9, 71, 0, 0, 0, 10),
    R(T.WAIT, 9, 71, WK.PROXY, 30, 1, 2),
    R(T.PROXY_DEQ, 9, 30, 0, 0, 0, 1.1),
    R(T.PROXY_CALL, 9, 30, 1, 0, 1.2, 1.8),
    R(T.SCOPE, 9, 300, 5, 30, 1.3, 1.7),
    R(T.SETTLE, 9, 300, SK.SCOPE, 0, 3, 0),
    R(T.WAIT, 9, 71, WK.SCOPE, 300, 5, 5.3),
  ];
  const r = frameOf(recs, 9);
  near(sum(r.path), 10);
  assert.deepEqual(r.path.map((e) => e.cat), ['run:w0', 'proxy_delivery', 'proxy_dispatch', 'main_call:kind1', 'wake:proxy', 'run:w0', 'wake:scope', 'run:w0']);
  near(r.path.find((e) => e.cat === 'wake:scope').ms, 0.3);
  assert.ok(!ids(r.path).includes('settle:scope:300'));
});

test('present wait decomposes into raf wait, getCurrentTexture, copy, and copy-scope settle', () => {
  const recs = [
    R(T.FRAME, 10, 0, 0, 0, 0, 40),
    R(T.JOB_RUN, 10, 81, 0, 0, 0, 40),
    R(T.WAIT, 10, 81, WK.PROXY, 40, 10, 10.5),
    R(T.PROXY_DEQ, 10, 40, 0, 0, 0, 10.1),
    R(T.PROXY_CALL, 10, 40, 4, 0, 10.2, 10.4),
    R(T.WAIT, 10, 81, WK.PRESENT, 500, 10.5, 36),
    R(T.PRESENT, 10, 500, 40, 600, 10.3, 25),
    R(T.GCT, 10, 500, 0, 0, 25, 26),
    R(T.PRESENT_DONE, 10, 500, 700, 0, 28, 0),
    R(T.SCOPE, 10, 600, 0, 0, 26.5, 27.5),
    R(T.SETTLE, 10, 600, SK.SCOPE, 0, 35, 0),
  ];
  const r = frameOf(recs, 10);
  near(sum(r.path), 40);
  assertNoDuplicates(r.path);
  assert.deepEqual(r.path.map((e) => e.cat), [
    'run:w0', 'proxy_delivery', 'proxy_dispatch', 'main_call:kind4', 'raf_wait', 'get_current_texture',
    'present_copy', 'scope_settle', 'wake:present', 'run:w0',
  ]);
  near(r.path.find((e) => e.cat === 'raf_wait').ms, 14.7);
  near(r.path.find((e) => e.cat === 'scope_settle').ms, 7.5);
  near(r.path.find((e) => e.cat === 'present_copy').ms, 1.5);
  assert.equal(r.unresolvedMs, 0);
});

test('cli prints a summary for an exported trace file', () => {
  const dir = mkdtempSync(join(tmpdir(), 'wgtrace-'));
  const file = join(dir, 'trace.json');
  const trace = {
    meta: { callKinds: { 1: 'createTexture' } },
    records: [
      R(T.FRAME, 1, 0, 0, 0, 0, 12),
      R(T.JOB_RUN, 1, 1, 0, 0, 0, 12),
      R(T.WAIT, 1, 1, WK.PROXY, 5, 1, 9),
      R(T.PROXY_DEQ, 1, 5, 0, 0, 0, 1.5),
      R(T.PROXY_CALL, 1, 5, 1, 0, 2, 3),
      R(T.SCOPE, 1, 50, 0, 5, 2.1, 2.9),
      R(T.SETTLE, 1, 50, SK.SCOPE, 0, 8.5, 0),
    ],
  };
  writeFileSync(file, JSON.stringify(trace));
  const here = dirname(fileURLToPath(import.meta.url));
  const out = execFileSync(process.execPath, [join(here, 'critical_path.js'), file, '--frames'], { encoding: 'utf8' });
  assert.match(out, /frames 1/);
  assert.match(out, /main_call:createTexture/);
  assert.match(out, /scope_settle/);
  assert.match(out, /critical path covers 100\.0%/);
});
