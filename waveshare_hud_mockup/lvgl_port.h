#ifndef LVGL_PORT_H
#define LVGL_PORT_H
#include <stdbool.h>



#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
bool hud_touch_take_menu_request(void);   /* HUD: двойное/одиночное касание -> меню */


#ifdef __cplusplus
}
#endif



#endif










