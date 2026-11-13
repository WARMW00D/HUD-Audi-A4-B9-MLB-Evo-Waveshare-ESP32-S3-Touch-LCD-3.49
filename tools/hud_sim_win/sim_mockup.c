/*
  sim_mockup.c — экран прошивки (hud_mockup.c) с одной поправкой для стола: меню настроек HUD
  открывается на любой скорости. В машине оно разрешено только на стоящей (HUD_MENU_ONLY_STOPPED 1
  в hud_config.h) и само закрывается, когда тронулись; при проигрывании лога это только мешает.
  Сам hud_mockup.c не меняется. Нужно поведение как в машине — уберите две строки #undef / #define.
*/
#include "hud_config.h"
#undef  HUD_MENU_ONLY_STOPPED
#undef  HUD_LOG_MEM                  /* счётчик памяти [mem] — только для платы */
#define HUD_LOG_MEM 0
#define HUD_MENU_ONLY_STOPPED 0
#include "hud_mockup.c"
