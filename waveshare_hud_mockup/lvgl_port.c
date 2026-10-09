#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "hud_mockup.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "user_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "src/axs15231b/esp_lcd_axs15231b.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "i2c_bsp.h"
#include "hud_config.h"

#define LCD_BIT_PER_PIXEL (16)

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;

static uint16_t *lvgl_dma_buf = NULL; 
static SemaphoreHandle_t lvgl_flush_semap;
static esp_io_expander_handle_t io_expander = NULL;

#if (Rotated == USER_DISP_ROT_90)
uint16_t* rotat_ptr = NULL;
#endif

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 100},
  {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t TaskWoken;
  xSemaphoreGiveFromISR(lvgl_flush_semap,&TaskWoken);
  return false;
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
#if (Rotated == USER_DISP_ROT_90)
  uint32_t index = 0;
  uint16_t *data_ptr = (uint16_t *)color_map;
  for (uint16_t j = 0; j < EXAMPLE_LCD_H_RES; j++)
  {
    for (uint16_t i = 0; i < EXAMPLE_LCD_V_RES; i++)
    {
      rotat_ptr[index++] = data_ptr[EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES - i - 1) + j];             
    }
  }
#endif
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
  const int offgap = (LCD_NOROT_VRES / flush_coun);
  const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
  int offsetx1 = 0;
  int offsety1 = 0;
  int offsetx2 = LCD_NOROT_HRES;
  int offsety2 = offgap;

#if (Rotated == USER_DISP_ROT_90)
  uint16_t *map = (uint16_t *)rotat_ptr;
#else
  uint16_t *map = (uint16_t *)color_map;
#endif

  xSemaphoreGive(lvgl_flush_semap);
  for(int i = 0; i<flush_coun; i++)
  {
    xSemaphoreTake(lvgl_flush_semap,portMAX_DELAY);
    memcpy(lvgl_dma_buf,map,LVGL_DMA_BUFF_LEN);
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, lvgl_dma_buf);
    offsety1 += offgap;
    offsety2 += offgap;
    map += dmalen;
  }
  xSemaphoreTake(lvgl_flush_semap,portMAX_DELAY);
  lv_disp_flush_ready(drv);
}

/* ---------------------------------------------------------------------------
   HUD: тач опрашивается в СВОЕЙ задаче (каждые 20 мс), а не из LVGL.
   Причина: пока LVGL перерисовывает крупную скорость (с красной обводкой —
   9 больших надписей), его цикл замедляется, опрос тача из LVGL становится
   редким, и короткие касания теряются — меню не открывалось на высокой
   скорости. Здесь же распознаётся одиночное / двойное касание для меню.
   --------------------------------------------------------------------------- */
static volatile bool     s_tp_pressed;
static volatile uint16_t s_tp_x, s_tp_y;
static volatile bool     s_menu_req;

static bool touch_read_hw(uint16_t *x, uint16_t *y)
{
  uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
  uint8_t buff[32] = {0};
  if (i2c_master_write_read_dev(disp_touch_dev_handle,read_touchpad_cmd,11,buff,32) != 0) return false;
  uint16_t pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  uint16_t pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  if (!(buff[1]>0 && buff[1]<5)) return false;
  /* HUD: в примере Waveshare координата на краю получалась = разрешению
     (RES - 0), а допустимо 0..RES-1 — LVGL сыпал предупреждениями
     "Y is 172 which is greater than ver. res". Теперь RES-1-p с ограничением. */
#if (Rotated == USER_DISP_ROT_NONO)
  if(pointX > EXAMPLE_LCD_V_RES - 1) pointX = EXAMPLE_LCD_V_RES - 1;
  if(pointY > EXAMPLE_LCD_H_RES - 1) pointY = EXAMPLE_LCD_H_RES - 1;
  *x = pointY;
  *y = (EXAMPLE_LCD_V_RES - 1 - pointX);
#else
  if(pointX > EXAMPLE_LCD_H_RES - 1) pointX = EXAMPLE_LCD_H_RES - 1;
  if(pointY > EXAMPLE_LCD_V_RES - 1) pointY = EXAMPLE_LCD_V_RES - 1;
  *x = (EXAMPLE_LCD_H_RES - 1 - pointX);
  *y = (EXAMPLE_LCD_V_RES - 1 - pointY);
#endif
  return true;
}

static void touch_task(void *arg)
{
  (void)arg;
  bool was = false;
  uint32_t t_down = 0, t_last_tap = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t x, y;
    bool pr = touch_read_hw(&x, &y);
    if (pr) { s_tp_x = x; s_tp_y = y; }
    s_tp_pressed = pr;
    if (pr && !was) t_down = now;
    if (!pr && was && now - t_down < 400) {               /* короткое касание отпущено */
#if HUD_MENU_DOUBLE_TAP
      if (t_last_tap && now - t_last_tap <= HUD_DOUBLE_TAP_MS) { s_menu_req = true; t_last_tap = 0; }
      else t_last_tap = now;
#else
      s_menu_req = true;
#endif
    }
    was = pr;
  }
}

/* запрос открыть меню (одиночное / двойное касание); сбрасывается при чтении */
bool hud_touch_take_menu_request(void)
{
  bool r = s_menu_req;
  s_menu_req = false;
  return r;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  (void)drv;
  data->state = s_tp_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  data->point.x = s_tp_x;
  data->point.y = s_tp_y;
}

static bool example_lvgl_lock(int timeout_ms)
{
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}

void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1)) 
    {
      task_delay_ms = lv_timer_handler();
      //Release the mutex
      example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

static void example_lcd_pwm_off_early(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_BK_LIGHT);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 0));
}

static void example_lcd_exio_init(void)
{
  if (io_expander == NULL) {
    i2c_master_bus_handle_t tca9554_i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  }

  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
#if HUD_SOUND_ENABLE
  /* усилитель звука: NS_MODE = 1 (как в примере Waveshare 08_Audio_Test) */
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, 1));
#endif
}

static void example_lcd_reset(void)
{
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 0));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
}

static void example_lcd_backlight_set(bool enable)
{
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, enable ? 1 : 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, enable ? 1 : 0));
}

void lvgl_port_init(void)
{
#if (Rotated == USER_DISP_ROT_90)
  rotat_ptr = (uint16_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  assert(rotat_ptr);
#endif
  lvgl_flush_semap = xSemaphoreCreateBinary();

  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions
  ESP_LOGI(TAG, "Initialize LCD reset and backlight");
  example_lcd_pwm_off_early();
  example_lcd_exio_init();

  ESP_LOGI(TAG, "Initialize QSPI bus");
  spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
	  esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    
  esp_lcd_panel_io_spi_config_t io_config = {};
	io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
    io_config.dc_gpio_num = -1;          
    io_config.spi_mode = 3;              
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;    
    io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
    //io_config.user_ctx = &disp_drv,         
    io_config.lcd_cmd_bits = 32;         
    io_config.lcd_param_bits = 8;        
    io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
  axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
  esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

  ESP_LOGI(TAG, "Install panel driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

  example_lcd_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  example_lcd_backlight_set(true);

  lv_init();

  lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN , MALLOC_CAP_DMA);
  assert(lvgl_dma_buf);
  lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
  lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
  assert(buffer_1);
  assert(buffer_2);
  lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

  ESP_LOGI(TAG, "Register display driver to LVGL");
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 1;          //full_refresh must be 1
  disp_drv.user_data = panel;
  lv_disp_drv_register(&disp_drv);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

#if HUD_TOUCH_ENABLE
  static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);
  xTaskCreatePinnedToCore(touch_task, "hud_touch", 3072, NULL, 3, NULL, 1);
#else
  (void)example_lvgl_touch_cb; (void)touch_task;   /* тач выключен */
#endif

  lvgl_mux = xSemaphoreCreateMutex();
  assert(lvgl_mux);
  xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 4000, NULL, 4, NULL,0); //运行于内核_0
  if (example_lvgl_lock(-1))
  {
    build_hud_mockup();
    //lv_demo_music();
    example_lvgl_unlock();
  }
}
