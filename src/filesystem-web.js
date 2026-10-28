// Browser persistence for the original red::paths /userdata directory.
// Retain native caller frames when a worker reports a startup failure.
if ('stackTraceLimit' in Error) Error.stackTraceLimit = 50;
// This is engine startup plumbing; no replacement menus or game logic.
Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  if (typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD) return;
  // Native Linux paths use /engine and /r6; both point to real packaged depot data.
  FS.symlink('/game/engine', '/engine');
  FS.symlink('/game/r6', '/r6');
  // The native Linux fallback requests platform/other; use original PC templates.
  FS.symlink('/game/r6/config/settings/platform/pc', '/game/r6/config/settings/platform/other');
  const dependency = 'cp2077-userdata-restore';
  addRunDependency(dependency);
  Module['cp2077PersistentStorageReady'] = false;
  Module['cp2077PersistentStorageError'] = null;
  const reportError = function (error) {
    Module['cp2077PersistentStorageError'] = error;
    console.error('CP2077: persistent user-data synchronization failed', error);
  };
  try {
    // SDK autopersistence ignores its completion callback's error argument.
    // Observe every synchronization failure, including automatic background writes.
    const synchronize = IDBFS.syncfs;
    // Serialize automatic and explicit synchronization so a stale background
    // snapshot cannot commit after a newer acknowledged save/delete operation.
    const pending = [];
    let synchronizing = false;
    const drain = function () {
      if (synchronizing || !pending.length) return;
      synchronizing = true;
      const operation = pending.shift();
      let completed = false;
      const finish = function (error) {
        if (completed) return;
        completed = true;
        if (error) reportError(error);
        try { operation.callback(error); }
        finally { synchronizing = false; drain(); }
      };
      try { synchronize.call(IDBFS, operation.mount, operation.populate, finish); }
      catch (error) {
        if (completed) throw error;
        finish(error);
      }
    };
    IDBFS.syncfs = function (mount, populate, callback) {
      pending.push({mount: mount, populate: populate, callback: callback});
      drain();
    };
    FS.mkdirTree('/userdata');
    FS.mount(IDBFS, {autoPersist: true}, '/userdata');
    Module['cp2077FlushPersistentStorage'] = function () {
      return new Promise(function (resolve, reject) {
        FS.syncfs(false, function (error) {
          if (error) reject(error);
          else resolve();
        });
      });
    };
    FS.syncfs(true, function (error) {
      if (error) {
        // Do not launch against an empty replacement for unreadable existing saves.
        abort('CP2077: could not restore browser user data: ' + error);
        return;
      }
      Module['cp2077PersistentStorageReady'] = true;
      // [saves] Read-only census of restored save slots (dirs with sav.dat) for test drivers such as
      // cp2077-jump.js (CONTINUE is the first main-menu item when any exist). Never opens IndexedDB itself.
      try {
        const saveRoot = '/userdata/CD Projekt Red/Cyberpunk 2077/saves';
        const slots = [];
        if (FS.analyzePath(saveRoot).exists) {
          for (const name of FS.readdir(saveRoot)) {
            if (name === '.' || name === '..') continue;
            if (FS.analyzePath(saveRoot + '/' + name + '/sav.dat').exists) slots.push(name);
          }
        }
        Module['cp2077RestoredSaveSlots'] = slots;
        console.log('CP2077 saves: restored save slots=' + slots.length + (slots.length ? ' (' + slots.join(', ') + ')' : ''));
      } catch (e) { Module['cp2077RestoredSaveSlots'] = null; }
      removeRunDependency(dependency);
    });
  } catch (error) {
    reportError(error);
    abort('CP2077: browser user-data mount failed: ' + error);
  }
});
