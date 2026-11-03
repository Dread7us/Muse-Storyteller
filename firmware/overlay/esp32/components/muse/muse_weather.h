#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MUSE_WEATHER_DAYS 5
typedef struct {char date[11];int code,rain;float high,low;} muse_weather_day_t;
typedef struct {bool valid,busy;uint32_t version,age_seconds,retry_seconds;char time[20],note[64];float temperature,feels,wind;int code;muse_weather_day_t days[MUSE_WEATHER_DAYS];} muse_weather_view_t;
/* Start the background service; first fetch waits for connected Wi-Fi. */
void muse_weather_init(void);
void muse_weather_request(void);
void muse_weather_copy(muse_weather_view_t *out);
const char *muse_weather_description(int code);
