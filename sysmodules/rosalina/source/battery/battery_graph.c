/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: curve graph on the bottom screen (320x240, RGB565).
*
*   Rosalina's Draw_* API only draws text, so this file has a few pixel / line helpers.
*   The bottom framebuffer is stored rotated: pixel (x, y) is at fb[x * 240 + (239 - y)]
*   (same layout Draw_DrawCharacter uses).
*/

#include <3ds.h>
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "battery/battery_curve.h"
#include "battery/battery_graph.h"

// Plot area (inclusive pixel coordinates)
#define PLOT_X0     34
#define PLOT_X1     313
#define PLOT_Y0     28  // top
#define PLOT_Y1     196 // bottom

#define COLOR_AXIS  COLOR_WHITE
#define COLOR_GRID  RGB565(0x05, 0x0A, 0x05)
#define COLOR_STOCK RGB565(0x12, 0x24, 0x12)
#define COLOR_BAND  RGB565(0x02, 0x16, 0x05)
#define COLOR_BANDT RGB565(0x06, 0x2A, 0x0A) // band legend text (the band color is too dark for text)
#define COLOR_MEAN  RGB565(0x08, 0x3F, 0x08)

static inline void batteryGraphPixel(s32 x, s32 y, u16 color)
{
    if (x < 0 || x >= SCREEN_BOT_WIDTH || y < 0 || y >= SCREEN_BOT_HEIGHT)
        return;
    u16 *const fb = (u16 *)FB_BOTTOM_VRAM_ADDR;
    fb[x * SCREEN_BOT_HEIGHT + (SCREEN_BOT_HEIGHT - 1 - y)] = color;
}

static void batteryGraphVLine(s32 x, s32 y0, s32 y1, u16 color)
{
    if (y0 > y1)
    {
        s32 t = y0;
        y0 = y1;
        y1 = t;
    }
    for (s32 y = y0; y <= y1; y++)
        batteryGraphPixel(x, y, color);
}

static void batteryGraphHLine(s32 x0, s32 x1, s32 y, u16 color)
{
    for (s32 x = x0; x <= x1; x++)
        batteryGraphPixel(x, y, color);
}

// Bresenham
static void batteryGraphLine(s32 x0, s32 y0, s32 x1, s32 y1, u16 color)
{
    s32 dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    s32 dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    s32 err = dx + dy;
    for (;;)
    {
        batteryGraphPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        s32 e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

typedef struct BatteryGraphRange
{
    float xMax, yMax;     // MCU %, real %
    u32 xStep, yStep;     // tick spacing
} BatteryGraphRange;

static float batteryGraphXToMcu(const BatteryGraphRange *r, s32 x)
{
    return r->xMax * (float)(x - PLOT_X0) / (float)(PLOT_X1 - PLOT_X0);
}

static s32 batteryGraphMcuToX(const BatteryGraphRange *r, float mcu)
{
    return PLOT_X0 + (s32)(mcu / r->xMax * (PLOT_X1 - PLOT_X0) + 0.5f);
}

static s32 batteryGraphRealToY(const BatteryGraphRange *r, float real)
{
    if (real > r->yMax)
        real = r->yMax;
    if (real < 0.0f)
        real = 0.0f;
    return PLOT_Y1 - (s32)(real / r->yMax * (PLOT_Y1 - PLOT_Y0) + 0.5f);
}

static void batteryGraphAxes(const BatteryGraphRange *r)
{
    char label[8];

    // Grid + tick labels
    for (u32 v = 0; v <= (u32)r->xMax; v += r->xStep)
    {
        s32 x = batteryGraphMcuToX(r, (float)v);
        for (s32 y = PLOT_Y0; y < PLOT_Y1; y += 3)
            batteryGraphPixel(x, y, COLOR_GRID);
        batteryGraphVLine(x, PLOT_Y1, PLOT_Y1 + 2, COLOR_AXIS);
        u32 len = (u32)sprintf(label, "%lu", v);
        s32 lx = x - (s32)(len * SPACING_X) / 2;
        if (lx + (s32)(len * SPACING_X) > SCREEN_BOT_WIDTH)
            lx = SCREEN_BOT_WIDTH - (s32)(len * SPACING_X);
        Draw_DrawString((u32)lx, PLOT_Y1 + 4, COLOR_WHITE, label);
    }
    for (u32 v = 0; v <= (u32)r->yMax; v += r->yStep)
    {
        s32 y = batteryGraphRealToY(r, (float)v);
        for (s32 x = PLOT_X0; x < PLOT_X1; x += 3)
            batteryGraphPixel(x, y, COLOR_GRID);
        batteryGraphHLine(PLOT_X0 - 2, PLOT_X0, y, COLOR_AXIS);
        u32 len = (u32)sprintf(label, "%lu", v);
        s32 ly = y - 5;
        if (ly < PLOT_Y0 - 5)
            ly = PLOT_Y0 - 5;
        Draw_DrawString(PLOT_X0 - 4 - len * SPACING_X, (u32)ly, COLOR_WHITE, label);
    }

    batteryGraphVLine(PLOT_X0, PLOT_Y0, PLOT_Y1, COLOR_AXIS);
    batteryGraphHLine(PLOT_X0, PLOT_X1, PLOT_Y1, COLOR_AXIS);
}

void BatteryGraph_Draw(const BatteryCurveFile *curve, bool zoom)
{
    BatteryGraphRange r;
    if (zoom)
    {
        r.xMax = 10.0f;
        r.yMax = 50.0f;
        r.xStep = 2;
        r.yStep = 10;
    }
    else
    {
        r.xMax = 100.0f;
        r.yMax = 100.0f;
        r.xStep = 25;
        r.yStep = 25;
    }

    if (curve != NULL)
    {
        // min/max band (one vertical line per column), drawn first so the rest stays on top
        for (s32 x = PLOT_X0 + 1; x <= PLOT_X1; x++)
        {
            float mcuX10 = batteryGraphXToMcu(&r, x) * 10.0f;
            float mn = BatteryCurve_Interp(curve->mcuGridX10, curve->mcuMin, BATTERY_CURVE_MCU_POINTS, mcuX10);
            float mx = BatteryCurve_Interp(curve->mcuGridX10, curve->mcuMax, BATTERY_CURVE_MCU_POINTS, mcuX10);
            batteryGraphVLine(x, batteryGraphRealToY(&r, mn), batteryGraphRealToY(&r, mx), COLOR_BAND);
        }
    }

    batteryGraphAxes(&r);

    // Stock curve: real = MCU
    float stockEnd = r.xMax < r.yMax ? r.xMax : r.yMax;
    batteryGraphLine(batteryGraphMcuToX(&r, 0.0f), batteryGraphRealToY(&r, 0.0f),
                     batteryGraphMcuToX(&r, stockEnd), batteryGraphRealToY(&r, stockEnd), COLOR_STOCK);

    // Legend: bottom right (full view) or top left (zoom); both regions stay empty for real curves
    u32 lx = zoom ? PLOT_X0 + 8 : PLOT_X1 - 15 * SPACING_X;
    u32 ly = zoom ? PLOT_Y0 + 2 : PLOT_Y1 - 4 - 3 * SPACING_Y;

    if (curve == NULL)
    {
        Draw_DrawString(lx, ly, COLOR_STOCK, "stock (MCU)");
        Draw_DrawString(PLOT_X0 + 20, (PLOT_Y0 + PLOT_Y1) / 2 - 20, COLOR_WHITE, "No curve yet.");
        Draw_DrawString(PLOT_X0 + 20, (PLOT_Y0 + PLOT_Y1) / 2 - 20 + SPACING_Y, COLOR_WHITE, "Record sessions, then use");
        Draw_DrawString(PLOT_X0 + 20, (PLOT_Y0 + PLOT_Y1) / 2 - 20 + 2 * SPACING_Y, COLOR_WHITE, "\"Build curve\".");
        return;
    }

    // Learned mean curve: connect column samples with lines (the bottom part is steep)
    s32 prevX = 0, prevY = 0;
    for (s32 x = PLOT_X0; x <= PLOT_X1; x++)
    {
        float m = BatteryCurve_Interp(curve->mcuGridX10, curve->mcuMean, BATTERY_CURVE_MCU_POINTS,
                                      batteryGraphXToMcu(&r, x) * 10.0f);
        s32 y = batteryGraphRealToY(&r, m);
        if (x != PLOT_X0)
            batteryGraphLine(prevX, prevY, x, y, COLOR_MEAN);
        prevX = x;
        prevY = y;
    }

    Draw_DrawString(lx, ly, COLOR_MEAN, "learned (mean)");
    Draw_DrawString(lx, ly + SPACING_Y, COLOR_BANDT, "min..max band");
    Draw_DrawString(lx, ly + 2 * SPACING_Y, COLOR_STOCK, "stock (MCU)");
}
