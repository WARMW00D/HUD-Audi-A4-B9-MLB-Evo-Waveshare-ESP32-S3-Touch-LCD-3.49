/* Сгенерировано tools/gen_min_images.py — не править вручную */
#ifndef HUD_MIN_IMAGES_H
#define HUD_MIN_IMAGES_H
#include "lvgl.h"

/* иконка ACC минимального режима: рамка 157 x 122; X/Y — смещение обрезанной картинки в рамке */
#define M_ACC_W 157
#define M_ACC_H 122
extern const lv_img_dsc_t img_m_lane_l;
#define M_LANE_L_X 0
#define M_LANE_L_Y 19
extern const lv_img_dsc_t img_m_lane_r;
#define M_LANE_R_X 108
#define M_LANE_R_Y 19
extern const lv_img_dsc_t img_m_lead;
#define M_LEAD_X 46
#define M_LEAD_Y 0
extern const lv_img_dsc_t img_m_arcs;
#define M_ARCS_X 48
#define M_ARCS_Y 36
extern const lv_img_dsc_t img_m_own;
#define M_OWN_X 30
#define M_OWN_Y 64
extern const lv_img_dsc_t img_m_swa;
extern const lv_img_dsc_t img_m_cam;

/* та же иконка для полного режима: рамка 64 x 50 */
#define F_ACC_W 64
#define F_ACC_H 50
extern const lv_img_dsc_t img_f_lane_l;
#define F_LANE_L_X 0
#define F_LANE_L_Y 6
extern const lv_img_dsc_t img_f_lane_r;
#define F_LANE_R_X 42
#define F_LANE_R_Y 6
extern const lv_img_dsc_t img_f_lead;
#define F_LEAD_X 18
#define F_LEAD_Y 0
extern const lv_img_dsc_t img_f_arcs;
#define F_ARCS_X 18
#define F_ARCS_Y 13
extern const lv_img_dsc_t img_f_own;
#define F_OWN_X 11
#define F_OWN_Y 25

#endif
