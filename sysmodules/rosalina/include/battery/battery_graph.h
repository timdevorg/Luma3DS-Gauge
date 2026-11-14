/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: curve graph on the bottom screen.
*/

#pragma once

#include "battery/battery_curve.h"

/// Draws axes, the stock diagonal and (if curve != NULL) the learned mean curve with its
/// min/max band. X = MCU %, Y = real %. zoom: MCU 0-10 %, real 0-50 %.
/// Draw_Lock must be held; the caller clears and flushes the framebuffer.
void BatteryGraph_Draw(const BatteryCurveFile *curve, bool zoom);
