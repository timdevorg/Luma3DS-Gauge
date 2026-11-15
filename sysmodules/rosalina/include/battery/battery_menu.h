/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: Rosalina submenu.
*/

#pragma once

#include "menu.h"

extern Menu batteryMenu;

void BatteryMenu_Status(void);
void BatteryMenu_StartRecording(void);
void BatteryMenu_StopRecording(void);
void BatteryMenu_Sessions(void);
void BatteryMenu_BuildCurve(void);
void BatteryMenu_ShowGraph(void);
void BatteryMenu_LedOverride(void);
void BatteryMenu_SleepLed(void);
void BatteryMenu_Bars(void);
void BatteryMenu_Thresholds(void);
void BatteryMenu_LedTest(void);

/// Sets the "LED override: ON/OFF" item title (called when config.ini is loaded / changed).
void BatteryMenu_UpdateLedTitle(bool on);
/// Sets the "Sleep LED: ..." item title (called when config.ini is loaded / changed).
void BatteryMenu_UpdateSleepLedTitle(u8 sleepMode);
/// Sets the "Battery bars: corrected / stock" item title (called when config.ini is loaded / changed).
void BatteryMenu_UpdateBarsTitle(bool corrected);
