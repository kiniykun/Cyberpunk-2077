// Second translation unit: proves the header links twice (no duplicate EM_JS or export symbols)
// and that hooks called from another TU share one counter table.
#include "race_hooks.h"
#include "../wgtrace.h"

uint32_t tu2_dec_begin_inst(const void* counter) { return wgtrace::ctr_dec_begin(counter).inst; }
uint32_t tu2_site() { return WGTRACE_SITE("RaceTest/SiteFromTU2"); }
