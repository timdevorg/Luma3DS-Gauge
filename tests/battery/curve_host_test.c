/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the Battery Manager curve code (sysmodules/rosalina/source/battery/battery_curve.c).
*   Builds a curve from session CSVs exactly like the console does (same two passes, same
*   functions), writes curve.bin and prints the same summary / report as
*   `scripts/build_curve.py --quiet`, then runs a few self-checks (monotonicity, continuity,
*   checksum). Run it through tests/battery/run_host_test.sh.
*
*   Usage: curve_host_test out.bin session.csv [...]
*   The sessions are used as given (selection / classification is done by the caller).
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "battery/battery_curve.h"

static BatteryCurveSession g_session;
static BatteryCurveAccum g_accum;
static BatteryCurveFile g_file;

typedef struct Row { double t, pct; int volt; } Row;
static Row g_rows[20000];
static u32 g_numRows;

// Streams one CSV into g_session, like battery_curve_sd.c does (line by line). Returns the session number.
static u32 feedFile(const char *path, bool keepRows)
{
    char line[256];
    u32 number = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        perror(path);
        exit(2);
    }

    BatteryCurve_SessionBegin(&g_session);
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "# session=", 10) == 0)
            number = (u32)atoi(line + 10);
        BatteryCurve_SessionFeedLine(&g_session, line);

        unsigned t, pi, pf, v;
        long long wall;
        if (keepRows && g_numRows < sizeof(g_rows) / sizeof(g_rows[0]) &&
            sscanf(line, "%u,%lld,%u,%u,%*[^,],%u,", &t, &wall, &pi, &pf, &v) == 5)
        {
            g_rows[g_numRows].t = t;
            g_rows[g_numRows].pct = pi + pf / 256.0;
            g_rows[g_numRows].volt = (int)v;
            g_numRows++;
        }
    }
    fclose(f);

    BatteryCurveSessionError err = BatteryCurve_SessionEnd(&g_session);
    if (err != BATTERY_CURVE_SESSION_OK)
    {
        fprintf(stderr, "%s: rejected (%d)\n", path, (int)err);
        exit(2);
    }
    return number;
}

static int g_failures;

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        g_failures++;
    }
}

static void printReportRow(const char *label, const Row *r, double tEnd)
{
    printf("  %-10s %7.1f %7.3f %3d %6.2f %6.2f %6.2f\n", label, r->t / 60000, r->pct, r->volt,
           100 * (tEnd - r->t) / tEnd,
           (double)BatteryCurve_Interp(g_file.mcuGridX10, g_file.mcuMean, BATTERY_CURVE_MCU_POINTS, (float)(r->pct * 10)),
           (double)BatteryCurve_CorrectedFrom(&g_file, (float)r->pct, (float)r->volt));
}

static void selfChecks(void)
{
    // Monotonic and continuous in pct (per voltage) and in voltage (per pct)
    float maxStepPct = 0, maxStepVolt = 0;
    bool monoPct = true, monoVolt = true;
    for (int vi = 0; vi <= 320; vi++)
    {
        float v = 140.0f + vi * 0.25f;
        float prev = -1;
        for (int pi = 0; pi <= 10000; pi++)
        {
            float c = BatteryCurve_CorrectedFrom(&g_file, pi * 0.01f, v);
            if (prev >= 0)
            {
                if (c < prev - 1e-4f)
                    monoPct = false;
                if (c - prev > maxStepPct)
                    maxStepPct = c - prev;
            }
            prev = c;
        }
    }
    for (int pi = 0; pi <= 1000; pi++)
    {
        float p = pi * 0.01f;
        float prev = -1;
        for (int vi = 0; vi <= 320; vi++)
        {
            float c = BatteryCurve_CorrectedFrom(&g_file, p, 140.0f + vi * 0.25f);
            if (prev >= 0)
            {
                if (c < prev - 1e-4f)
                    monoVolt = false;
                if (c - prev > maxStepVolt)
                    maxStepVolt = c - prev;
            }
            prev = c;
        }
    }
    printf("self-check: monotonic in pct %s (max step %.2f per 0.01 %%), in volt %s (max step %.2f per 0.25 raw)\n",
           monoPct ? "yes" : "NO", (double)maxStepPct, monoVolt ? "yes" : "NO", (double)maxStepVolt);
    check(monoPct, "corrected % not monotonic in MCU %");
    check(monoVolt, "corrected % not monotonic in voltage");
    check(maxStepPct < 1.0f, "corrected % jumps in MCU % (discontinuity)");
    check(maxStepVolt < 2.0f, "corrected % jumps in voltage (discontinuity)");

    check(BatteryCurve_FileIsValid(&g_file), "file not valid");
    BatteryCurveFile bad = g_file;
    bad.mcuMean[80] ^= 1;
    check(!BatteryCurve_FileIsValid(&bad), "corrupted file accepted");
    bad = g_file;
    bad.version = 2;
    check(!BatteryCurve_FileIsValid(&bad), "wrong version accepted");
    check(BatteryCurve_CorrectedFrom(&g_file, 100.0f, 0) == g_file.mcuMean[BATTERY_CURVE_MCU_POINTS - 1] / 100.0f, "clamp at 100 %");
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc - 2 > BATTERY_CURVE_MAX_SESSIONS)
    {
        fprintf(stderr, "usage: %s out.bin session.csv [...] (max %d sessions)\n", argv[0], BATTERY_CURVE_MAX_SESSIONS);
        return 2;
    }

    u32 n = (u32)(argc - 2);
    u16 numbers[BATTERY_CURVE_MAX_SESSIONS];
    double durations[BATTERY_CURVE_MAX_SESSIONS], first[BATTERY_CURVE_MAX_SESSIONS], last[BATTERY_CURVE_MAX_SESSIONS];

    // Pass 1: sum / min / max
    BatteryCurve_AccumInit(&g_accum);
    for (u32 i = 0; i < n; i++)
    {
        numbers[i] = (u16)feedFile(argv[2 + i], i == 0);
        durations[i] = g_session.lastT;
        first[i] = g_session.firstPct;
        last[i] = g_session.lastPct;
        BatteryCurve_AccumAdd(&g_accum, &g_session);
    }
    BatteryCurve_AccumFinish(&g_accum, &g_file);

    // Pass 2: deviation of each session from the others
    for (u32 i = 0; i < n; i++)
    {
        feedFile(argv[2 + i], false);
        g_file.sessions[i] = numbers[i];
        g_file.sessionDevX100[i] = BatteryCurve_Deviation(&g_accum, &g_session);
        if (g_file.sessionDevX100[i] > BATTERY_CURVE_OUTLIER_X100)
            g_file.numOutliers++;
    }
    BatteryCurve_FileFinalize(&g_file);

    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(&g_file, sizeof(g_file), 1, f) != 1)
    {
        perror(argv[1]);
        return 2;
    }
    fclose(f);

    printf("sessions used: %u, max spread: %u.%02u points, outliers: %u\n", n,
           g_file.maxSpreadX100 / 100, g_file.maxSpreadX100 % 100, g_file.numOutliers);
    for (u32 i = 0; i < n; i++)
    {
        u16 d = g_file.sessionDevX100[i];
        printf("  #%04u  %.2f h  %.1f -> %.1f %%  deviation %u.%02u%s\n", numbers[i], durations[i] / 3600000,
               first[i], last[i], d / 100, d % 100, d > BATTERY_CURVE_OUTLIER_X100 ? "  OUTLIER" : "");
    }

    // Same rows as build_curve.py's report_rows()
    printf("\nReport (session #%04u): label, t_min, mcu_pct, volt_raw, real_time_based, mcu_table, corrected\n", numbers[0]);
    double tEnd = g_rows[g_numRows - 1].t;
    static const int targets[] = { 50, 25, 10, 5, 3, 2, 1 };
    for (u32 k = 0; k < sizeof(targets) / sizeof(targets[0]); k++)
        for (u32 i = 0; i < g_numRows; i++)
            if (g_rows[i].pct <= targets[k])
            {
                char label[16];
                sprintf(label, "mcu<=%d", targets[k]);
                printReportRow(label, &g_rows[i], tEnd);
                break;
            }
    static const int mins[] = { 15, 10, 5, 2, 0 };
    for (u32 k = 0; k < sizeof(mins) / sizeof(mins[0]); k++)
        for (u32 i = 0; i < g_numRows; i++)
            if (g_rows[i].t >= tEnd - mins[k] * 60000.0)
            {
                char label[16];
                sprintf(label, "end-%dmin", mins[k]);
                printReportRow(label, &g_rows[i], tEnd);
                break;
            }

    printf("\n");
    selfChecks();
    printf("%s\n", g_failures == 0 ? "host test: OK" : "host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
