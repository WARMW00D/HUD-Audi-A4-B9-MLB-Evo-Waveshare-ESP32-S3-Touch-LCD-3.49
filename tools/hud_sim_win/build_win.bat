@echo off
rem Build hud_sim.exe (HUD simulator for Windows).
rem Needs: git + a C compiler in PATH: zig (ziglang.org, one zip) or MinGW-w64 gcc.
rem LVGL 8.4.0 is cloned into .\lvgl on the first run; objects go to .\build.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "FW=../../waveshare_hud_mockup"
set "CC="
where zig >nul 2>nul && set "CC=zig cc -target x86_64-windows-gnu"
if not defined CC where gcc >nul 2>nul && set "CC=gcc"
if not defined CC (
  echo ERROR: neither zig nor gcc found in PATH
  exit /b 1
)
echo Compiler: %CC%

if not exist lvgl\lvgl.h (
  git clone --depth 1 -b v8.4.0 https://github.com/lvgl/lvgl.git lvgl || exit /b 1
)

set "CF=-O2 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I. -Ishim -Ilvgl -I%FW%"
if not exist build\lv mkdir build\lv

rem ---- LVGL: compiled once (delete .\build to rebuild) ----
if not exist build\lv.rsp (
  echo [1/3] LVGL, a few minutes...
  set N=0
  for /r lvgl\src %%f in (*.c) do (
    set /a N+=1
    %CC% %CF% -w -c "%%f" -o build\lv\!N!.o || exit /b 1
    echo build/lv/!N!.o>>build\lv.tmp
  )
  ren build\lv.tmp lv.rsp
)

rem ---- HUD firmware sources, unchanged ----
echo [2/3] HUD firmware sources...
set "OBJS="
for %%n in (psd_speedlimit cam_alert hud_anim hud_images hud_min_images hud_nav_images hud_sounds hud_font_fuel hud_font_gear hud_font_kmh hud_font_menu hud_font_route hud_font_sign2 hud_font_sign3 hud_font_small hud_font_speed hud_font_mspeed hud_font_mdist hud_font_munit22 hud_font_munit20 hud_font_mcam hud_font_msign2 hud_font_msign3 hud_font_mcdisc2 hud_font_mcdisc3) do (
  %CC% %CF% -w -c %FW%/%%n.c -o build\%%n.o || exit /b 1
  set "OBJS=!OBJS! build/%%n.o"
)

echo [3/3] simulator...
%CC% %CF% -w -c sim_mockup.c -o build\sim_mockup.o || exit /b 1
%CC% %CF% -w -c sim_decode_mlb.c -o build\sim_decode_mlb.o || exit /b 1
%CC% %CF% -w -c sim_decode_mqb.c -o build\sim_decode_mqb.o || exit /b 1
%CC% %CF% -Wall -c sim_core.c -o build\sim_core.o || exit /b 1
%CC% %CF% -Wall -c sim_win.c -o build\sim_win.o || exit /b 1
%CC% -o hud_sim.exe build/sim_win.o build/sim_core.o build/sim_decode_mlb.o build/sim_decode_mqb.o build/sim_mockup.o %OBJS% @build/lv.rsp -municode -mwindows -lgdi32 -luser32 -lcomdlg32 -lcomctl32 -lshell32 -lole32 -lwinmm -lgdiplus -s || exit /b 1
echo OK: hud_sim.exe
