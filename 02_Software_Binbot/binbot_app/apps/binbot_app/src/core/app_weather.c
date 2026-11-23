#include "binbot/app_weather.h"
#include "binbot/component/pen.h"
#include <stddef.h>

const app_weather_info_t *app_weather_info(unsigned condition)
{
    static const app_weather_info_t conditions[BB_PEN_CONDITION_COUNT] = {
        [BB_PEN_SUN] = {"晴", "sun"}, [BB_PEN_CLOUD] = {"多云", "cloudy"},
        [BB_PEN_RAIN] = {"小雨", "rain"}, [BB_PEN_SNOW] = {"小雪", "snow"},
        [BB_PEN_STORM] = {"雷阵雨", "storm"}, [BB_PEN_FOG] = {"雾", "fog"},
        [BB_PEN_OVERCAST] = {"阴", "overcast"}, [BB_PEN_NIGHT] = {"晴夜", "night"},
        [BB_PEN_CLOUD_NIGHT] = {"多云夜", "cloudynight"}, [BB_PEN_SHOWER] = {"阵雨", "shower"},
        [BB_PEN_HEAVY_RAIN] = {"大雨", "heavyrain"}, [BB_PEN_DOWNPOUR] = {"暴雨", "downpour"},
        [BB_PEN_SLEET] = {"雨夹雪", "sleet"}, [BB_PEN_HEAVY_SNOW] = {"大雪", "heavysnow"},
        [BB_PEN_HAIL] = {"冰雹", "hail"}, [BB_PEN_WIND] = {"大风", "wind"},
        [BB_PEN_HAZE] = {"霾", "haze"}, [BB_PEN_DUST] = {"沙尘", "dust"},
        [BB_PEN_TYPHOON] = {"台风", "typhoon"}, [BB_PEN_HOT] = {"高温", "hot"},
    };
    if(condition >= BB_PEN_CONDITION_COUNT || !conditions[condition].name) return NULL;
    return &conditions[condition];
}
