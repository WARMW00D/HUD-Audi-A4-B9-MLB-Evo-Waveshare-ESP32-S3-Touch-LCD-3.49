/*
  hud_source.cpp — выбор источника CAN-кадров
  ------------------------------------------------------------------------------
  HUD_SRC_BLE / _TWAI / _FAKE — один источник. HUD_SRC_AUTO — работают BLE и
  TWAI одновременно: пока по своему трансиверу идут кадры (последний не старше
  HUD_AUTO_CAN_HOLD_MS), кадры из BLE отбрасываются, а соединение BLE
  разрывается; CAN замолчал — BLE снова подключается и используется.
  Значок «нет связи» горит, только если связи нет ни у одного источника.
*/
#include <Arduino.h>
#include "hud_config.h"
#include "hud_data.h"
#include "hud_log.h"
#include "hud_source.h"

static volatile uint32_t s_twai_last = 0;
static volatile bool     s_up[3]     = { false, false, false };

void hud_src_twai_frame(void) { uint32_t t = millis(); s_twai_last = t ? t : 1; }

bool hud_src_twai_active(void)
{
    uint32_t l = s_twai_last;
    return l && (millis() - l) < HUD_AUTO_CAN_HOLD_MS;
}

static volatile bool s_pair_failed = false;
void hud_src_set_pair_failed(bool on) { s_pair_failed = on; }
bool hud_src_pair_failed(void)        { return s_pair_failed; }

int hud_src_current(void)
{
    if (hud_src_twai_active() || s_up[HUD_SRC_ID_TWAI]) return 1;
    if (s_up[HUD_SRC_ID_BLE] || s_up[HUD_SRC_ID_FAKE])  return 0;
    return -1;
}

void hud_src_set_up(int src, bool up)
{
    if (src < 0 || src > 2) return;
    s_up[src] = up;
    hud_data_set_link(s_up[0] || s_up[1] || s_up[2]);
}

#if HUD_DATA_SOURCE == HUD_SRC_AUTO
static void auto_monitor(void *)
{
    int cur = -1;                                      /* -1 нет, 0 BLE, 1 CAN */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int now_src = hud_src_twai_active() ? 1 : (s_up[HUD_SRC_ID_BLE] ? 0 : -1);
        if (now_src != cur) {
            cur = now_src;
            if (HUD_LOG_SRC)
                hud_log_write(cur == 1 ? "[src] источник: CAN (свой трансивер)\n" :
                              cur == 0 ? "[src] источник: BLE (сниффер / гейт)\n" :
                                         "[src] источник: нет данных\n");
        }
    }
}
#endif

void hud_source_start(void)
{
    hud_data_set_link(false);
#if   HUD_DATA_SOURCE == HUD_SRC_BLE
    ble_source_start();
#elif HUD_DATA_SOURCE == HUD_SRC_TWAI
    twai_source_start();
#elif HUD_DATA_SOURCE == HUD_SRC_FAKE
    fake_source_start();
#elif HUD_DATA_SOURCE == HUD_SRC_AUTO
    twai_source_start();
    ble_source_start();
    xTaskCreatePinnedToCore(auto_monitor, "hud_src", 2560, nullptr, 1, nullptr, 1);
#else
#error "HUD_DATA_SOURCE: HUD_SRC_BLE / HUD_SRC_TWAI / HUD_SRC_FAKE / HUD_SRC_AUTO"
#endif
}
