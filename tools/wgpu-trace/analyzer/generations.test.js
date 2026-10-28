// Counter-generation races: every shape must be reported as ambiguous, create no dependency edge,
// and leave the critical path summing exactly to the frame.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { analyze, T } from './critical_path.js';

const Rt = (tid, type, frameId, a, b, c, t0, t1 = 0) => ({ type, frameId, a, b, c, tid, t0, t1 });
const near = (x, y, eps = 1e-6) => assert.ok(Math.abs(x - y) < eps, `expected ${x} ≈ ${y}`);
const sum = (p) => p.reduce((s, e) => s + e.ms, 0);
const cats = (p) => p.map((e) => e.cat);
const reasons = (out, inst) => out.counterAudit.ambiguities.filter((a) => a.inst === inst).map((a) => a.reason).sort();
const hasEdge = (g, to, from) => g.get(to)?.edges.some((e) => e.from === from);
const ADDR = 0x5000;

test('competing implicit arms at one address are a generation overlap, not a dependency', () => {
  const recs = [
    Rt(1, T.FRAME, 1, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 1, 10, 0, 0, 0, 20),
    Rt(2, T.CTR_ARM, 1, 1, ADDR, 0, 1.0, 1),
    Rt(4, T.CTR_DEC, 1, 1, 0, 1, 1.05, 1.06),   // what a pre-CAS losing creator emitted under its own id
    Rt(3, T.CTR_ARM, 1, 2, ADDR, 0, 1.1, 1),
    Rt(2, T.CTR_CONTRIB, 1, 2, 11, 1, 1.2, 1.3),
    Rt(2, T.JOB_RUN, 1, 11, 1, 0, 2, 9), Rt(2, T.CTR_DEC, 1, 2, 11, 0, 9, 9.1),
    Rt(1, T.CTR_WAIT, 1, 2, 10, 1, 3, 12), Rt(1, T.CTR_WAKE, 1, 2, 10, 1, 11.8, 12),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 20);
  assert.deepEqual(reasons(out, 2), ['generation_overlap', 'generation_overlap']);
  assert.equal(out.counterAudit.byInst.get(2).zeroStatus, 'generation_overlap');
  assert.equal(r.counterWaits[0].unresolved, 'counter_zero_generation_overlap');
  assert.ok(!r.path.some((e) => e.id === 'run:11:0'), 'no edge from the decrement of an overlapping generation');
});

test('a wait-only implicit instance followed by a real generation is not an overlap', () => {
  const recs = [
    Rt(1, T.FRAME, 8, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 8, 80, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 8, 20, ADDR, 0, 0.2, 1),
    Rt(1, T.CTR_WAIT, 8, 20, 80, 1, 0.2, 0.3),
    Rt(1, T.CTR_ARM, 8, 21, ADDR, 0, 1, 0), Rt(1, T.CTR_CONTRIB, 8, 21, 81, 1, 0.9, 1.1), Rt(1, T.JOB_SPAWN, 8, 81, 80, 1, 0.9),
    Rt(2, T.JOB_RUN, 8, 81, 1, 0, 2, 9), Rt(2, T.CTR_DEC, 8, 21, 81, 0, 9, 9.1),
    Rt(1, T.CTR_WAIT, 8, 21, 80, 1, 3, 10), Rt(1, T.CTR_WAKE, 8, 21, 80, 1, 9.5, 10),
  ];
  const out = analyze(recs);
  assert.equal(out.counterAudit.byInst.get(21).zeroStatus, 'verified');
  assert.equal(out.counterAudit.ambiguities.length, 0);
  const r = out.frames[0];
  near(sum(r.path), 20);
  assert.ok(r.path.some((e) => e.id === 'run:81:0'));
});

test('an add after the zero and an add overlapping it are ambiguous with candidates; the zero still verifies', () => {
  const recs = [
    Rt(1, T.FRAME, 2, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 2, 20, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 2, 7, ADDR, 0, 0.9, 0),
    Rt(1, T.CTR_CONTRIB, 2, 7, 21, 1, 1, 1.1), Rt(1, T.JOB_SPAWN, 2, 21, 20, 1, 1),
    Rt(2, T.JOB_RUN, 2, 21, 1, 0, 1.2, 10), Rt(2, T.CTR_DEC, 2, 7, 21, 0, 10, 10.2),
    Rt(3, T.CTR_CONTRIB, 2, 7, 23, 1, 10.1, 10.3),   // overlaps the zero window
    Rt(4, T.CTR_ARM, 2, 8, ADDR, 0, 11.05, 0),
    Rt(3, T.CTR_CONTRIB, 2, 7, 22, 1, 11, 11.1),     // began after the zero ended: stale tag
    Rt(1, T.CTR_WAIT, 2, 7, 20, 1, 2, 10.6), Rt(1, T.CTR_WAKE, 2, 7, 20, 1, 10.4, 10.6),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 20);
  assert.deepEqual(reasons(out, 7), ['add_after_zero', 'add_overlaps_zero']);
  const after = out.counterAudit.ambiguities.find((a) => a.reason === 'add_after_zero');
  assert.equal(after.jobId, 22);
  assert.deepEqual(after.candidates, [7, 8]);
  assert.equal(out.counterAudit.byInst.get(7).zeroStatus, 'verified');
  assert.deepEqual(cats(r.path).slice(-5), ['run:w1', 'unresolved:counter_zero_window', 'wake:counter', 'counter_resume', 'run:w0']);
  const cw = r.counterWaits[0];
  assert.equal(cw.contributions.find((c) => c.jobId === 22).ambiguous, 'add_after_zero');
  assert.equal(cw.contributions.find((c) => c.jobId === 21).ambiguous, null);
});

test('a decrement after the zero, including a second zero tagged to the same instance, gets no edge', () => {
  const recs = [
    Rt(1, T.FRAME, 3, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 3, 30, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 3, 9, ADDR, 0, 0.5, 0),
    Rt(1, T.CTR_CONTRIB, 3, 9, 31, 1, 0.6, 0.7), Rt(1, T.CTR_CONTRIB, 3, 9, 32, 1, 0.8, 0.9),
    Rt(2, T.JOB_RUN, 3, 31, 1, 0, 1, 5), Rt(2, T.CTR_DEC, 3, 9, 31, 1, 5, 5.1),
    Rt(3, T.JOB_RUN, 3, 32, 2, 0, 1, 8), Rt(3, T.CTR_DEC, 3, 9, 32, 0, 8, 8.1),
    Rt(4, T.JOB_RUN, 3, 34, 3, 0, 11, 12), Rt(4, T.CTR_DEC, 3, 9, 34, 0, 12, 12.1),  // stale: second zero
    Rt(1, T.CTR_WAIT, 3, 9, 30, 1, 2, 13), Rt(1, T.CTR_WAKE, 3, 9, 30, 1, 12.8, 13),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 20);
  assert.deepEqual(reasons(out, 9), ['dec_after_zero']);
  assert.equal(out.counterAudit.byInst.get(9).zero.jobId, 32, 'the first zero is the zero');
  const wakeId = 'wait:30:0:wake';
  assert.ok(!hasEdge(r.graph, wakeId, 'run:34:0'));
  assert.ok(hasEdge(r.graph, wakeId, 'run:31:0'), 'certain non-zero decrement keeps its edge');
  assert.ok(hasEdge(r.graph, 'wait:30:0:zero_window', 'run:32:0'));
});

test('a wait tagged to a stale instance while a later generation was armed before resume is ambiguous', () => {
  const recs = [
    Rt(1, T.FRAME, 4, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 4, 40, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 4, 11, ADDR, 0, 0.5, 0), Rt(1, T.CTR_CONTRIB, 4, 11, 41, 1, 0.6, 0.7),
    Rt(2, T.JOB_RUN, 4, 41, 1, 0, 1, 4), Rt(2, T.CTR_DEC, 4, 11, 41, 0, 4, 4.1),
    Rt(3, T.CTR_ARM, 4, 12, ADDR, 0, 5, 0), Rt(3, T.CTR_CONTRIB, 4, 12, 42, 1, 4.9, 5.1),
    Rt(3, T.JOB_RUN, 4, 42, 2, 0, 6, 14), Rt(3, T.CTR_DEC, 4, 12, 42, 0, 14, 14.1),
    Rt(1, T.CTR_WAIT, 4, 11, 40, 1, 6, 15),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 20);
  const cw = r.counterWaits[0];
  assert.equal(cw.unresolved, 'counter_wait_generation_ambiguous');
  assert.deepEqual(cw.candidateInstances, [11, 12]);
  near(cw.blockedUnattributedMs, 9);
  assert.ok(!r.path.some((e) => e.id === 'run:42:0'), 'the later generation is a candidate, not an edge');
  assert.ok(!r.path.some((e) => e.id === 'run:41:0'));
});

test('a count mismatch makes the zero ambiguous', () => {
  const recs = [
    Rt(1, T.FRAME, 5, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 5, 50, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 5, 13, ADDR, 0, 0.5, 0),
    Rt(1, T.CTR_CONTRIB, 5, 13, 51, 1, 0.6, 0.7), Rt(1, T.CTR_CONTRIB, 5, 13, 52, 1, 0.8, 0.9),
    Rt(2, T.JOB_RUN, 5, 51, 1, 0, 1, 9), Rt(2, T.CTR_DEC, 5, 13, 51, 0, 9, 9.1),
    Rt(1, T.CTR_WAIT, 5, 13, 50, 1, 2, 10),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 20);
  assert.equal(out.counterAudit.byInst.get(13).zeroStatus, 'count_mismatch');
  assert.deepEqual(out.counterAudit.byInst.get(13).count, { contributedCertain: 2, contributedOverlapping: 0, decrementedCertain: 1, decrementedOverlapping: 0 });
  assert.equal(r.counterWaits[0].unresolved, 'counter_zero_count_mismatch');
  assert.ok(!r.path.some((e) => e.id === 'run:51:0'));
});

test('a zero instance created before the capture cannot be verified', () => {
  const recs = [
    Rt(1, T.FRAME, 6, 0, 0, 0, 0, 10),
    Rt(1, T.JOB_RUN, 6, 60, 0, 0, 0, 10),
    Rt(2, T.JOB_RUN, 6, 61, 1, 0, 1, 6), Rt(2, T.CTR_DEC, 6, 15, 61, 0, 6, 6.1),
    Rt(1, T.CTR_WAIT, 6, 15, 60, 1, 2, 7),
  ];
  const out = analyze(recs);
  assert.equal(out.counterAudit.byInst.get(15).zeroStatus, 'generation_started_before_capture');
  assert.equal(out.frames[0].counterWaits[0].unresolved, 'counter_zero_generation_started_before_capture');
  near(sum(out.frames[0].path), 10);
});

test('preemption inside the decrement window is an unattributed interval, bounded by the wake', () => {
  const mk = (wake) => [
    Rt(1, T.FRAME, 7, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 7, 70, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 7, 17, ADDR, 0, 0.5, 0), Rt(1, T.CTR_CONTRIB, 7, 17, 71, 1, 0.6, 0.7), Rt(1, T.JOB_SPAWN, 7, 71, 70, 0, 0.7),
    Rt(2, T.JOB_RUN, 7, 71, 1, 0, 1, 10), Rt(2, T.CTR_DEC, 7, 17, 71, 0, 10, 14),
    Rt(1, T.CTR_WAIT, 7, 17, 70, 1, 2, wake + 0.5), Rt(1, T.CTR_WAKE, 7, 17, 70, 1, wake, wake + 0.5),
  ];
  const late = analyze(mk(14.5)).frames[0];
  near(sum(late.path), 20);
  near(late.path.find((e) => e.cat === 'unresolved:counter_zero_window').ms, 4);
  near(late.counterWaits[0].zeroWindowMs, 4);
  near(late.path.find((e) => e.cat === 'wake:counter').ms, 0.5);
  // the waiter observed zero at 12, so the transition happened in [10, 12], not at the end stamp 14
  const early = analyze(mk(12)).frames[0];
  near(sum(early.path), 20);
  near(early.path.find((e) => e.cat === 'unresolved:counter_zero_window').ms, 2);
  assert.equal(early.inconsistencies.length, 0);
});

test('the trace written by the emsdk race test is classified as ambiguous where the race happened', (t) => {
  const file = join(dirname(fileURLToPath(import.meta.url)), '..', 'runtime', 'test', 'out', 'race_trace.json');
  if (!existsSync(file)) { t.skip('run runtime/test/build_and_run.ps1 first'); return; }
  const out = analyze(JSON.parse(readFileSync(file, 'utf8')));
  const amb = out.counterAudit.ambiguities;
  // the complete ambiguity set: exactly the forced race, nothing else anywhere in the trace
  assert.deepEqual(amb.map((a) => [a.jobId, a.reason]).sort(), [[103, 'add_after_zero'], [103, 'dec_after_zero']]);
  for (const v of out.counterAudit.byInst.values()) if (v.zero) assert.equal(v.zeroStatus, 'verified', `instance ${v.inst}`);
  // scenario 4: the waiter observed zero while the decrementer was parked before its end stamp
  const r4 = out.frames.find((f) => f.frameId === 4);
  assert.ok(r4, 'scenario 4 frame present');
  assert.equal(r4.inconsistencies.length, 0);
  const cw4 = r4.counterWaits[0];
  assert.equal(cw4.unresolved, null);
  assert.ok(cw4.wakeAt < cw4.zeroWindowEndAt, 'wake stamp precedes the decrement end stamp');
  const win = r4.graph.get('wait:400:0:zero_window');
  near(win.end - r4.graph.frame.start, cw4.wakeAt, 1e-9);
  for (const a of amb.filter((x) => x.jobId === 103)) assert.equal(a.candidates.length, 2, 'stale instance and the generation published after it');
  const inst101 = [...out.counterAudit.byInst.values()].find((v) => v.zero && v.zero.jobId === 101);
  assert.equal(inst101.zeroStatus, 'verified');
  const inst102 = [...out.counterAudit.byInst.values()].find((v) => v.zero && v.zero.jobId === 102);
  // the stale contribution and decrement of job 103 are missing from generation G, so its count still balances
  assert.equal(inst102.zeroStatus, 'verified');
  assert.equal(out.preserved.unknown && Object.keys(out.preserved.unknown).length, 0);
});

test('gate release links a waiting-list job to the job that zeroed the releasing counter; two candidates stay ambiguous', () => {
  const base = [
    Rt(1, T.FRAME, 9, 0, 0, 0, 0, 30),
    Rt(1, T.JOB_RUN, 9, 90, 0, 0, 0, 30),
    Rt(1, T.CTR_ARM, 9, 30, 0x700, 0, 0.5, 0), Rt(1, T.CTR_CONTRIB, 9, 30, 0, 1, 0.6, 0.7),
    Rt(1, T.CTR_ARM, 9, 31, 0x800, 0, 0.5, 0), Rt(1, T.CTR_CONTRIB, 9, 31, 0, 1, 0.6, 0.7),
    // job 91 drives counter 31 (the gate) to zero; that releases EmptyJob 92, which then zeroes counter 30
    Rt(2, T.JOB_RUN, 9, 91, 1, 0, 2, 20), Rt(2, T.CTR_DEC, 9, 31, 91, 0, 19.9, 19.95),
    Rt(2, T.GATE_RELEASE, 9, 31, 30, 91, 19.96, 1),
    Rt(3, T.JOB_RUN, 9, 92, 2, 0, 22, 22.1), Rt(3, T.CTR_DEC, 9, 30, 92, 0, 22.1, 22.15),
    Rt(1, T.CTR_WAIT, 9, 30, 90, 1, 1, 23), Rt(1, T.CTR_WAKE, 9, 30, 90, 1, 22.5, 23),
  ];
  const r = analyze(base).frames[0];
  near(sum(r.path), 30);
  assert.ok(r.path.some((e) => e.cat === 'gate_queue'), 'release-to-start time is labelled');
  near(r.path.find((e) => e.cat === 'gate_queue').ms, 22 - 19.96);
  assert.ok(r.path.some((e) => e.id === 'run:91:0'), 'the gating job is on the path');
  assert.ok(r.path.some((e) => e.id === 'run:92:0'), 'the released job follows the gate');
  const amb = analyze([...base, Rt(4, T.GATE_RELEASE, 9, 77, 30, 95, 10, 1)]).frames[0];
  near(sum(amb.path), 30);
  assert.ok(amb.unresolved.some((u) => u.what === 'gate_release_ambiguous' && u.candidates.length === 2));
  assert.ok(!amb.path.some((e) => e.cat === 'gate_queue'));
});
