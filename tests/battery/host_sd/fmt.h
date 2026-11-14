// Host stub of Rosalina's fmt.h for sd_host_test.c. Rosalina's long is 32 bits (ARM), so the code
// formats u32 with %lu; host_sprintf drops a single 'l' before d/u/x so u32 arguments work on 64-bit hosts.
#pragma once
#include <stdio.h>
int host_sprintf(char *buf, const char *fmt, ...);
#undef sprintf
#define sprintf host_sprintf
