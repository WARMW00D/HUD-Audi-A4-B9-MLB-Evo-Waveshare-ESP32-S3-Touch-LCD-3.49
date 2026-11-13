#!/bin/sh
# Сборка hud_shot — ядро симулятора без окна (Linux/macOS/WSL): кадр HUD из лога в BMP.
#   mode=1 ./hud_shot ...  — минимальный экран (по умолчанию полный)
#   ./hud_shot can_log_0004.txt 215 out.bmp [cams.bin]
set -e
cd "$(dirname "$0")"
FW=../../waveshare_hud_mockup
[ -f lvgl/lvgl.h ] || git clone --depth 1 -b v8.4.0 https://github.com/lvgl/lvgl.git lvgl
${CC:-cc} -O2 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I. -Ishim -Ilvgl -I$FW \
  sim_headless.c sim_core.c sim_decode_mlb.c sim_decode_mqb.c sim_mockup.c \
  $FW/psd_speedlimit.c $FW/cam_alert.c $FW/hud_anim.c \
  $FW/hud_images.c $FW/hud_min_images.c $FW/hud_nav_images.c $FW/hud_sounds.c $FW/hud_font_*.c \
  $(find lvgl/src -name '*.c') -lm -o hud_shot
echo OK: hud_shot
