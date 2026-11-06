/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* Unknown evidence never becomes a made-up full gauge. */
static inline bool muse_status_percent_valid(int pct){return pct>=0 && pct<=100;}
static inline int muse_status_fill(int width,int pct)
{
    return width>0 && muse_status_percent_valid(pct)?(width*pct+50)/100:0;
}
static inline int muse_status_wifi_bars(bool connected,int rssi)
{
    if(!connected || rssi>=0 || rssi < -127)return 0;
    return rssi>=-55?4:rssi>=-67?3:rssi>=-78?2:1;
}
/* Approximate received signal quality: -100 dBm = 0%, -50 dBm = 100%.
 * This describes radio strength, not throughput. A failed RSSI read stays unknown. */
static inline int muse_status_wifi_percent(bool connected,int rssi)
{
    if(!connected)return 0;
    if(rssi>=0 || rssi < -127)return -1;
    return rssi<=-100?0:rssi>=-50?100:2*(rssi+100);
}
static inline unsigned muse_status_battery_color(int pct)
{
    return !muse_status_percent_valid(pct)?0x897f9c:pct<=10?0xff667b:pct<=25?0xffc267:0x6fe5b6;
}
