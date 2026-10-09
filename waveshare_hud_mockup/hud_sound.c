/*
  hud_sound.c — звук HUD: кодек ES8311 + усилитель на плате Waveshare 3.49
  ------------------------------------------------------------------------------
  Своя задача FreeRTOS на ядре 1 с низким приоритетом: LVGL (ядро 0) и приём
  CAN от неё не зависят. I2S отдаёт данные через DMA, так что звук не
  прерывается, даже если экран занят перерисовкой.

  Разводка (таблица GPIO платы): I2S MCLK=GPIO7, BCLK=GPIO15, WS=GPIO46,
  DOUT=GPIO45; управление ES8311 по I2C порт 0 (GPIO47/48, общая шина BSP);
  режим усилителя — EXIO7 (NS_MODE) расширителя TCA9554, ставится в lvgl_port.c.

  Звуки — PCM 16 бит моно 22050 Гц во флеше (hud_sounds.c, tools/make_sounds.py).
*/
#include "hud_config.h"
#include "hud_sound.h"

#if HUD_SOUND_ENABLE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"
#include "src/esp_codec_dev/include/esp_codec_dev_defaults.h"
#include "hud_sounds.h"

#define SND_I2S_PORT   I2S_NUM_0
#define SND_PIN_MCLK   GPIO_NUM_7
#define SND_PIN_BCLK   GPIO_NUM_15
#define SND_PIN_WS     GPIO_NUM_46
#define SND_PIN_DOUT   GPIO_NUM_45

#ifdef __cplusplus
extern "C" {
#endif
    void arduino_printf(const char *format, ...);
#ifdef __cplusplus
}
#endif

typedef struct { const int16_t *pcm; uint32_t n; } SndJob;

static QueueHandle_t          s_q;
static esp_codec_dev_handle_t s_dev;
static volatile uint8_t       s_vol = HUD_SOUND_VOLUME;
static uint8_t                s_vol_set = 0xFF;
static i2s_chan_handle_t      s_tx;

static bool snd_hw_init(void)
{
    /* I2S: стандартный Philips, 16 бит, стерео-слоты (моно дублируем), MCLK = 256*fs */
    i2s_chan_handle_t tx = NULL;                           /* сохраняется в s_tx */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(SND_I2S_PORT, I2S_ROLE_MASTER);
    cc.auto_clear = true;                                  /* тишина при опустевшем DMA */
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(HUD_SOUND_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = SND_PIN_MCLK, .bclk = SND_PIN_BCLK, .ws = SND_PIN_WS,
            .dout = SND_PIN_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;
    s_tx = tx;
    i2s_channel_enable(tx);       /* esp_codec_dev_open() сначала выключает канал — без этого в лог
                                     падает безобидная ошибка "channel has not been enabled yet" */

    /* ES8311 по I2C — шина порта 0 уже создана BSP (i2c_master_Init) */
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(0, &bus) != ESP_OK || !bus) return false;

    audio_codec_i2s_cfg_t i2s_cfg = { .port = SND_I2S_PORT, .rx_handle = NULL, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) return false;

    es8311_codec_cfg_t es = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,                                 /* усилитель включён всегда (NS_MODE) */
        .use_mclk   = true,
    };
    es.hw_gain.pa_gain = 6;                               /* как в примере Waveshare */
    const audio_codec_if_t *codec_if = es8311_codec_new(&es);
    if (!codec_if) return false;

    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if };
    s_dev = esp_codec_dev_new(&dc);
    return s_dev != NULL;
}

static void snd_play_now(const int16_t *pcm, uint32_t n)
{
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = HUD_SOUND_RATE };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) return;
    if (s_vol != s_vol_set) { esp_codec_dev_set_out_vol(s_dev, s_vol); s_vol_set = s_vol; }

    static int16_t st[2 * 256];                           /* моно -> стерео кусками */
    for (uint32_t i = 0; i < n; ) {
        uint32_t k = n - i < 256 ? n - i : 256;
        for (uint32_t j = 0; j < k; j++) st[2 * j] = st[2 * j + 1] = pcm[i + j];
        esp_codec_dev_write(s_dev, st, (int)(k * 2 * sizeof(int16_t)));
        i += k;
    }
    memset(st, 0, sizeof st);                             /* хвост тишины, чтобы DMA доиграл */
    for (int r = 0; r < 4; r++) esp_codec_dev_write(s_dev, st, sizeof st);
    esp_codec_dev_close(s_dev);
    /* close() выключает канал I2S, а следующий open() сначала его выключает снова —
       отсюда безобидная ошибка "channel has not been enabled yet". Включаем канал обратно. */
    if (s_tx) i2s_channel_enable(s_tx);
}

static void snd_task(void *arg)
{
    (void)arg;
    if (!snd_hw_init()) {
        if (HUD_LOG_HUD) arduino_printf("[snd] не удалось запустить кодек ES8311 / I2S\n");
        vTaskDelete(NULL);
        return;
    }
    if (HUD_LOG_HUD) arduino_printf("[snd] звук готов, громкость %u\n", (unsigned)s_vol);
#if HUD_SOUND_STARTUP
    vTaskDelay(pdMS_TO_TICKS(HUD_SOUND_STARTUP_DELAY_MS));
    snd_play_now(hud_snd_startup, hud_snd_startup_len);
#endif
    SndJob j;
    for (;;)
        if (xQueueReceive(s_q, &j, portMAX_DELAY) == pdTRUE) snd_play_now(j.pcm, j.n);
}

void hud_sound_start(void)
{
    s_q = xQueueCreate(4, sizeof(SndJob));
    xTaskCreatePinnedToCore(snd_task, "hud_snd", 4096, NULL, 2, NULL, 1);
}

void hud_sound_play(const int16_t *pcm, uint32_t samples)
{
    if (!s_q || !pcm || !samples) return;
    SndJob j = { pcm, samples };
    xQueueSend(s_q, &j, 0);                               /* очередь полна — звук пропускаем */
}

void hud_sound_set_volume(uint8_t vol) { s_vol = vol > 100 ? 100 : vol; }
uint8_t hud_sound_get_volume(void)     { return s_vol; }

#else   /* звук выключен — пустые функции */
void hud_sound_start(void) {}
void hud_sound_play(const int16_t *pcm, uint32_t samples) { (void)pcm; (void)samples; }
void hud_sound_set_volume(uint8_t vol) { (void)vol; }
uint8_t hud_sound_get_volume(void)     { return 0; }
#endif
