// Host stub of Rosalina's draw.h for graph_host_render.c: a plain array as the bottom framebuffer.
#pragma once
#include <3ds.h>
extern u16 g_hostFb[320 * 240];
#define FB_BOTTOM_VRAM_ADDR ((void *)g_hostFb)
#define SCREEN_BOT_WIDTH  320
#define SCREEN_BOT_HEIGHT 240
#define SPACING_Y 11
#define SPACING_X 6
#define COLOR_TITLE RGB565(0x00, 0x26, 0x1F)
#define COLOR_WHITE RGB565(0x1F, 0x3F, 0x1F)
#define COLOR_RED   RGB565(0x1F, 0x00, 0x00)
#define COLOR_GREEN RGB565(0x00, 0x1F, 0x00)
#define COLOR_BLACK RGB565(0x00, 0x00, 0x00)
u32 Draw_DrawString(u32 posX, u32 posY, u32 color, const char *string);
