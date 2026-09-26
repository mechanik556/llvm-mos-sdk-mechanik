# Fails if the link map MAP mentions the cache runtime: a program that does not
# use it must not link it (nor pay for it in size or zero page).
file(READ ${MAP} map)
if(map MATCHES "cache\.c|cache-gate|cache-host|mos_cache|mos_cacheable")
  message(FATAL_ERROR "${MAP} mentions the cache runtime")
endif()
