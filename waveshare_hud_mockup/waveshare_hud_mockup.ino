/*
  waveshare_hud_mockup.ino — HUD на живых данных CAN (v7)
  ------------------------------------------------------------------------------
  v7: данные со сниффера по BLE (ACL-поток, fw сниффера 2.6.0+), декодирование
      в HUD (can_decode.c), обновление экрана — lv_timer внутри задачи LVGL.
      loop() больше не трогает LVGL.
      Настройки — hud_config.h (HUD_DATA_SOURCE: BLE / свой трансивер / генератор).
  Библиотеки: lvgl 8.x, NimBLE-Arduino 2.x.
*/

#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "i2c_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "lvgl.h"
#include "hud_mockup.h"
#include "hud_source.h"
#include "hud_sound.h"
#include "hud_log.h"
#include "hud_ota.h"
#include "hud_config.h"

void lvgl_log_cb(const char *buf)
{
  if (!HUD_LOG_LVGL) return;
  hud_log_write("[LVGL] ");
  hud_log_write(buf);
}

void setup()
{
  i2c_master_Init();
  Serial.begin(115200);
  delay(300);
  hud_log_init();                   /* журнал: Serial + SD-карта (HUD_LOG_TO_SD) */
  hud_settings_load();              /* язык, единицы, знаки, бар — из NVS (меню) */
  hud_log_write("=== setup() start ===\n");
  bool ota = hud_ota_check_boot();  /* BOOT удерживали -> загрузка в режиме обновления (Wi-Fi) */

  lv_log_register_print_cb(lvgl_log_cb);

  lvgl_port_init();                 /* строит экран и запускает lv_timer */
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  hud_log_write("=== экран готов ===\n");

  if (ota) {                        /* режим обновления: только Wi-Fi + страница загрузки */
    hud_ota_run();
    return;
  }

  hud_sound_start();                /* звук: своя задача, стартовый звук */
  hud_ota_start_button();           /* удержание BOOT -> режим обновления */

  hud_source_start();               /* источник кадров: BLE / TWAI / генератор */
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
