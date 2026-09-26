#ifndef PROTO_COMPAT_H
#define PROTO_COMPAT_H
/* The prototype tests were written against the prototype's own runtime
 * (modtab.c, gate.s). They now run on the shipped one (mos-platform/c128/
 * cache.h, cache-gate.s, cache-host.*); this header maps the old names onto it. */
#include <cache.h>

void mod_init(void);         /* static 5-unit bank-0 pool, 32-unit bank-1 pool, host = module 6 */
void mod_init_shared(void);  /* the same but bank 0's pool is left for mos_cache_shared */
void mod_clear_refs(void);

#define obj_bank mos_cache_handle_bank
#define obj_lock mos_cache_handle_locks
#define pool_free_units mos_cache_free_units
#define pool_max_run mos_cache_max_run
#define mod_evict mos_cache_module_evict
#define pool0_size() mos_cache_pool_units(0)
#define pool0_base() mos_cache_pool_base(0)

#define mod_loads (mos_cache_stats.mod_loads)
#define mod_evictions (mos_cache_stats.mod_evictions)
#define defrag_moves (mos_cache_stats.defrag_moves)
#define place_refused (mos_cache_stats.place_refused)
#define obj_spills (mos_cache_stats.obj_spills)
#define sh_grows (mos_cache_stats.grown_units)
#define sh_moves (mos_cache_stats.pool_moves)
#define sh_hook_calls (mos_cache_stats.hook_calls)
#define sh_yielded (mos_cache_stats.yielded_units)
#define sh_services (mos_cache_stats.services)

extern volatile uint8_t __mos_gate_ams_top;
#define ams_top __mos_gate_ams_top
#endif
