/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Renders the Battery Manager graph (battery_graph.c, unchanged) on the host into PPM images,
*   to check the layout without a console.
*
*   Usage: graph_host_render curve.bin|- out_prefix   ("-" = no curve)
*   Writes out_prefix_full.ppm and out_prefix_zoom.ppm (320x240).
*   Build: see tests/battery/run_host_test.sh (it renders the graph too).
*/

#include <stdio.h>
#include <string.h>
#include "draw.h"
#include "font.h"
#include "battery/battery_graph.h"

u16 g_hostFb[320 * 240];

// Same pixel layout as Draw_DrawCharacter / Draw_DrawString (no wrapping needed here)
u32 Draw_DrawString(u32 posX, u32 posY, u32 color, const char *string)
{
    for (u32 i = 0; string[i] != 0; i++, posX += SPACING_X)
        for (u32 y = 0; y < 10; y++)
        {
            char row = font[(unsigned char)string[i] * 10 + y];
            for (s32 x = 6; x >= 1; x--)
            {
                u32 px = posX + (5 - x), py = posY + y;
                if (px < 320 && py < 240)
                    g_hostFb[px * 240 + (239 - py)] = ((row >> x) & 1) ? (u16)color : COLOR_BLACK;
            }
        }
    return posY;
}

static void writePpm(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n320 240\n255\n");
    for (u32 y = 0; y < 240; y++)
        for (u32 x = 0; x < 320; x++)
        {
            u16 c = g_hostFb[x * 240 + (239 - y)];
            unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 0x3F) << 2), (unsigned char)((c & 0x1F) << 3) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    static BatteryCurveFile curve;
    const BatteryCurveFile *c = NULL;
    char path[512];

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s curve.bin|- out_prefix\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "-") != 0)
    {
        FILE *f = fopen(argv[1], "rb");
        if (f == NULL || fread(&curve, sizeof(curve), 1, f) != 1 || !BatteryCurve_FileIsValid(&curve))
        {
            fprintf(stderr, "%s: not a valid curve\n", argv[1]);
            return 2;
        }
        fclose(f);
        c = &curve;
    }

    for (int zoom = 0; zoom < 2; zoom++)
    {
        char title[64];
        if (c != NULL)
            snprintf(title, sizeof(title), "Battery curve, %u session(s)%s", c->numSessions, zoom ? ", MCU 0-10%" : "");
        else
            snprintf(title, sizeof(title), "Battery curve");
        memset(g_hostFb, 0, sizeof(g_hostFb));
        Draw_DrawString(10, 10, COLOR_TITLE, title);
        BatteryGraph_Draw(c, zoom != 0);
        Draw_DrawString(10, 240 - 18, COLOR_TITLE, zoom ? "Y: full view  B: back" : "Y: zoom 0-10%  B: back");
        Draw_DrawString(320 - 4 - 19 * SPACING_X, 240 - 18, COLOR_WHITE, "x: MCU %, y: real %");
        snprintf(path, sizeof(path), "%s_%s.ppm", argv[2], zoom ? "zoom" : "full");
        writePpm(path);
    }
    return 0;
}
