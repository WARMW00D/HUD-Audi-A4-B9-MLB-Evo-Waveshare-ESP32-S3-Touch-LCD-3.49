/*
  sim_decode_mqb.c — декодер прошивки (can_decode.c), собранный для платформы MQB.
  В прошивке платформа выбирается при сборке (HUD_PLATFORM в hud_config.h); симулятор собирает
  can_decode.c дважды — с разными именами функций — и переключает их на ходу (sim_core.c).
*/
#define HUD_PLATFORM 2                      /* = HUD_PLATFORM_MQB — hud_config.h своё значение уже не подставит */
#define hud_now_ms                  mqb_hud_now_ms
#define hud_data_snapshot           mqb_hud_data_snapshot
#define hud_data_set_link           mqb_hud_data_set_link
#define hud_clock_now               mqb_hud_clock_now
#define hud_data_note_lost          mqb_hud_data_note_lost
#define hud_data_take_speed_gap_max mqb_hud_data_take_speed_gap_max
#define hud_data_debug_print        mqb_hud_data_debug_print
#define hud_data_fake_doors         mqb_hud_data_fake_doors
#define can_decode_frame            mqb_can_decode_frame
#include "can_decode.c"
