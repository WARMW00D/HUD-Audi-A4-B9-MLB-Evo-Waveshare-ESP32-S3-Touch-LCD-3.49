/*
  can_twai_source.cpp — свой CAN-трансивер (Adafruit CAN Pal, TJA1051T/3) на I-CAN
  ------------------------------------------------------------------------------
  Встроенный контроллер TWAI ESP32-S3 в режиме LISTEN_ONLY: ничего не передаёт,
  не подтверждает кадры (ACK), в шину не вмешивается. Все кадры шины идут в тот
  же декодер can_decode_frame(), что и кадры от BLE-сниффера. ACL тут не нужен:
  декодер сам отбрасывает ненужные ID за один switch.

  Защита от передачи в шину — в два слоя:
    1. TWAI_MODE_LISTEN_ONLY (контроллер всегда держит TX в рецессиве);
    2. вывод SLNT на CAN Pal подтянут к 3V3 (передатчик TJA1051 выключен).
  Вывод TX на CAN Pal подключён к GPIO2 (TWAI TX): в воздухе его оставлять
  нельзя, на части модулей это ломает приём.
  Терминатор на CAN Pal — ВЫКЛ: шина в машине уже согласована.

  Подключение (таблица GPIO Waveshare 3.49): CAN Pal RX -> GPIO1, TX -> GPIO2.
*/
#include <Arduino.h>
#include "hud_config.h"
#include "hud_data.h"
#include "hud_source.h"
#include "hud_log.h"

#if HUD_DATA_SOURCE == HUD_SRC_TWAI || HUD_DATA_SOURCE == HUD_SRC_AUTO

#include "driver/twai.h"

static void twai_log(const char *s) { if (HUD_LOG_TWAI) hud_log_write(s); }
static void serial_line(const char *s) { hud_log_write(s); }

static bool twai_up(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)HUD_CAN_TX_PIN,
                                                          (gpio_num_t)HUD_CAN_RX_PIN,
                                                          TWAI_MODE_LISTEN_ONLY);
    g.tx_queue_len   = 0;          /* передачи нет */
    g.rx_queue_len   = 128;        /* I-CAN ~1500-1700 кадров/с, запас на всплески */
    g.alerts_enabled = TWAI_ALERT_NONE;

#if   HUD_CAN_BITRATE_K == 500
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
#elif HUD_CAN_BITRATE_K == 250
    twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
#elif HUD_CAN_BITRATE_K == 1000
    twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
#else
#error "HUD_CAN_BITRATE_K: поддерживаются 250, 500, 1000"
#endif
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t e = twai_driver_install(&g, &t, &f);
    if (e != ESP_OK) {
        if (HUD_LOG_TWAI) arduino_printf("[TWAI] driver_install: %s\n", esp_err_to_name(e));
        return false;
    }
    e = twai_start();
    if (e != ESP_OK) {
        if (HUD_LOG_TWAI) arduino_printf("[TWAI] start: %s\n", esp_err_to_name(e));
        twai_driver_uninstall();
        return false;
    }
    if (HUD_LOG_TWAI) arduino_printf("[TWAI] LISTEN_ONLY %u кбит/с, RX=GPIO%d, TX=GPIO%d\n",
                                    (unsigned)HUD_CAN_BITRATE_K, HUD_CAN_RX_PIN, HUD_CAN_TX_PIN);
    return true;
}

static void twai_task(void *)
{
    while (!twai_up()) vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t last_rx = 0, last_chk = 0, last_stat = millis();
    uint32_t frames = 0, gap_max = 0;
    bool link = false;

    for (;;) {
        twai_message_t m;
        if (twai_receive(&m, pdMS_TO_TICKS(50)) == ESP_OK) {
            if (!m.rtr && m.data_length_code <= 8)
                can_decode_frame(m.identifier, m.extd, m.data, m.data_length_code);
            last_rx = millis();
            frames++;
            hud_src_twai_frame();
        }

        uint32_t now = millis();
        if (now - last_chk >= 250) {
            last_chk = now;
            /* пауза между кадрами скорости (0x30B): длиннее 200 мс — сразу в журнал вместе со счётчиками драйвера,
               чтобы отличить потерю кадров на шине/в очереди от проблем отрисовки */
            uint32_t g = hud_data_take_speed_gap_max();
            if (g > gap_max) gap_max = g;
            if (g >= 200 && HUD_LOG_TWAI) {
                twai_status_info_t si = {};
                twai_get_status_info(&si);
                arduino_printf("[TWAI] пауза 0x30B %lu мс; очередь: в ожидании %lu, потеряно %lu, переполнение RX %lu; ошибок шины %lu; состояние %d\n",
                               (unsigned long)g, (unsigned long)si.msgs_to_rx, (unsigned long)si.rx_missed_count,
                               (unsigned long)si.rx_overrun_count, (unsigned long)si.bus_error_count, (int)si.state);
            }
            bool up = last_rx && (now - last_rx) < 1000;     /* шина молчит > 1 с — связи нет */
            if (up != link) {
                link = up;
                hud_src_set_up(HUD_SRC_ID_TWAI, up);
                twai_log(up ? "[TWAI] кадры идут\n" : "[TWAI] шина молчит\n");
            }
            twai_status_info_t st;
            if (twai_get_status_info(&st) == ESP_OK) {
                if (st.state == TWAI_STATE_BUS_OFF) {        /* в LISTEN_ONLY не должно быть */
                    twai_log("[TWAI] BUS_OFF, восстановление\n");
                    twai_initiate_recovery();
                } else if (st.state == TWAI_STATE_STOPPED) {
                    twai_start();
                }
            }
        }

        if (now - last_stat >= 10000) {
            last_stat = now;
            if (HUD_LOG_STAT) {
                twai_status_info_t st = {};
                twai_get_status_info(&st);
                arduino_printf("[TWAI] кадров %lu за 10 с, потеряно (очередь) %lu, переполнение %lu, ошибок шины %lu, макс. пауза 0x30B %lu мс\n",
                              (unsigned long)frames, (unsigned long)st.rx_missed_count,
                              (unsigned long)st.rx_overrun_count, (unsigned long)st.bus_error_count,
                              (unsigned long)gap_max);
            }
            if (HUD_LOG_CAN) hud_data_debug_print(serial_line);
            frames = 0; gap_max = 0;
        }
    }
}

void twai_source_start(void)
{
    xTaskCreatePinnedToCore(twai_task, "can_twai", 4096, nullptr, 4, nullptr, 1);
}

#endif  /* HUD_SRC_TWAI || HUD_SRC_AUTO */
