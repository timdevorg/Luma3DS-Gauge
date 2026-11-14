// Host stub of <3ds.h> for graph_host_render.c (battery_graph.c only needs types and RGB565).
#pragma once
#include <3ds/types.h>
#define RGB565(r,g,b)  (((b)&0x1f)|(((g)&0x3f)<<5)|(((r)&0x1f)<<11))
