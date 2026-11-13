/*
  sim_decode_mlb.c — декодер прошивки (can_decode.c), собранный для платформы MLB-Evo.
  В прошивке платформа выбирается при сборке (HUD_PLATFORM в hud_config.h); симулятор собирает
  can_decode.c дважды — с разными именами функций — и переключает их на ходу (sim_core.c).
*/
#define HUD_PLATFORM 1                      /* = HUD_PLATFORM_MLB_EVO — hud_config.h своё значение уже не подставит */
#define hud_now_ms                  mlb_hud_now_ms
#define hud_data_snapshot           mlb_hud_data_snapshot
#define hud_data_set_link           mlb_hud_data_set_link
#define hud_clock_now               mlb_hud_clock_now
#define hud_data_note_lost          mlb_hud_data_note_lost
#define hud_data_take_speed_gap_max mlb_hud_data_take_speed_gap_max
#define hud_data_debug_print        mlb_hud_data_debug_print
#define hud_data_fake_doors         mlb_hud_data_fake_doors
#define can_decode_frame            mlb_can_decode_frame
#include "can_decode.c"
