// Authored browser storage bridge for original engine service workers.
#pragma once
namespace cp2077::web {
// Must run on a live pthread, never the browser main runtime thread.
// Returns only after the IndexedDB synchronization callback completes.
bool FlushPersistentStorage();
}
