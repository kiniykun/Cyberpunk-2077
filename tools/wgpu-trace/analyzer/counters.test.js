// Counter waits (records 21..25) and preserved integration records (16..20).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { analyze, schedule, T, WK } from './critical_path.js';

const Rt = (tid, type, frameId, a, b, c, t0, t1 = 0) => ({ type, frameId, a, b, c, tid, t0, t1 });
const near = (x, y, eps = 1e-6) => assert.ok(Math.abs(x - y) < eps, `expected ${x} ≈ ${y}`);
const sum = (p) => p.reduce((s, e) => s + e.ms, 0);
const ids = (p) => p.map((e) => e.id);
const cats = (p) => p.map((e) => e.cat);
test('unknown instance zero cannot import unrelated jobs from later frames', () => {
  const out=analyze([
    Rt(1,T.FRAME,1,0,0,0,0,20),Rt(1,T.JOB_RUN,1,100,0,0,0,20),
    Rt(1,T.CTR_WAIT,1,0,100,1,2,18),Rt(1,T.CTR_WAKE,1,0,100,1,17,18),
    Rt(2,T.JOB_RUN,2,200,1,0,100,200),Rt(2,T.CTR_DEC,2,0,200,0,199,200)
  ]);
  near(out.frames[0].pathMs,20);
  assert.ok(out.frames[0].unresolved.some(u=>u.what==='counter_wait_without_instance'));
});
const assertNoDuplicates = (p) => {
  const seen = new Set();
  for (const e of p) { assert.ok(!seen.has(e.id), `duplicate path entry ${e.id}`); seen.add(e.id); }
};

test('cooperative execution: jobs run inside a counter wait are not blocked time and bind the wake only when they overrun', () => {
  // Waiter W=10 (tid 1) runs H=12 cooperatively while contributor C=11 (tid 2) brings the counter to zero.
  const early = [
    Rt(1, T.FRAME, 1, 0, 0, 0, 0, 30),
    Rt(1, T.JOB_RUN, 1, 10, 0, 0, 0, 30),
    Rt(1, T.CTR_ARM, 1, 1, 0xabc, 0, 0.9, 0),
    Rt(1, T.CTR_CONTRIB, 1, 1, 11, 1, 1),
    Rt(1, T.JOB_SPAWN, 1, 11, 10, 1, 1),
    Rt(2, T.JOB_RUN, 1, 11, 1, 0, 2, 25),
    Rt(2, T.CTR_DEC, 1, 1, 11, 0, 25.2),
    Rt(1, T.JOB_RUN, 1, 12, 0, 0, 8, 18),
    Rt(1, T.CTR_WAIT, 1, 1, 10, 7, 5, 26),
    Rt(1, T.CTR_WAKE, 1, 1, 10, 7, 25.5, 26),
  ];
  const r = analyze({ meta: { siteNames: { 7: 'Frame/WaitPrev' } }, records: early }).frames[0];
  near(sum(r.path), 30);
  assertNoDuplicates(r.path);
  assert.deepEqual(cats(r.path), ['run:w0', 'unresolved:enqueue_to_start', 'run:w1', 'counter_signal', 'wake:counter', 'counter_resume', 'run:w0']);
  assert.ok(!ids(r.path).includes('run:12:0'), 'help job finished before zero, so it is not on the path');
  near(r.path.find((e) => e.cat === 'counter_signal').ms, 0.2);
  near(r.path.find((e) => e.cat === 'wake:counter').ms, 0.3);
  near(r.unresolvedMs, 1, 1e-6); // enqueue at 1, start at 2: not proven scheduling
  const cw = r.counterWaits.find((c) => c.waiterJobId === 10);
  near(cw.waitMs, 21);
  near(cw.cooperativeMs, 10);
  near(cw.notCooperativeMs, 11);
  near(cw.zeroAt, 25.2);
  assert.equal(cw.zeroBy.jobId, 11);
  assert.equal(cw.zeroBy.spawnedBy, 10);
  assert.equal(cw.siteName, 'Frame/WaitPrev');
  near(cw.wakeAfterReleaseMs, 0.3);
  near(cw.resumeAfterWakeMs, 0.5);
  assert.deepEqual(cw.helps.map((h) => h.jobId), [12]);
  assert.equal(cw.contributions[0].jobId, 11);
  near(cw.contributions[0].decAt, 25.2);

  // Same shape, but the help job is still running when the counter reaches zero: the help binds the wake.
  const late = [
    Rt(1, T.FRAME, 2, 0, 0, 0, 0, 30),
    Rt(1, T.JOB_RUN, 2, 20, 0, 0, 0, 30),
    Rt(1, T.CTR_ARM, 2, 2, 0xabd, 0, 0.9, 0),
    Rt(1, T.CTR_CONTRIB, 2, 2, 21, 1, 1),
    Rt(1, T.JOB_SPAWN, 2, 21, 20, 0, 1),
    Rt(2, T.JOB_RUN, 2, 21, 1, 0, 2, 12),
    Rt(2, T.CTR_DEC, 2, 2, 21, 0, 12),
    Rt(1, T.JOB_RUN, 2, 22, 0, 0, 8, 20),
    Rt(1, T.CTR_WAIT, 2, 2, 20, 7, 5, 20.4),
    Rt(1, T.CTR_WAKE, 2, 2, 20, 7, 20.2, 20.4),
  ];
  const r2 = analyze(late).frames[0];
  near(sum(r2.path), 30);
  assertNoDuplicates(r2.path);
  assert.ok(ids(r2.path).includes('run:22:0'), 'overrunning help job is on the path');
  assert.ok(!ids(r2.path).includes('run:21:0'));
  near(r2.path.find((e) => e.cat === 'wake:counter').ms, 0.2);
  const cw2 = r2.counterWaits[0];
  near(cw2.cooperativeMs, 12);
  near(cw2.lastHelpEndAt, 20);
  assert.equal(r2.unresolvedMs, 0);
  near(r2.critical.find((c) => c.cat === 'wake:counter').slack, 0);
});

test('overlapping workers: two waiters and two contributors on one counter, only the last decrement binds', () => {
  const recs = [
    Rt(1, T.FRAME, 3, 0, 0, 0, 0, 40),
    Rt(1, T.JOB_RUN, 3, 30, 0, 0, 0, 40),
    Rt(1, T.CTR_ARM, 3, 5, 0x10, 0, 1.4, 0),
    Rt(1, T.CTR_CONTRIB, 3, 5, 31, 1, 1.5), Rt(1, T.JOB_SPAWN, 3, 31, 30, 1, 1.5),
    Rt(1, T.CTR_CONTRIB, 3, 5, 32, 1, 1.6), Rt(1, T.JOB_SPAWN, 3, 32, 30, 1, 1.6),
    Rt(3, T.JOB_RUN, 3, 31, 2, 0, 2, 20), Rt(3, T.CTR_DEC, 3, 5, 31, 1, 20),
    Rt(4, T.JOB_RUN, 3, 32, 3, 0, 2, 28), Rt(4, T.CTR_DEC, 3, 5, 32, 0, 28),
    Rt(1, T.CTR_WAIT, 3, 5, 30, 1, 2, 30), Rt(1, T.CTR_WAKE, 3, 5, 30, 1, 29.6, 30),
    // a second waiter on another worker, overlapping the first wait
    Rt(2, T.JOB_RUN, 3, 33, 1, 0, 1, 35),
    Rt(2, T.CTR_WAIT, 3, 5, 33, 1, 3, 29.5), Rt(2, T.CTR_WAKE, 3, 5, 33, 1, 29.2, 29.5),
  ];
  const r = analyze(recs).frames[0];
  near(sum(r.path), 40);
  assertNoDuplicates(r.path);
  assert.deepEqual(cats(r.path), ['run:w0', 'unresolved:enqueue_to_start', 'run:w3', 'wake:counter', 'counter_resume', 'run:w0']);
  near(r.path.find((e) => e.cat === 'run:w3').ms, 26);
  near(r.path.find((e) => e.cat === 'wake:counter').ms, 1.6);
  assert.ok(!ids(r.path).some((id) => id.startsWith('wait:33:')), 'the other waiter is off the path');
  const s = schedule(r.graph);
  near(s.ls.get('run:31:0') - s.es.get('run:31:0'), 8);
  assert.equal(r.counterWaits.length, 2);
  for (const c of r.counterWaits) { assert.equal(c.zeroBy.jobId, 32); near(c.cooperativeMs, 0); }
  assert.deepEqual(r.counterWaits[0].contributions.map((c) => [c.jobId, c.valueAfter]), [[31, 1], [32, 0]]);
  near(r.unresolvedMs, 0.4);
  assert.equal(r.inconsistencies.length, 0);
});

test('counter completed before the wait began: the wait is wake overhead, the contributor is off the path', () => {
  const recs = [
    Rt(1, T.FRAME, 4, 0, 0, 0, 0, 10),
    Rt(1, T.JOB_RUN, 4, 40, 0, 0, 0, 10),
    Rt(1, T.CTR_ARM, 4, 9, 0xa9, 0, 0.4, 0),
    Rt(1, T.CTR_CONTRIB, 4, 9, 41, 1, 0.5), Rt(1, T.JOB_SPAWN, 4, 41, 40, 0, 0.5),
    Rt(2, T.JOB_RUN, 4, 41, 1, 0, 1, 4), Rt(2, T.CTR_DEC, 4, 9, 41, 0, 4),
    Rt(1, T.CTR_WAIT, 4, 9, 40, 2, 6, 6.3), Rt(1, T.CTR_WAKE, 4, 9, 40, 2, 6.1, 6.3),
  ];
  const r = analyze(recs).frames[0];
  near(sum(r.path), 10);
  assert.deepEqual(cats(r.path), ['run:w0', 'wake:counter', 'counter_resume', 'run:w0']);
  near(r.path[1].ms, 0.1);
  near(r.path[2].ms, 0.2);
  assert.ok(!ids(r.path).includes('run:41:0'));
  assert.equal(r.counterWaits[0].completedBeforeWait, true);
  near(r.counterWaits[0].zeroAt, 4);
  assert.equal(r.unresolvedMs, 0);
});

test('unresolved dependencies are explicit: missing zero, contributor without run, untyped legacy wait, missing spawn', () => {
  const recs = [
    // frame 5: zero never recorded; a help job runs inside the wait and is not counted as unresolved
    Rt(1, T.FRAME, 5, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 5, 50, 0, 0, 0, 20),
    Rt(1, T.CTR_CONTRIB, 5, 7, 51, 2, 0.5), Rt(1, T.JOB_SPAWN, 5, 51, 50, 0, 0.5),
    Rt(2, T.JOB_RUN, 5, 51, 1, 0, 1, 5), Rt(2, T.CTR_DEC, 5, 7, 51, 1, 5),
    Rt(1, T.JOB_RUN, 5, 52, 0, 0, 6, 10),
    Rt(1, T.CTR_WAIT, 5, 7, 50, 3, 2, 18),
    // frame 6: zero reached by a job with no JOB_RUN
    Rt(1, T.FRAME, 6, 0, 0, 0, 100, 110),
    Rt(1, T.JOB_RUN, 6, 60, 0, 0, 100, 110),
    Rt(1, T.CTR_ARM, 6, 8, 0xa8, 0, 100.5, 0), Rt(1, T.CTR_CONTRIB, 6, 8, 61, 1, 100.6),
    Rt(3, T.CTR_DEC, 6, 8, 61, 0, 107),
    Rt(1, T.CTR_WAIT, 6, 8, 60, 3, 102, 108),
    // frame 7: legacy kind-6 wait matched by COUNTER_ACTIVITY, with a cooperative job inside it
    Rt(1, T.FRAME, 7, 0, 0, 0, 200, 230),
    Rt(1, T.JOB_RUN, 7, 70, 0, 0, 200, 230),
    Rt(1, T.WAIT, 7, 70, WK.OTHER, 0xdead, 201, 225),
    Rt(1, T.COUNTER_ACTIVITY, 7, 70, 0xdead, 454, 201, 225),
    Rt(1, T.JOB_RUN, 7, 71, 0, 0, 205, 212),
    Rt(2, T.JOB_META, 0, 72, 0xdead, 9, 210, 211),
    Rt(2, T.JOB_RUN, 7, 72, 1, 0, 210, 211),
    // integration records that must be preserved
    Rt(1, T.MUTEX_WAIT, 7, 0, 3, 4, 226, 227),
    Rt(1, T.MUTEX_HOLD, 7, 0, 3, 4, 227, 228),
    Rt(1, T.RENDER_PHASE, 7, 70, 1, 2, 213, 214),
    Rt(1, 99, 7, 0, 0, 0, 213, 214),
    Rt(1, 99, 7, 0, 0, 0, 214, 215),
  ];
  const out = analyze({ meta: { jobNames: { 9: 'EmptyJob' } }, records: recs });
  const [f5, f6, f7] = out.frames;

  near(sum(f5.path), 20);
  assertNoDuplicates(f5.path);
  assert.ok(ids(f5.path).includes('run:52:0'), 'cooperative job sits between blocked segments');
  const u5 = f5.unresolved.find((u) => u.what === 'counter_zero_not_recorded');
  near(u5.waitMs, 16); near(u5.cooperativeMs, 4); near(u5.ms, 12);
  near(f5.unresolvedMs, 12);
  assert.equal(f5.counterWaits[0].unresolved, 'counter_zero_not_recorded');
  near(f5.counterWaits[0].blockedUnattributedMs, 12);

  near(sum(f6.path), 10);
  assert.ok(f6.unresolved.some((u) => u.what === 'counter_contributor_without_run' && u.target === 61));
  near(f6.path.find((e) => e.cat === 'unresolved:counter_contributor').ms, 5);

  near(sum(f7.path), 30);
  assertNoDuplicates(f7.path);
  const u7 = f7.unresolved.find((u) => u.what === 'counter_wait_untyped_release_unknown');
  near(u7.ms, 17); near(u7.cooperativeMs, 7); near(u7.counterAddr, 0xdead);
  near(f7.path.filter((e) => e.cat === 'unresolved:wait:counter').reduce((a, e) => a + e.ms, 0), 17);
  const cw7 = f7.counterWaits[0];
  assert.deepEqual(cw7.addressAssociatedJobs.map((j) => [j.jobId, j.name]), [[72, 'EmptyJob']]);
  assert.match(cw7.addressAssociationNote, /unverified/);
  assert.ok(!ids(f7.path).includes('run:72:0'), 'address association never creates an edge');
  near(f7.mutexOnPath.ms, 1);

  assert.equal(out.preserved.jobMeta, 1);
  assert.equal(out.preserved.counterActivity, 1);
  assert.equal(out.preserved.mutexWait, 1);
  assert.equal(out.preserved.mutexHold, 1);
  assert.equal(out.preserved.renderPhase, 1);
  assert.deepEqual(out.preserved.unknown, { 99: 2 });
  assert.equal(out.index.unknown.get(99).length, 2);
  assert.equal(out.notes.filter((n) => n.startsWith('unknown record type')).length, 1);

  // a contributor with no spawn record has no known start dependency; a wait without CTR_WAKE is flagged
  const lone = [
    Rt(1, T.FRAME, 8, 0, 0, 0, 0, 10),
    Rt(1, T.JOB_RUN, 8, 80, 0, 0, 0, 10),
    Rt(1, T.CTR_ARM, 8, 11, 0xab, 0, 0.5, 0), Rt(1, T.CTR_CONTRIB, 8, 11, 81, 1, 0.6),
    Rt(2, T.JOB_RUN, 8, 81, 1, 0, 3, 8), Rt(2, T.CTR_DEC, 8, 11, 81, 0, 8),
    Rt(1, T.CTR_WAIT, 8, 11, 80, 1, 1, 9),
  ];
  const f8 = analyze(lone).frames[0];
  near(sum(f8.path), 10);
  assert.equal(f8.path[0].cat, 'unresolved:root_gap');
  near(f8.path[0].ms, 3);
  assert.ok(f8.unresolved.some((u) => u.what === 'counter_wake_not_stamped'));
});

test('a decrement from a thread with no job context is named by thread, and record loss is reported', () => {
  const recs = [
    Rt(1, T.FRAME, 10, 0, 0, 0, 0, 20),
    Rt(1, T.JOB_RUN, 10, 100, 0, 0, 0, 20),
    Rt(1, T.CTR_ARM, 10, 13, 0xad, 0, 0.9, 0),
    Rt(1, T.CTR_CONTRIB, 10, 13, 0, 1, 1),
    Rt(77, T.CTR_DEC, 10, 13, 0, 0, 15),
    Rt(1, T.CTR_WAIT, 10, 13, 100, 1, 2, 16), Rt(1, T.CTR_WAKE, 10, 13, 100, 1, 15.5, 16),
  ];
  const out = analyze({ meta: { droppedWasm: 3, unregisteredEmits: 5 }, records: recs });
  const r = out.frames[0];
  near(sum(r.path), 20);
  const u = r.unresolved.find((x) => x.what === 'counter_decrement_outside_job');
  assert.ok(u);
  assert.equal(u.tid, 77);
  near(u.ms, 13);
  near(r.path.find((e) => e.cat === 'unresolved:counter_decrement_outside_job').ms, 13);
  assert.ok(out.notes.some((n) => n.includes('3 worker records dropped')));
  assert.ok(out.notes.some((n) => n.includes('5 records emitted from threads that never called thread_register')));
});

test('a typed counter wait supersedes the legacy kind-6 wait at the same site', () => {
  const recs = [
    Rt(1, T.FRAME, 9, 0, 0, 0, 0, 10),
    Rt(1, T.JOB_RUN, 9, 90, 0, 0, 0, 10),
    Rt(1, T.WAIT, 9, 90, WK.OTHER, 0x20, 2, 8),
    Rt(1, T.CTR_ARM, 9, 12, 0xac, 0, 0.9, 0),
    Rt(1, T.CTR_CONTRIB, 9, 12, 91, 1, 1), Rt(1, T.JOB_SPAWN, 9, 91, 90, 0, 1),
    Rt(2, T.JOB_RUN, 9, 91, 1, 0, 1, 7), Rt(2, T.CTR_DEC, 9, 12, 91, 0, 7),
    Rt(1, T.CTR_WAIT, 9, 12, 90, 1, 2, 8), Rt(1, T.CTR_WAKE, 9, 12, 90, 1, 7.5, 8),
  ];
  const out = analyze(recs);
  const r = out.frames[0];
  near(sum(r.path), 10);
  assert.equal(r.inconsistencies.length, 0);
  assert.ok(!cats(r.path).some((c) => c.startsWith('unresolved')));
  assert.ok(out.notes.some((n) => n.includes('superseded by CTR_WAIT')));
});
