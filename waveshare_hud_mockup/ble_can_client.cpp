/*
  ble_can_client.cpp — приём CAN-кадров со сниффера по BLE (протокол ACL, fw 2.6.0+)
  ------------------------------------------------------------------------------
  Порядок: скан -> connect (MTU 247) -> подписка на ...0008 -> запись ACL в ...0007.
  Сниффер сбрасывает список при отключении клиента, поэтому ACL пишется
  заново после КАЖДОГО подключения.

  Написано под NimBLE-Arduino 2.x.

  Здесь же генератор кадров (HUD_SRC_FAKE). Свой трансивер — can_twai_source.cpp.
*/
#include <Arduino.h>
#include "hud_config.h"
#include "hud_data.h"
#include "hud_source.h"

/* код доступа BLE из secrets.h (нет файла или 0 — защита выключена, как раньше) */
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef BLE_PASSKEY
#define BLE_PASSKEY 0
#endif
#include "hud_log.h"

#if HUD_DATA_SOURCE == HUD_SRC_BLE || HUD_DATA_SOURCE == HUD_SRC_FAKE || HUD_DATA_SOURCE == HUD_SRC_AUTO

/* ============================================================================
   Список ACL для HUD
   ----------------------------------------------------------------------------
   Для циклических сигналов — minIntervalMs, а НЕ onchange: при onchange
   кадр с неизменной скоростью не приходит, и HUD гасит поле по таймауту.
   Для 0x366 — без ограничений: нужен каждый переход такта мигания.
   Двери и капот тоже НЕ onchange: иначе открытая дверь пропадала бы с экрана
   по таймауту, а после переподключения HUD не знал бы текущее состояние.
   Для BAP — без onchange/interval (многокадровые сообщения).
   ========================================================================== */
#define ACL_PERMIT  0x01
#define ACL_EXT     0x02
#define ACL_ONCHG   0x04
#define ACL_ANYFMT  0x08

struct AclRule { uint8_t flags; uint16_t interval_ms; uint32_t id; uint32_t mask; };

static const AclRule ACL[] = {
    { ACL_PERMIT,           100, 0x30B,      0x7FF      },  /* Kombi_01: скорость      */
    { ACL_PERMIT,           200, 0x2A6,      0x7FF      },  /* ACC_12: заданная        */
    { ACL_PERMIT,           200, 0x2A8,      0x7FF      },  /* ACC_14: статус          */
    { ACL_PERMIT,           200, 0x31E,      0x7FF      },  /* лимитер (CRC+BZ)        */
    { ACL_PERMIT,           100, 0x2A9,      0x7FF      },  /* ACC_15: pre sense (AWV) */
    { ACL_PERMIT,           100, 0x5F0,      0x7FF      },  /* Dimmung_01: яркость (колёсико — каждые ~60 мс) */
    { ACL_PERMIT,           100, 0x30F,      0x7FF      },  /* SWA_01: Side Assist (CRC — без ONCHANGE) */
    /* 0x101 ESP_02 убран из списка: бар ускорения берёт производную скорости Kombi_01 */
    { ACL_PERMIT,           200, 0x5A0,      0x7FF      },  /* RLS_01: датчик света (яркость HUD)  */
    { ACL_PERMIT,           100, 0x64F,      0x7FF      },  /* BCM1_04: колёсико подсветки         */
    /* 0x106 ESP_05 и 0x107 Motor_04 убраны из списка: на I-CAN их нет (поиск 03.10),
       декодер их по-прежнему понимает — для своего трансивера на другой шине */
    { ACL_PERMIT,          1000, 0x6B2,      0x7FF      },  /* Diagnose_01: дата/время (журнал на SD) */
    { ACL_PERMIT,             0, 0x462,      0x7FE      },  /* PSD_04 + PSD_05: сегменты и позиция — без фильтров! */
    { ACL_PERMIT,             0, 0x464,      0x7FF      },  /* PSD_06: ограничения (мультиплекс) — без фильтров! */
    { ACL_PERMIT,           200, 0x394,      0x7FF      },  /* WBA_03: режим и № передачи */
    { ACL_PERMIT,             0, 0x366,      0x7FF      },  /* Blinkmodi_02            */
    { ACL_PERMIT,           500, 0x181,      0x7FF      },  /* VZE_01: знаки           */
    { ACL_PERMIT, LKA_MIN_INTERVAL_MS, 0x397, 0x7FF },     /* LDW_02: Lane Assist     */
    { ACL_PERMIT,          1000, 0x583,      0x7FF      },  /* ZV_02: двери, багажник  */
    { ACL_PERMIT,             0, 0x65A,      0x7FF      },  /* BCM_01: капот (1 раз/с) */
    { ACL_PERMIT | ACL_EXT,   0, 0x17333210, 0x1FFFFFFE },  /* BAP Nav FSG ..10/..11   */
    { ACL_PERMIT | ACL_EXT,   0, 0x17330F10, 0x1FFFFFFF },  /* BAP_BC: бортовой компьютер приборки */
};
#define ACL_COUNT (sizeof(ACL) / sizeof(ACL[0]))

static size_t build_acl(uint8_t *out)
{
    size_t o = 0;
    out[o++] = 0x01;                               /* версия */
    for (size_t i = 0; i < ACL_COUNT; i++) {
        const AclRule &r = ACL[i];
        out[o++] = r.flags;
        out[o++] = 0;
        out[o++] = r.interval_ms & 0xFF;
        out[o++] = r.interval_ms >> 8;
        for (int b = 0; b < 4; b++) out[o++] = (r.id   >> (8 * b)) & 0xFF;
        for (int b = 0; b < 4; b++) out[o++] = (r.mask >> (8 * b)) & 0xFF;
    }
    return o;
}

/* ========================================================================== */
#if HUD_DATA_SOURCE == HUD_SRC_BLE || HUD_DATA_SOURCE == HUD_SRC_AUTO

#include <NimBLEDevice.h>

static const NimBLEUUID UUID_SVC   ("A1B2C3D4-0001-41A2-9E3B-000000000001");
static const NimBLEUUID UUID_ACL   ("A1B2C3D4-0001-41A2-9E3B-000000000007");
static const NimBLEUUID UUID_FRAMES("A1B2C3D4-0001-41A2-9E3B-000000000008");

/* Разбор пачки: [count][flags][ts:2][idf:4][dlc:1][data]... */
static void on_frames(NimBLERemoteCharacteristic *, uint8_t *p, size_t len, bool)
{
    if (len < 2) return;
#if HUD_DATA_SOURCE == HUD_SRC_AUTO
    if (hud_src_twai_active()) return;                 /* AUTO: идёт CAN — BLE не используем */
#endif
    uint8_t count = p[0];
    if (p[1] & 0x01) hud_data_note_lost();
    size_t off = 2;
    for (uint8_t i = 0; i < count && off + 7 <= len; i++) {
        uint32_t idf;
        memcpy(&idf, p + off + 2, 4);
        uint8_t dlc = p[off + 6];
        if (dlc > 8 || off + 7 + dlc > len) break;
        if (!(idf & 0x40000000u))                                  /* не RTR */
            can_decode_frame(idf & 0x1FFFFFFFu, idf & 0x80000000u, p + off + 7, dlc);
        off += 7 + dlc;
    }
}

static void serial_line(const char *s) { hud_log_write(s); }

static bool session_setup(NimBLEClient *c)
{
    NimBLERemoteService *svc = c->getService(UUID_SVC);
    if (!svc) { if (HUD_LOG_BLE) hud_log_write("[BLE] сервис сниффера не найден\n"); return false; }

    NimBLERemoteCharacteristic *frames = svc->getCharacteristic(UUID_FRAMES);
    NimBLERemoteCharacteristic *acl    = svc->getCharacteristic(UUID_ACL);
    if (!frames || !acl) {
        if (HUD_LOG_BLE) hud_log_write("[BLE] нет ...0007/...0008 — прошивка сниффера ниже 2.6.0?\n");
        return false;
    }
    if (!frames->subscribe(true, on_frames, true)) {
        if (HUD_LOG_BLE) hud_log_write("[BLE] подписка на ...0008 не удалась\n");
        return false;
    }
    uint8_t buf[1 + 12 * 32];
    size_t n = build_acl(buf);
    if (!acl->writeValue(buf, n, true)) {
        if (HUD_LOG_BLE) hud_log_write("[BLE] запись ACL не удалась\n");
        return false;
    }
    if (HUD_LOG_BLE) arduino_printf("[BLE] ACL записан: %u правил, %u байт, MTU %u\n",
                  (unsigned)ACL_COUNT, (unsigned)n, (unsigned)c->getMTU());
    /* контроль: читаем список обратно и сравниваем */
    NimBLEAttValue back = acl->readValue();
    bool same = back.size() == n && memcmp(back.data(), buf, n) == 0;
    if (HUD_LOG_BLE) {
        if (!same)
            arduino_printf("[BLE] ВНИМАНИЕ: ACL прочитан обратно %u байт и не совпадает с записанным!\n",
                          (unsigned)back.size());
        else
            hud_log_write("[BLE] ACL прочитан обратно — совпадает\n");
    }
    return true;
}

#if BLE_PASSKEY
/* ---------------------------------------------------------------------------
   Сопряжение по коду доступа (LE Secure Connections, MITM, bonding).
   Устройство «показывает» код, мы «вводим» — подставляем BLE_PASSKEY.
   Порядок: connect -> secureConnection() -> проверка isEncrypted &&
   isAuthenticated -> только потом подписка и запись ACL.
   --------------------------------------------------------------------------- */
class HudPairCb : public NimBLEClientCallbacks {
    void onPassKeyEntry(NimBLEConnInfo &ci) override { NimBLEDevice::injectPassKey(ci, BLE_PASSKEY); }
};
static HudPairCb s_pair_cb;

static bool secure_channel(NimBLEClient *c)
{
    if (!c->secureConnection()) {
        if (HUD_LOG_BLE) hud_log_write("[BLE] сопряжение не удалось (код? слот занят? см. BLE_pairing_protocol)\n");
        return false;
    }
    NimBLEConnInfo ci = c->getConnInfo();
    if (!ci.isEncrypted() || !ci.isAuthenticated()) {      /* без кода (Just Works) — не доверяем */
        if (HUD_LOG_BLE) hud_log_write("[BLE] канал не аутентифицирован — отключаюсь\n");
        return false;
    }
    if (HUD_LOG_BLE) hud_log_write("[BLE] канал зашифрован, сопряжение пройдено\n");
    return true;
}
#endif

static void ble_task(void *)
{
    NimBLEDevice::init("HUD-3.49");
#if BLE_PASSKEY
    NimBLEDevice::setSecurityAuth(true, true, true);              /* bonding, MITM, Secure Connections */
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);      /* «ввожу код» */
#endif
    NimBLEDevice::setMTU(247);
    NimBLEClient *client = NimBLEDevice::createClient();
    client->setConnectTimeout(5000);
#if BLE_PASSKEY
    client->setClientCallbacks(&s_pair_cb, false);
    uint8_t  pair_fail = 0;                    /* неудач сопряжения подряд */
    uint32_t fail_t[3] = {0, 0, 0};            /* времена последних трёх неудач (не чаще 3 в минуту) */
    uint8_t  fail_i = 0;
#endif

    uint32_t last_stat = 0;

    for (;;) {
#if HUD_DATA_SOURCE == HUD_SRC_AUTO
        /* AUTO: пока идёт CAN со своего трансивера — BLE не нужен */
        if (hud_src_twai_active()) {
            if (client->isConnected()) {
                if (HUD_LOG_BLE) hud_log_write("[BLE] идёт CAN со своего трансивера — отключаюсь от сниффера\n");
                client->disconnect();
                hud_src_set_up(HUD_SRC_ID_BLE, false);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
#endif
        if (client->isConnected()) {
            if (millis() - last_stat > 10000) {
                if (HUD_LOG_STAT) {
                    HudData d; hud_data_snapshot(&d);
                    arduino_printf("[BLE] кадров %lu, пачек с потерями %lu, макс. пауза 0x30B %lu мс\n",
                                  (unsigned long)d.frames_rx, (unsigned long)d.batches_lost,
                                  (unsigned long)hud_data_take_speed_gap_max());
                }
                if (HUD_LOG_CAN) hud_data_debug_print(serial_line);
                last_stat = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        hud_src_set_up(HUD_SRC_ID_BLE, false);

        NimBLEScan *scan = NimBLEDevice::getScan();
        scan->setActiveScan(true);
        NimBLEScanResults res = scan->getResults(3000, false);

        const NimBLEAdvertisedDevice *dev = nullptr;
        for (int i = 0; i < res.getCount(); i++) {
            const NimBLEAdvertisedDevice *d = res.getDevice(i);
            if (d && d->getName() == HUD_SNIFFER_NAME) { dev = d; break; }
        }
        if (!dev) { scan->clearResults(); continue; }

        if (HUD_LOG_BLE) arduino_printf("[BLE] найден %s, подключаюсь\n", dev->getAddress().toString().c_str());
#if BLE_PASSKEY
        NimBLEAddress dev_addr = dev->getAddress();                  /* копия: dev исчезнет после clearResults() */
#endif
        bool ok = client->connect(dev);
        scan->clearResults();
        if (!ok) { if (HUD_LOG_BLE) hud_log_write("[BLE] connect не удался\n"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

#if BLE_PASSKEY
        if (!secure_channel(client)) {
            client->disconnect();
            pair_fail++;
            uint32_t now_ms = millis();
            fail_t[fail_i] = now_ms ? now_ms : 1; fail_i = (fail_i + 1) % 3;
            if (pair_fail == 2) NimBLEDevice::deleteBond(dev_addr);  /* привязка могла устареть (устройство сбросили) */
            if (pair_fail >= HUD_BLE_PAIR_MSG_AFTER) hud_src_set_pair_failed(true);
            /* паузы 2, 4, 6, 8, 10 с; не чаще трёх попыток в минуту (после 5 неудач устройство блокирует на 60 с) */
            uint32_t pause = pair_fail >= 5 ? 10000u : 2000u * pair_fail;
            uint32_t oldest = fail_t[fail_i];                       /* самая старая из трёх */
            if (oldest && now_ms - oldest < 60000u && 60000u - (now_ms - oldest) > pause)
                pause = 60000u - (now_ms - oldest);
            vTaskDelay(pdMS_TO_TICKS(pause));
            continue;
        }
        pair_fail = 0;
        hud_src_set_pair_failed(false);
#endif
        if (!session_setup(client)) {
            client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        hud_src_set_up(HUD_SRC_ID_BLE, true);
        last_stat = millis();
    }
}

void ble_source_start(void)
{
    xTaskCreatePinnedToCore(ble_task, "ble_can", 6144, nullptr, 3, nullptr, 1);
}

/* ========================================================================== */
#endif  /* BLE */

#if HUD_DATA_SOURCE == HUD_SRC_FAKE

static void put_sig(uint8_t *d, int start, int len, uint32_t v)
{
    uint64_t raw = 0;
    for (int i = 7; i >= 0; i--) raw = (raw << 8) | d[i];
    uint64_t m = ((1ULL << len) - 1) << start;
    raw = (raw & ~m) | (((uint64_t)v << start) & m);
    for (int i = 0; i < 8; i++) d[i] = (raw >> (8 * i)) & 0xFF;
}

static void bap_send_single(uint8_t fct, const uint8_t *p, uint8_t n)   /* n <= 6 */
{
    uint8_t f[8] = { 0x4C, (uint8_t)(0x80 | fct) };   /* opcode 4, LSG 0x32 */
    memcpy(f + 2, p, n);
    can_decode_frame(0x17333210, true, f, 2 + n);
}

static void bap_send_multi(uint8_t ch, uint8_t fct, const uint8_t *p, uint8_t n) /* n <= 255 */
{
    uint8_t f[8] = { (uint8_t)(0x80 | (ch << 4)), n, 0x4C, (uint8_t)(0x80 | fct) };
    uint8_t c = n < 4 ? n : 4;
    memcpy(f + 4, p, c);
    can_decode_frame(0x17333210, true, f, 4 + c);
    uint8_t seq = 0;
    while (c < n) {
        uint8_t k = (n - c) < 7 ? (n - c) : 7;
        f[0] = 0xC0 | (ch << 4) | (seq++ & 0x0F);
        memcpy(f + 1, p + c, k);
        can_decode_frame(0x17333210, true, f, 1 + k);
        c += k;
    }
}

static void fake_task(void *)
{
    uint32_t t0 = millis(), last_nav = 0;
    for (;;) {
        uint32_t t = millis() - t0;
        uint8_t d[8];

        /* скорость: пила 0..160 км/ч за 32 с (KBI_angez_Geschw — с шагом 0.32 км/ч) */
        uint32_t v_kmh = (t / 200) % 161;
        memset(d, 0, 8); put_sig(d, 24, 9, v_kmh); put_sig(d, 48, 10, (t % 32200) / 64);   /* та же пила, 0.32 км/ч */
        can_decode_frame(0x30B, false, d, 8);

        /* счётчик топлива: 6 л/100 км + 2 л/ч на холостых; > 20 л/100 км на разгоне с места */
        {
            static uint32_t kvs_ul = 0; static float frac = 0;
            float lph = 2.0f + 6.0f * v_kmh / 100.0f + (v_kmh < 20 ? 6.0f : 0.0f);
            frac += lph * 1e6f / 3600.0f / 20.0f;                /* мкл за 50 мс */
            kvs_ul += (uint32_t)frac; frac -= (uint32_t)frac;
            memset(d, 0, 8); put_sig(d, 48, 15, kvs_ul & 0x7FFF);
            can_decode_frame(0x107, false, d, 8);
        }

        /* ACC: 120 км/ч (raw 375), статус по кругу: активен / пассив / выкл */
        /* ACC_Tempolimit по кругу раз в 6 с: 60, 120, конец ограничения, нет */
        static const uint8_t tl_seq[4] = { 13, 23, 31, 0 };
        /* цикл 60 с: 0-40 ACC (20-30 с — режим пробки), 40-50 лимитер 80 км/ч, 50-60 лимитер без скорости */
        uint32_t c60 = t % 60000;
        bool lim = c60 >= 40000;
        memset(d, 0, 8); put_sig(d, 12, 10, lim ? 1023 : 375); put_sig(d, 0, 5, tl_seq[(t / 6000) % 4]);
        put_sig(d, 45, 2, (t / 3000) % 2);                              /* машина впереди есть/нет */
        put_sig(d, 37, 3, 3);
        put_sig(d, 62, 2, (c60 >= 20000 && c60 < 30000) ? 2 : 0);      /* STA активен */
        can_decode_frame(0x2A6, false, d, 8);
        memset(d, 0, 8); put_sig(d, 12, 10, !lim ? 1022 : (c60 < 50000 ? 250 : 1023));
        can_decode_frame(0x31E, false, d, 8);

        /* VZE: неизвестный код 0x5A (-> берётся Tempolimit), превышение, если скорость > 100 */
        memset(d, 0, 8); put_sig(d, 11, 8, 0x5A); put_sig(d, 35, 1, ((t / 200) % 161) > 100);
        can_decode_frame(0x181, false, d, 8);

        /* LKA: зелёный / жёлтый / предупреждение справа, по 5 с */
        uint32_t lp = (t / 5000) % 3;
        memset(d, 0, 8); d[1] = 0x40;
        if (lp == 0) put_sig(d, 62, 1, 1);
        if (lp == 1) put_sig(d, 61, 1, 1);
        if (lp == 2) { put_sig(d, 62, 1, 1); put_sig(d, 57, 1, 1); }
        put_sig(d, 38, 2, 2); put_sig(d, 36, 2, 2);
        can_decode_frame(0x397, false, d, 8);
        static const uint8_t acc_seq[3] = { 3, 2, 0 };
        memset(d, 0, 8); put_sig(d, 16, 3, lim ? 0 : acc_seq[(t / 8000) % 3]);
        can_decode_frame(0x2A8, false, d, 8);

        /* поворотники: 0-10 с левый, 10-20 правый, 20-30 аварийка, 30-40 выкл */
        uint32_t ph = (t / 10000) % 4;
        bool takt = (t % 1000) < 500;
        memset(d, 0, 8);
        if (ph == 0 && takt) put_sig(d, 27, 1, 1);
        if (ph == 1 && takt) put_sig(d, 28, 1, 1);
        if (ph == 2) { put_sig(d, 20, 1, 1); if (takt) { put_sig(d, 27, 1, 1); put_sig(d, 28, 1, 1); } }
        can_decode_frame(0x366, false, d, 8);

        /* двери: 40-60 с цикла по очереди FL, FR, RL, RR, капот, багажник, все */
        {
            static const uint8_t dseq[7] = { DOOR_FL, DOOR_FR, DOOR_RL, DOOR_RR, DOOR_HOOD, DOOR_TRUNK, 0x3F };
            uint32_t c = t % 60000;
            hud_data_fake_doors(c >= 40000 ? dseq[((c - 40000) / 3000) % 7] : 0);
        }

        /* pre sense: 30-32 с — предупреждение, 32-34 с — острое */
        memset(d, 0, 8);
        { uint32_t c = t % 60000; put_sig(d, 16, 3, (c >= 30000 && c < 32000) ? 2 : (c >= 32000 && c < 34000) ? 3 : 0); }
        can_decode_frame(0x2A9, false, d, 8);

        /* яркость: «уровень света» по кругу за 60 с: темно (колёсико 10..100),
           день (до 253), солнце (FW в потолке, Boost 15 -> 0) */
        memset(d, 0, 8);
        { uint32_t c = t % 60000; uint32_t xd = c < 20000 ? 10 + c * 90 / 20000 : c < 40000 ? 100 + (c - 20000) * 153 / 20000 : 253;
          put_sig(d, 0, 8, xd); put_sig(d, 15, 1, xd <= 100);
          uint8_t r[8] = {0}; uint32_t fw = xd <= 100 ? 0 : (xd - 99) * 1021 / 154; uint32_t boost = c < 40000 ? 15 : 15 - (c - 40000) * 15 / 20000;
          put_sig(r, 8, 10, fw > 1021 ? 1021 : fw); put_sig(r, 35, 4, boost);
          can_decode_frame(0x5A0, false, r, 8);
          uint8_t w[8] = {0}; put_sig(w, 25, 7, 1 + (t / 100) % 100);          /* колёсико 1..100 за 10 с */
          can_decode_frame(0x64F, false, w, 8); }
        can_decode_frame(0x5F0, false, d, 8);

        /* BAP_BC «с момента запуска» раз в 4 с: 18 байт, многокадровое, LSG 0x0F */
        {
            static uint32_t last_bc = 0;
            if (t - last_bc >= 4000) {
                last_bc = t;
                uint8_t bc[18] = {0};
                uint16_t avg = 75 + (t / 4000) % 20;                    /* 7.5..9.4 л/100 */
                uint16_t dist = (uint16_t)(t / 1000);                   /* 0.1 км в секунду */
                bc[0] = avg & 0xFF; bc[1] = avg >> 8; bc[6] = dist & 0xFF; bc[7] = dist >> 8;
                bc[11] = (uint8_t)(t / 60000); bc[15] = 0x68; bc[16] = 0x01;   /* 36.0 км/ч */
                uint8_t f[8] = { 0x80, 18, 0x43, (uint8_t)(0xC0 | 0x18), bc[0], bc[1], bc[2], bc[3] };
                can_decode_frame(0x17330F10, true, f, 8);
                for (uint8_t seq = 0, c = 4; c < 18; seq++) {
                    uint8_t k = (18 - c) < 7 ? (18 - c) : 7;
                    f[0] = 0xC0 | (seq & 0x0F); memcpy(f + 1, bc + c, k);
                    can_decode_frame(0x17330F10, true, f, 1 + k);
                    c += k;
                }
                uint8_t fl[3] = { 0x43, (uint8_t)(0xC0 | 0x1C), (uint8_t)(84 - (t / 20000) % 80) };   /* уровень топлива, % */
                can_decode_frame(0x17330F10, true, fl, 3);
            }
        }

        /* Side Assist: слева машина 10-20 с цикла 30 с, на 15-17 с — попытка перестроения */
        {
            uint32_t c = t % 30000;
            memset(d, 0, 8);
            put_sig(d, 26, 1, c >= 10000 && c < 20000); put_sig(d, 27, 1, c >= 15000 && c < 17000);
            can_decode_frame(0x30F, false, d, 8);
        }

        /* часы: 1 октября 2026, 14:05:00 + время работы генератора */
        {
            uint32_t sec = 14 * 3600 + 5 * 60 + t / 1000;
            memset(d, 0, 8);
            put_sig(d, 28, 7, 26); put_sig(d, 35, 4, 10); put_sig(d, 39, 5, 1);
            put_sig(d, 44, 5, (sec / 3600) % 24); put_sig(d, 49, 6, (sec / 60) % 60); put_sig(d, 55, 6, sec % 60);
            can_decode_frame(0x6B2, false, d, 8);
        }

        /* PSD: сегмент 10 (загородная дорога), явный знак 70 с 0-й до 40-й с цикла,
           с 20-й по 40-ю — ещё и запрет обгона; потом конец ограничения -> 90 по правилам */
        {
            uint32_t c = t % 60000;
            memset(d, 0, 8); put_sig(d, 0, 6, 10); put_sig(d, 6, 6, 9); put_sig(d, 12, 7, 120);
            put_sig(d, 19, 3, 3); put_sig(d, 38, 1, 1);
            can_decode_frame(0x462, false, d, 8);
            memset(d, 0, 8); put_sig(d, 0, 3, 2); put_sig(d, 3, 6, 10); put_sig(d, 16, 5, c < 40000 ? 13 : 23);
            put_sig(d, 21, 2, 1); put_sig(d, 39, 5, 25); put_sig(d, 44, 5, 25);
            put_sig(d, 49, 2, (c >= 20000 && c < 40000) ? 1 : 0);
            can_decode_frame(0x464, false, d, 8);
            memset(d, 0, 8); put_sig(d, 0, 6, 10); put_sig(d, 6, 7, 50);
            can_decode_frame(0x463, false, d, 8);
        }

        /* передача: P R N D S M по 4 с */
        static const uint8_t gseq[6] = { 1, 2, 3, 4, 5, 6 };
        memset(d, 0, 8); put_sig(d, 12, 4, gseq[(t / 4000) % 6]);
        put_sig(d, 24, 4, 1 + (t / 1000) % 7);                          /* передачи 1..7 */
        can_decode_frame(0x394, false, d, 8);

        /* навигация раз в секунду */
        if (t - last_nav >= 1000) {
            last_nav = t;
            uint8_t rg = 1;
            bap_send_single(0x11, &rg, 1);

            uint32_t md = 5000 - ((t / 100) % 5000);          /* 500.0 -> 0 м */
            uint8_t m[8] = { (uint8_t)md, (uint8_t)(md >> 8), (uint8_t)(md >> 16), (uint8_t)(md >> 24),
                             0, 1, (uint8_t)(md * 100 / 5000), 1 };
            bap_send_multi(0, 0x12, m, 8);

            uint32_t dd = 235;                                 /* 23.5 км */
            uint8_t ds[6] = { (uint8_t)dd, (uint8_t)(dd >> 8), 0, 0, 1, 0x01 };
            bap_send_single(0x15, ds, 6);

            uint8_t tt[7] = { 0x00, 34, 0, 0, 0, 0, 0x06 };   /* 0:34 в пути */
            bap_send_multi(1, 0x16, tt, 7);

            /* манёвры по кругу, по 3 с: все повороты, кольцо, съезды, развилки, разворот, финиш */
            static const uint8_t man_seq[][2] = {
                {0x0B,0x00},{0x0D,0x20},{0x0D,0x40},{0x0D,0x60},{0x0D,0xA0},{0x0D,0xC0},{0x0D,0xE0},
                {0x15,0x00},{0x15,0x40},{0x15,0xC0},{0x15,0x80},{0x0F,0x00},{0x10,0x00},
                {0x13,0x30},{0x14,0xD0},{0x19,0x40},{0x03,0x00},{0x09,0x00},{0x2A,0x50} };
            const uint8_t *mm = man_seq[(t / 3000) % (sizeof(man_seq) / 2)];
            /* как у MIB: 3 манёвра по 4 байта, второй — следующий по списку */
            const uint8_t *m2 = man_seq[((t / 3000) + 1) % (sizeof(man_seq) / 2)];
            uint8_t mn[12] = { mm[0], mm[1], 0, 0,  m2[0], m2[1], 0, 0,  0, 0, 0, 0 };
            bap_send_multi(2, 0x17, mn, 12);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void fake_source_start(void)
{
    uint8_t buf[1 + 12 * 32];
    size_t n = build_acl(buf);
    arduino_printf("[FAKE] генератор кадров вместо BLE (ACL был бы %u байт)\n", (unsigned)n);
    hud_src_set_up(HUD_SRC_ID_FAKE, true);
    xTaskCreatePinnedToCore(fake_task, "fake_can", 4096, nullptr, 3, nullptr, 1);
}

#endif


#endif  /* HUD_SRC_BLE || HUD_SRC_FAKE || HUD_SRC_AUTO */
