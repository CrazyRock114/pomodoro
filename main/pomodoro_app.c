// main/pomodoro_app.c —— 三键番茄钟应用:自有 UI + 按键分发 + 蜂鸣/深睡 worker。
//
// 衍生应用(见 docs/development/ai-guide.md 强制 UI 重设计规则):不复用 demo
// 菜单壳与 ui_pixel 视觉,页面与交互按番茄钟重新设计——深色底 + 圆弧进度环 +
// 大字号剩余时间。生命周期与并发模式参考 demo_audio.c(worker 握手)与
// demo_low_power.c(深睡前外设关断顺序)。
//
// 并发模型:
//   - s_core / s_last_activity_ms 只在 portMUX 临界区内读写(C3 单核,仅防
//     64 位字段撕裂与指令级竞态);临界区内不做 LVGL/日志/总线操作。
//   - 按键回调(esp_timer 任务)只入队;input task 处理按键并在持 LVGL 锁时
//     刷 UI;LVGL tick 定时器在 LVGL 任务上下文运行,免锁直接刷 UI。
//   - 蜂鸣/深睡是阻塞操作,全部在各自 worker task 里执行。
#include "pomodoro_app.h"
#include "pomodoro_core.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"     // esp_clk_rtc_time:RTC 计数,深睡期间持续走
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>

LV_FONT_DECLARE(pomo_cjk_16);   // assets/fonts 生成的中文子集,见 assets/README.md
LV_FONT_DECLARE(pomo_cjk_48);   // 录音"开始"提示用 48px 小字库

static const char *TAG = "pomodoro";

// ===== 参数 =====
#define POMO_TICK_MS           250
#define POMO_BATT_PERIOD_TICKS 20                  // 电量 5s 刷一次
#define WORKER_STOP_TIMEOUT_MS 2000

// bsp_pins.h:三键共用 ADC1_CH0 = GPIO0(10k 上拉,按下拉到 0~1.9V)。
// C3 的 strapping 脚是 GPIO2/8/9,GPIO0 可安全作低电平唤醒源;
// C3 无 ext0(SOC_PM_SUPPORT_EXT0_WAKEUP=0),深睡 GPIO 唤醒走
// esp_deep_sleep_enable_gpio_wakeup(),唤醒原因为 ESP_SLEEP_WAKEUP_GPIO。
#define WAKE_GPIO_NUM   GPIO_NUM_0

#define BEEP_SAMPLE_HZ    16000
#define BEEP_TONE_HZ       1000
#define BEEP_ON_MS          150
#define BEEP_GAP_MS         100
#define BEEP_COUNT            3
#define BEEP_CHUNK_SAMPLES  512
#define BEEP_AMPLITUDE      6000

#define VOICE_SAMPLE_HZ    16000      // 录音 16kHz/16bit 单声道(BSP 已验证速率),2 秒 = 64KB
#define VOICE_SECONDS          2
#define VOICE_BUF_BYTES (VOICE_SAMPLE_HZ * VOICE_SECONDS * 2)
#define FLASH_TOGGLE_MS       300     // 白屏闪烁半周期
#define HOLD_START_MS         500     // 与 BSP 长按门限一致

// 自有视觉(区别于 demo 的天空蓝像素风):深海军蓝底 + 番茄红进度。
#define COL_BG       0x0D1B2A
#define COL_TRACK    0x24405C
#define COL_ACCENT   0xE8503F
#define COL_TEXT     0xF2F5F7
#define COL_MUTED    0x6C8BA3
#define COL_RUNNING  0x3DDC97
#define COL_PAUSED   0xFFB23E
#define COL_LOW_BAT  0xE43B2F

typedef enum {
    AUDIO_CMD_BEEP = 1,      // 三短声
    AUDIO_CMD_STOP,
    AUDIO_CMD_BEEP_LOOP,     // 持续蜂鸣直到按键
    AUDIO_CMD_VOICE_REC,     // 录 3s 提示语音并试放一遍
    AUDIO_CMD_VOICE_LOOP,    // 循环播放已录语音+闪屏直到按键(缺失回退持续蜂鸣)
} audio_cmd_t;

typedef enum {
    SLEEP_CMD_SLEEP = 1,
} sleep_cmd_t;

// 深睡保留区:SRAM 深睡即失,现场(状态/时长/剩余/RTC 计数)只放 RTC memory。
#define SLEEP_STATE_MAGIC   0x504F4D4FUL   // "POMO"
#define SLEEP_STATE_VERSION 1UL
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t duration_s;
    uint32_t elapsed_s;
    uint64_t rtc_us_at_sleep;
} sleep_state_t;
static RTC_DATA_ATTR sleep_state_t s_sleep_state;

// ===== 应用状态 =====
static pomodoro_core_t s_core;                       // 仅在临界区内访问
static portMUX_TYPE s_core_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_last_activity_ms;                  // 仅在临界区内访问

static volatile bool s_sleeping;                     // 深睡流程启动,输入/tick 停工
static uint64_t s_input_grace_until_ms;              // 唤醒宽限窗,start 时一次性写入

// ===== 设置/提醒/快调状态 =====
static pomodoro_settings_t s_settings;               // s_core_mux 保护;input 写,audio/tick 读
static volatile bool s_beep_cancel;                  // 持续蜂鸣取消标志
static uint8_t s_voice_pcm[VOICE_BUF_BYTES];         // 录音缓冲(.bss;深睡即失,唤醒后回退蜂鸣)
static bool s_voice_ready;
static volatile bool s_alert_active;                 // FINISHED 提醒进行中(tick 驱动)
static uint32_t s_alert_flash_left;                  // 白屏闪烁剩余半周期
static uint64_t s_alert_next_ms;
static uint64_t s_alert_started_ms;
static int s_hold_btn = -1;                          // SETUP 中按住的 UP/DOWN(-1 无)
static uint64_t s_hold_press_ms;

// ===== 设置页 =====
static lv_obj_t *s_set_scr, *s_set_rows[POMODORO_SET_ITEM_COUNT], *s_set_hint;
static lv_obj_t *s_rec_big;                          // 录音倒计时大数字(设置页)
static int s_set_selected;
static bool s_page_settings;                         // false=主页面
static bool s_sleep_notified;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_audio_stopped;
static TaskHandle_t s_sleep_task;
static lv_timer_t *s_tick_timer;

// UI 对象(LVGL 任务或持锁上下文访问)
static lv_obj_t *s_scr, *s_title, *s_battery, *s_arc;
static lv_obj_t *s_number, *s_caption, *s_hint;
static lv_obj_t *s_flash;                            // 白屏闪烁遮罩(主屏)
static pomodoro_state_t s_ui_state = POMODORO_STATE_SETUP;
static uint32_t s_ui_remaining = UINT32_MAX;
static int s_ui_battery = -2;                        // -2 = 尚未读过

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static void build_settings_screen(void);
static const char *alert_name(uint8_t alert);

// ===== 设置持久化(NVS;结构校验不过则回退默认) =====

static void settings_load(void) {
    pomodoro_settings_defaults(&s_settings);
    nvs_handle_t handle;
    if (nvs_open("pomo", NVS_READONLY, &handle) != ESP_OK) return;
    pomodoro_settings_t stored = s_settings;
    uint8_t value;
    if (nvs_get_u8(handle, "step", &value) == ESP_OK) stored.step_min = value;
    if (nvs_get_u8(handle, "alert", &value) == ESP_OK) stored.alert = value;
    if (nvs_get_u8(handle, "vol", &value) == ESP_OK) stored.volume = value;
    if (nvs_get_u8(handle, "slp", &value) == ESP_OK) stored.sleep_min = value;
    if (pomodoro_settings_load_valid(&stored)) s_settings = stored;
    nvs_close(handle);
}

static void settings_save(void) {
    nvs_handle_t handle;
    if (nvs_open("pomo", NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u8(handle, "step", s_settings.step_min);
    (void)nvs_set_u8(handle, "alert", s_settings.alert);
    (void)nvs_set_u8(handle, "vol", s_settings.volume);
    (void)nvs_set_u8(handle, "slp", s_settings.sleep_min);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

// ===== 蜂鸣 worker(模式取自 demo_audio.c:命令字 + 退出握手) =====

// 当前设置快照(临界区很短,worker 与 tick 共用)
static pomodoro_settings_t settings_snapshot(void) {
    portENTER_CRITICAL(&s_core_mux);
    const pomodoro_settings_t snapshot = s_settings;
    portEXIT_CRITICAL(&s_core_mux);
    return snapshot;
}

// worker 上下文的屏上状态提示(持锁短改标签;醒目亮色)
static void ui_status_set(const char *text) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_set_hint && s_page_settings) {
        lv_label_set_text(s_set_hint, text);
        lv_obj_set_style_text_color(s_set_hint, lv_color_hex(COL_TEXT), 0);
    } else if (s_hint) {
        lv_label_set_text(s_hint, text);
        lv_obj_set_style_text_color(s_hint, lv_color_hex(COL_TEXT), 0);
    }
    bsp_lvgl_unlock();
}

// worker 上下文的录音倒计时大数字(设置页中央,48px)
static void rec_big_show_num(int n) {
    if (!bsp_lvgl_lock(300)) return;
    if (s_rec_big) {
        lv_obj_set_style_text_font(s_rec_big, &lv_font_montserrat_48, 0);
        lv_label_set_text_fmt(s_rec_big, "%d", n);
        lv_obj_remove_flag(s_rec_big, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

static void rec_big_show_start(void) {
    if (!bsp_lvgl_lock(300)) return;
    if (s_rec_big) {
        lv_obj_set_style_text_font(s_rec_big, &pomo_cjk_48, 0);
        lv_label_set_text(s_rec_big, "开始");
        lv_obj_remove_flag(s_rec_big, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

static void rec_big_hide(void) {
    if (!bsp_lvgl_lock(300)) return;
    if (s_rec_big) lv_obj_add_flag(s_rec_big, LV_OBJ_FLAG_HIDDEN);
    bsp_lvgl_unlock();
}

static void play_tone_chunks(uint32_t hz, uint32_t volume, uint32_t total_ms,
                             bool until_cancel) {
    if (bsp_audio_set_format(hz, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "蜂鸣:采样格式设置失败");
        return;
    }
    bsp_audio_set_volume((uint8_t)volume);

    int16_t *buf = malloc(BEEP_CHUNK_SAMPLES * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "蜂鸣:临时缓冲分配失败");
        return;
    }

    const int on_samples = (int)(hz * BEEP_ON_MS / 1000);
    const int cycle = (int)(hz * (BEEP_ON_MS + BEEP_GAP_MS) / 1000);
    const int period = (int)(hz / BEEP_TONE_HZ);
    int total = (int)(hz * total_ms / 1000);

    int pos = 0;
    while (pos < total && !(until_cancel && s_beep_cancel)) {
        int n = total - pos < BEEP_CHUNK_SAMPLES ? total - pos : BEEP_CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            const int local = (pos + i) % cycle;
            if (local < on_samples) {
                const int phase = local % period;
                buf[i] = (int16_t)(phase < period / 2 ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE);
            } else {
                buf[i] = 0;
            }
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "蜂鸣:音频写出失败(%d/%d)", pos, total);
            break;
        }
        pos += n;
    }
    free(buf);
}

// 三短声或持续蜂鸣;持续模式下每轮之间检查取消标志
static void play_beeps(uint32_t volume, bool until_cancel) {
    do {
        play_tone_chunks(BEEP_SAMPLE_HZ, volume,
                         BEEP_COUNT * (BEEP_ON_MS + BEEP_GAP_MS), until_cancel);
        if (until_cancel && !s_beep_cancel) vTaskDelay(pdMS_TO_TICKS(400));
    } while (until_cancel && !s_beep_cancel);
}

// 循环播放已录语音直到取消;录音缺失(如深睡唤醒后缓冲已失)回退持续蜂鸣
static void play_voice_alert(const pomodoro_settings_t *st) {
    if (!s_voice_ready) {
        play_beeps(st->volume, true);
        return;
    }
    if (bsp_audio_set_format(VOICE_SAMPLE_HZ, 16, 1) != ESP_OK) {
        play_beeps(st->volume, true);
        return;
    }
    bsp_audio_set_volume((uint8_t)st->volume);
    while (!s_beep_cancel) {
        for (size_t off = 0; off < VOICE_BUF_BYTES && !s_beep_cancel;
             off += BEEP_CHUNK_SAMPLES * 2) {
            size_t n = VOICE_BUF_BYTES - off;
            if (n > BEEP_CHUNK_SAMPLES * 2) n = BEEP_CHUNK_SAMPLES * 2;
            if (bsp_audio_write(s_voice_pcm + off, n) != ESP_OK) return;
        }
        if (!s_beep_cancel) vTaskDelay(pdMS_TO_TICKS(400));
    }
}

static void record_voice(void) {
    if (bsp_audio_set_format(VOICE_SAMPLE_HZ, 16, 1) != ESP_OK) {
        ui_status_set("录音失败:格式");
        return;
    }
    bsp_audio_set_volume(settings_snapshot().volume);   // 试听音量跟设置走
    // 3 秒准备倒计时:纯视觉大数字(不发声)
    for (int n = 3; n > 0; n--) {
        rec_big_show_num(n);
        ui_status_set("准备录音...请对设备说话");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    rec_big_show_start();                 // "开始"提示,与准备倒计时的 3-2-1 区隔
    vTaskDelay(pdMS_TO_TICKS(600));
    ui_status_set("录音中 请说话");
    size_t got = 0;
    int last_sec = -1;
    while (got < VOICE_BUF_BYTES) {
        size_t n = VOICE_BUF_BYTES - got;
        if (n > BEEP_CHUNK_SAMPLES * 2) n = BEEP_CHUNK_SAMPLES * 2;
        if (bsp_audio_read(s_voice_pcm + got, n) != ESP_OK) {
            rec_big_hide();
            ui_status_set("录音失败");
            return;
        }
        got += n;
        const int sec = VOICE_SECONDS - (int)(got / (VOICE_SAMPLE_HZ * 2));
        if (sec != last_sec) {
            last_sec = sec;
            rec_big_show_num(sec);   // 剩余秒数大数字
        }
    }
    // 按峰值归一放大(目标 25000,上限 8 倍,饱和保护):麦克风电平低,不放大则声音偏轻
    int16_t *samples = (int16_t *)s_voice_pcm;
    int16_t peak = 0;
    for (size_t i = 0; i < VOICE_BUF_BYTES / 2; i++) {
        const int16_t v = samples[i] < 0 ? (int16_t)-samples[i] : samples[i];
        if (v > peak) peak = v;
    }
    uint32_t gain_q8 = peak > 0 ? (25000u * 256u) / (uint32_t)peak : 256u;
    if (gain_q8 > 8u * 256u) gain_q8 = 8u * 256u;
    for (size_t i = 0; i < VOICE_BUF_BYTES / 2; i++) {
        int32_t v = ((int32_t)samples[i] * (int32_t)gain_q8) >> 8;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
    ESP_LOGI(TAG, "录音峰值 %d,归一增益 %.2fx", peak, (double)gain_q8 / 256.0);
    s_voice_ready = true;
    rec_big_hide();
    ui_status_set("录音完成,试听...");
    for (size_t off = 0; off < VOICE_BUF_BYTES; off += BEEP_CHUNK_SAMPLES * 2) {
        size_t n = VOICE_BUF_BYTES - off;
        if (n > BEEP_CHUNK_SAMPLES * 2) n = BEEP_CHUNK_SAMPLES * 2;
        if (bsp_audio_write(s_voice_pcm + off, n) != ESP_OK) {
            ui_status_set("试听失败");
            return;
        }
    }
    ui_status_set("已录好,长按OK返回");
}

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t command = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command == AUDIO_CMD_STOP) break;
        const pomodoro_settings_t settings = settings_snapshot();
        s_beep_cancel = false;
        switch (command) {
        case AUDIO_CMD_BEEP:       play_beeps(settings.volume, false); break;
        case AUDIO_CMD_BEEP_LOOP:  play_beeps(settings.volume, true); break;
        case AUDIO_CMD_VOICE_REC:  record_voice(); break;
        case AUDIO_CMD_VOICE_LOOP: play_voice_alert(&settings); break;
        default: break;
        }
    }
    // 生命周期 owner 持句柄直到收到该应答;此后不得再碰共享状态/UI。
    // 挂起而非退出,让超时的 stop 可安全重试;只有 owner 删除/替换本任务。
    xSemaphoreGive(s_audio_stopped);
    for (;;) vTaskSuspend(NULL);
}

static esp_err_t audio_worker_start(void) {
    if (s_audio_task) return ESP_OK;
    if (s_audio_stopped) {
        vSemaphoreDelete(s_audio_stopped);
        s_audio_stopped = NULL;
    }
    s_audio_stopped = xSemaphoreCreateBinary();
    if (!s_audio_stopped) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_task, "pomo_audio", 4096, NULL, 4, &s_audio_task) != pdPASS) {
        vSemaphoreDelete(s_audio_stopped);
        s_audio_stopped = NULL;
        s_audio_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t audio_worker_stop(void) {
    TaskHandle_t task = s_audio_task;
    if (!task) {
        if (s_audio_stopped) {
            vSemaphoreDelete(s_audio_stopped);
            s_audio_stopped = NULL;
        }
        return ESP_OK;
    }
    xTaskNotify(task, AUDIO_CMD_STOP, eSetValueWithOverwrite);
    if (!s_audio_stopped ||
        xSemaphoreTake(s_audio_stopped, pdMS_TO_TICKS(WORKER_STOP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelete(task);
    s_audio_task = NULL;
    vSemaphoreDelete(s_audio_stopped);
    s_audio_stopped = NULL;
    return ESP_OK;
}

// ===== UI(自有设计,不使用 ui_pixel 像素壳) =====

// 可从 LVGL 任务(tick,免锁)或 input task(持 bsp_lvgl_lock)调用。
static void apply_ui(void) {
    if (!s_scr) return;   // 建屏失败的保护:逻辑继续跑,不动 UI
    const pomodoro_settings_t settings = settings_snapshot();
    pomodoro_state_t state;
    uint32_t remaining;
    uint32_t permille;
    portENTER_CRITICAL(&s_core_mux);
    const uint64_t now = now_ms();
    state = s_core.state;
    remaining = pomodoro_core_remaining_s(&s_core, now);
    permille = pomodoro_core_progress_permille(&s_core, now);
    portEXIT_CRITICAL(&s_core_mux);

    char buf[8];
    if (state == POMODORO_STATE_SETUP) {
        lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
        unsigned minutes = remaining / 60u;   // 设置页上限 180,钳制供格式化
        if (minutes > 180u) minutes = 180u;
        snprintf(buf, sizeof(buf), "%u", minutes);
        lv_label_set_text(s_number, buf);
        // 大数字下方直接显示当前提醒模式
        lv_obj_set_style_text_font(s_caption, &pomo_cjk_16, 0);
        lv_label_set_text(s_caption, alert_name(settings.alert));
        lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_MUTED), 0);
        lv_label_set_text(s_hint, "UP/DOWN: 调时间  OK: 开始\n长按OK: 设置");
    } else {
        lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(s_arc, (int32_t)permille);
        pomodoro_format_mmss(remaining, buf, sizeof(buf));
        lv_label_set_text(s_number, buf);
        const char *first_line;
        switch (state) {
        case POMODORO_STATE_RUNNING:
            lv_label_set_text(s_caption, "RUNNING");
            lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_RUNNING), 0);
            first_line = "OK: 暂停  长按OK: 重置";
            break;
        case POMODORO_STATE_PAUSED:
            lv_label_set_text(s_caption, "PAUSED");
            lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_PAUSED), 0);
            first_line = "OK: 继续  长按OK: 重置";
            break;
        default:
            lv_label_set_text(s_caption, "DONE");
            lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_ACCENT), 0);
            first_line = "OK: 新一轮";
            break;
        }
        // 提示第二行常显当前提醒模式
        char hint[96];
        snprintf(hint, sizeof(hint), "%s\n提醒: %s",
                 first_line, alert_name(settings.alert));
        lv_label_set_text(s_hint, hint);
    }
    s_ui_state = state;
    s_ui_remaining = remaining;
}

static void update_battery_label(void) {
    if (!s_battery) return;   // 设置页时主屏对象已删
    const int soc = bsp_battery_soc();
    if (soc == s_ui_battery) return;
    s_ui_battery = soc;
    if (soc < 0) {
        lv_label_set_text(s_battery, "");            // 读失败:不画数字
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
        lv_obj_set_style_text_color(
            s_battery,
            lv_color_hex(soc < 20 ? COL_LOW_BAT : COL_MUTED), 0);
    }
}

// 持 bsp_lvgl_lock 调用。
static void build_screen(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);

    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(s_title, "POMODORO");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 12, 10);

    // 电量默认放右上角(仓库 UI 惯例);SOC<20% 变红。
    s_battery = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_battery, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);

    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, 212, 212);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, -8);
    lv_arc_set_rotation(s_arc, 270);                 // 12 点起整圈
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);  // 纯展示,无旋钮
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    s_number = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_number, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_number, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(s_number, LV_ALIGN_CENTER, 0, -26);

    s_caption = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_caption, LV_ALIGN_CENTER, 0, 32);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &pomo_cjk_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 提醒闪烁遮罩:静音模式用白屏闪烁代替声音
    s_flash = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_COVER, 0);
    lv_obj_set_size(s_flash, 240, 320);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_scr);
}

// ===== 提醒引擎(tick/LVGL 上下文驱动;worker 负责声音) =====

static void alert_stop(void) {
    s_alert_active = false;
    s_beep_cancel = true;   // 持续蜂鸣立即停
    if (bsp_lvgl_lock(200)) {
        if (s_flash) lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
        bsp_lvgl_unlock();
    }
}

// FINISHED 时刻(含深睡睡穿唤醒)启动一次提醒
static void alert_start(void) {
    const pomodoro_settings_t settings = settings_snapshot();
    s_beep_cancel = false;
    s_alert_flash_left = 0;
    s_alert_next_ms = 0;
    switch ((pomodoro_alert_t)settings.alert) {
    case POMODORO_ALERT_FLASH_5:
        s_alert_flash_left = 10;                    // 5 次亮灭
        s_alert_next_ms = now_ms();
        break;
    case POMODORO_ALERT_FLASH_LOOP:
        s_alert_flash_left = UINT32_MAX;
        s_alert_next_ms = now_ms();
        break;
    case POMODORO_ALERT_BEEP_SHORT:
        if (s_audio_task) xTaskNotify(s_audio_task, AUDIO_CMD_BEEP, eSetValueWithOverwrite);
        break;
    case POMODORO_ALERT_BEEP_LOOP:
        if (s_audio_task) xTaskNotify(s_audio_task, AUDIO_CMD_BEEP_LOOP, eSetValueWithOverwrite);
        break;
    case POMODORO_ALERT_VOICE:
        s_alert_flash_left = UINT32_MAX;            // 语音同时闪屏
        s_alert_next_ms = now_ms();
        if (s_audio_task) xTaskNotify(s_audio_task, AUDIO_CMD_VOICE_LOOP, eSetValueWithOverwrite);
        break;
    default:
        return;
    }
    s_alert_active = true;
}

// tick 调用(LVGL 任务上下文):白屏闪烁节拍
static void alert_tick(uint64_t now) {
    if (!s_alert_active || s_alert_flash_left == 0 ||
        now < s_alert_next_ms) return;
    s_alert_next_ms = now + FLASH_TOGGLE_MS;
    if (bsp_lvgl_lock(100)) {
        if (s_flash) {
            if (lv_obj_has_flag(s_flash, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
            }
        }
        bsp_lvgl_unlock();
    }
    if (s_alert_flash_left != UINT32_MAX && --s_alert_flash_left == 0) {
        s_alert_active = false;
        if (bsp_lvgl_lock(100)) {
            if (s_flash) lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
            bsp_lvgl_unlock();
        }
    }
}

// ===== 页面切换(输入任务持 LVGL 锁调用;旧屏必须删,防泄漏) =====

static void page_open_settings(void) {
    if (s_page_settings) return;
    if (s_scr) lv_obj_delete(s_scr);
    s_scr = NULL; s_hint = NULL; s_battery = NULL; s_flash = NULL;
    s_arc = NULL; s_number = NULL; s_caption = NULL;
    s_page_settings = true;
    build_settings_screen();
}

static void page_open_main(void) {
    if (!s_page_settings) return;
    if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; s_set_hint = NULL; s_rec_big = NULL; }
    s_page_settings = false;
    build_screen();
    apply_ui();
    update_battery_label();
}

// ===== 设置页(自有交互:UP/DOWN 选行,OK 换值,长按 OK 返回) =====

static const char *alert_name(uint8_t alert) {
    switch ((pomodoro_alert_t)alert) {
    case POMODORO_ALERT_FLASH_5:   return "静音 闪5次";
    case POMODORO_ALERT_FLASH_LOOP: return "静音 闪到按键";
    case POMODORO_ALERT_BEEP_SHORT: return "蜂鸣 短";
    case POMODORO_ALERT_BEEP_LOOP:  return "蜂鸣 持续";
    case POMODORO_ALERT_VOICE:      return "语音";
    default: return "?";
    }
}

// 持 LVGL 锁调用;刷新三行文字与选中高亮
static void settings_refresh_locked(void) {
    static const char *ITEMS[POMODORO_SET_ITEM_COUNT] =
        { "步幅", "提醒", "音量", "休眠" };
    char buf[48];
    for (int i = 0; i < POMODORO_SET_ITEM_COUNT; i++) {
        if (i == POMODORO_SET_ITEM_STEP) {
            snprintf(buf, sizeof(buf), "%s: %u 分钟", ITEMS[i],
                     (unsigned)s_settings.step_min);
        } else if (i == POMODORO_SET_ITEM_ALERT) {
            snprintf(buf, sizeof(buf), "%s: %s", ITEMS[i], alert_name(s_settings.alert));
        } else if (i == POMODORO_SET_ITEM_SLEEP) {
            if (s_settings.sleep_min == POMODORO_SLEEP_WHEN_DONE) {
                snprintf(buf, sizeof(buf), "%s: 倒计时结束", ITEMS[i]);
            } else {
                snprintf(buf, sizeof(buf), "%s: %u 分钟", ITEMS[i],
                         (unsigned)s_settings.sleep_min);
            }
        } else {
            snprintf(buf, sizeof(buf), "%s: %u", ITEMS[i],
                     (unsigned)s_settings.volume);
        }
        lv_label_set_text(s_set_rows[i], buf);
        lv_obj_set_style_text_color(s_set_rows[i],
            lv_color_hex(i == s_set_selected ? COL_TEXT : COL_MUTED), 0);
    }
}

// 持 LVGL 锁调用
static void build_settings_screen(void) {
    s_set_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(COL_BG), 0);

    lv_obj_t *title = lv_label_create(s_set_scr);
    lv_obj_set_style_text_font(title, &pomo_cjk_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(title, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    for (int i = 0; i < POMODORO_SET_ITEM_COUNT; i++) {
        s_set_rows[i] = lv_label_create(s_set_scr);
        lv_obj_set_style_text_font(s_set_rows[i], &pomo_cjk_16, 0);
        lv_obj_align(s_set_rows[i], LV_ALIGN_TOP_MID, 0, 78 + i * 48);
    }

    // 录音倒计时大数字(平时隐藏)
    s_rec_big = lv_label_create(s_set_scr);
    lv_obj_set_style_text_font(s_rec_big, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_rec_big, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_rec_big, "");
    lv_obj_align(s_rec_big, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_add_flag(s_rec_big, LV_OBJ_FLAG_HIDDEN);

    s_set_hint = lv_label_create(s_set_scr);
    lv_obj_set_style_text_font(s_set_hint, &pomo_cjk_16, 0);
    lv_obj_set_style_text_color(s_set_hint, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(s_set_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_set_hint, "OK: 切换   长按OK: 返回");
    lv_obj_align(s_set_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    settings_refresh_locked();
    s_page_settings = true;
    lv_screen_load(s_set_scr);
}

// ===== LVGL 心跳(LVGL 任务上下文:免锁刷 UI;notify 均非阻塞) =====

static void tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_sleeping) return;
    const uint64_t now = now_ms();

    portENTER_CRITICAL(&s_core_mux);
    const bool finished_now = pomodoro_core_tick(&s_core, now);
    const pomodoro_state_t state = s_core.state;
    const uint32_t remaining = pomodoro_core_remaining_s(&s_core, now);
    const uint64_t idle_ms = now - s_last_activity_ms;
    const uint8_t sleep_min = s_settings.sleep_min;
    portEXIT_CRITICAL(&s_core_mux);

    // 自动休眠策略:1/3/5 分钟为统一闲置门限;
    // "待倒计时结束"=倒计时中不休眠,其余状态闲置 3 分钟休眠
    uint64_t idle_timeout_ms;
    if (sleep_min == POMODORO_SLEEP_WHEN_DONE) {
        idle_timeout_ms = state == POMODORO_STATE_RUNNING
                        ? UINT64_MAX
                        : POMODORO_DONE_IDLE_MIN * 60u * 1000u;
    } else {
        idle_timeout_ms = (uint64_t)sleep_min * 60u * 1000u;
    }

    if (finished_now) alert_start();
    alert_tick(now);
    if (state != s_ui_state || remaining != s_ui_remaining) apply_ui();

    // 长按 UP/DOWN 快调:按住满 500ms 后每 tick 步进一次,松开即停
    if (s_hold_btn >= 0 && now - s_hold_press_ms >= HOLD_START_MS) {
        const int mv = bsp_button_read_mv();
        if (mv >= 0 && mv < 1900) {
            portENTER_CRITICAL(&s_core_mux);
            const pomodoro_action_t repeat_action = pomodoro_core_key(
                &s_core, s_hold_btn == BSP_BTN_UP ? POMODORO_KEY_UP
                                                  : POMODORO_KEY_DOWN, now);
            portEXIT_CRITICAL(&s_core_mux);
            if (repeat_action == POMODORO_ACTION_DURATION) apply_ui();
        } else {
            s_hold_btn = -1;
        }
    }

    static unsigned batt_counter;
    if (++batt_counter >= POMO_BATT_PERIOD_TICKS) {
        batt_counter = 0;
        update_battery_label();
    }

    if (!s_sleep_notified && s_sleep_task && idle_ms >= idle_timeout_ms) {
        s_sleep_notified = true;
        ESP_LOGI(TAG, "闲置 %llu 分钟,进入深睡",
                 (unsigned long long)(idle_ms / 60000ull));
        xTaskNotify(s_sleep_task, SLEEP_CMD_SLEEP, eSetValueWithOverwrite);
    }
}

// ===== 输入分发(回调只入队,处理在任务里) =====

static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (s_sleeping || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = event };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        if (s_sleeping) continue;
        const uint64_t now = now_ms();

        // 任何按键都先停掉进行中的提醒(白屏闪烁/持续蜂鸣)
        if (s_alert_active) alert_stop();

        portENTER_CRITICAL(&s_core_mux);
        s_last_activity_ms = now;
        pomodoro_action_t action = POMODORO_ACTION_NONE;
        bool refresh_settings = false;
        bool open_main = false;
        bool page_settings_requested = false;
        bool changed = false;
        bool became_voice = false;   // 提醒项刚切到"语音"→触发录音
        if (now >= s_input_grace_until_ms) {
            if (s_page_settings) {
                if (input.event == BSP_BTN_CLICK) {
                    if (input.btn == BSP_BTN_UP) {
                        s_set_selected = (s_set_selected + POMODORO_SET_ITEM_COUNT - 1)
                                       % POMODORO_SET_ITEM_COUNT;
                        refresh_settings = true;
                    } else if (input.btn == BSP_BTN_DOWN) {
                        s_set_selected = (s_set_selected + 1) % POMODORO_SET_ITEM_COUNT;
                        refresh_settings = true;
                    } else if (input.btn == BSP_BTN_OK) {
                        const pomodoro_set_item_t item =
                            (pomodoro_set_item_t)s_set_selected;
                        if (pomodoro_settings_cycle(&s_settings, item)) {
                            pomodoro_core_set_step(&s_core, s_settings.step_min);
                            refresh_settings = true;
                            changed = true;
                            became_voice = item == POMODORO_SET_ITEM_ALERT &&
                                           s_settings.alert == POMODORO_ALERT_VOICE;
                        }
                    }
                } else if (input.event == BSP_BTN_LONG && input.btn == BSP_BTN_OK) {
                    open_main = true;
                }
            } else {
                pomodoro_key_t key = POMODORO_KEY_NONE;
                if (input.event == BSP_BTN_CLICK) {
                    if (input.btn == BSP_BTN_UP) {
                        key = POMODORO_KEY_UP;
                        s_hold_btn = -1;                  // 松开,取消快调候选
                    } else if (input.btn == BSP_BTN_DOWN) {
                        key = POMODORO_KEY_DOWN;
                        s_hold_btn = -1;
                    } else if (input.btn == BSP_BTN_OK) {
                        key = POMODORO_KEY_OK;
                    }
                } else if (input.event == BSP_BTN_PRESS &&
                           (input.btn == BSP_BTN_UP || input.btn == BSP_BTN_DOWN) &&
                           s_core.state == POMODORO_STATE_SETUP) {
                    s_hold_btn = (int)input.btn;          // 进入快调候选,tick 接管
                    s_hold_press_ms = now;
                } else if (input.event == BSP_BTN_LONG && input.btn == BSP_BTN_OK) {
                    key = POMODORO_KEY_OK_LONG;
                }
                if (key != POMODORO_KEY_NONE) {
                    action = pomodoro_core_key(&s_core, key, now);
                    // 设置页入口:SETUP 态长按 OK(持锁后建页)
                    if (action == POMODORO_ACTION_NONE &&
                        key == POMODORO_KEY_OK_LONG &&
                        s_core.state == POMODORO_STATE_SETUP) {
                        page_settings_requested = true;
                    }
                }
            }
        }
        portEXIT_CRITICAL(&s_core_mux);

        // 持锁处理 UI 与副作用
        if (open_main) {
            if (bsp_lvgl_lock(500)) { page_open_main(); bsp_lvgl_unlock(); }
        } else if (page_settings_requested) {
            if (bsp_lvgl_lock(500)) { page_open_settings(); bsp_lvgl_unlock(); }
        } else if (s_page_settings) {
            if (refresh_settings && bsp_lvgl_lock(500)) {
                settings_refresh_locked();
                bsp_lvgl_unlock();
            }
            if (became_voice && s_audio_task) {
                xTaskNotify(s_audio_task, AUDIO_CMD_VOICE_REC, eSetValueWithOverwrite);
            }
            if (changed) settings_save();
        }
        if (action != POMODORO_ACTION_NONE && bsp_lvgl_lock(500)) {
            apply_ui();
            bsp_lvgl_unlock();
        }
    }
}

// ===== 深睡 worker(关断顺序取自 demo_low_power.c,顺序是仓库契约) =====

static void log_shutdown_step(const char *step, esp_err_t err) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "深睡继续:%s 失败: %s", step, esp_err_to_name(err));
    }
}

static void sleep_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t command = 0;
        xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);
        if (command != SLEEP_CMD_SLEEP) continue;

        // 1) 输入/tick 停工;蜂鸣 worker 走退出握手(最坏 ~2s)。
        s_sleeping = true;
        // 熄屏前提示(中文用 CJK 子集字体;深睡中倒计时由 RTC 继续走)
        if (bsp_lvgl_lock(1000)) {
            if (s_caption) {
                lv_obj_set_style_text_font(s_caption, &pomo_cjk_16, 0);
                lv_label_set_text(s_caption,
                                  "即将熄屏省电\n倒计时会继续工作\n任意按键恢复");
                lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_MUTED), 0);
            }
            bsp_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_err_t audio_err = audio_worker_stop();
        if (audio_err != ESP_OK) {
            ESP_LOGW(TAG, "蜂鸣 worker 停止超时,随深睡一并断电");
        }

        // 2) 冻结 + 快照 + 落 RTC memory,同一临界区,杜绝与按键竞态。
        uint32_t wake_after_s = 0;
        portENTER_CRITICAL(&s_core_mux);
        const uint64_t now = now_ms();
        pomodoro_core_freeze(&s_core, now);
        const pomodoro_state_t state = s_core.state;
        s_sleep_state.magic = SLEEP_STATE_MAGIC;
        s_sleep_state.version = SLEEP_STATE_VERSION;
        s_sleep_state.state = (uint32_t)state;
        s_sleep_state.duration_s = s_core.duration_s;
        s_sleep_state.elapsed_s = s_core.elapsed_s;
        s_sleep_state.rtc_us_at_sleep = esp_clk_rtc_time();
        if (state == POMODORO_STATE_RUNNING) {
            wake_after_s = pomodoro_core_remaining_s(&s_core, now);
            if (wake_after_s == 0) wake_after_s = 1;  // 关断瞬间恰好到点
        }
        portEXIT_CRITICAL(&s_core_mux);

        // 3) 唤醒源:任意键(C3 无 ext0,用 deep-sleep GPIO 唤醒,低电平触发;
        //    计时中另加 RTC timer 到点补蜂鸣)。
        // 深睡期间唤醒脚的 SLP_SEL 被禁用(gpio_deep_sleep_wakeup_enable 内部),
        // 睡眠中采样的是焊盘"现行配置"的输入路径。GPIO0 平时被按键驱动配成
        // ADC 模拟复用,该路径悬空 → 唤醒检测器读到不确定电平 → 入睡即自唤醒
        // (实测 W:7 而线上电平 2898mV 高位)。入睡准备必须切回普通数字输入,
        // 稳态电平由板载 10k 外部上拉保证,不依赖内部电阻。
        const gpio_config_t wake_io = {
            .pin_bit_mask = 1ULL << WAKE_GPIO_NUM,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&wake_io) != ESP_OK) {
            ESP_LOGE(TAG, "唤醒脚输入配置失败,重启");
            esp_restart();
        }
        esp_err_t wake_err = esp_deep_sleep_enable_gpio_wakeup(
            1ULL << WAKE_GPIO_NUM, ESP_GPIO_WAKEUP_GPIO_LOW);
        if (wake_err == ESP_OK && wake_after_s > 0) {
            wake_err = esp_sleep_enable_timer_wakeup(
                (uint64_t)wake_after_s * 1000000ull);
        }
        if (wake_err != ESP_OK) {
            // 没有任何唤醒源会睡死;重启恢复外设比不可唤醒更安全。
            ESP_LOGE(TAG, "唤醒源设置失败: %s,重启", esp_err_to_name(wake_err));
            esp_restart();
        }

        // 4) 外设关断顺序(与 demo_low_power 一致,勿随意调换)。
        log_shutdown_step("CW2017 suspend", bsp_battery_sleep());
        log_shutdown_step("ES8311 suspend", bsp_audio_sleep());
        log_shutdown_step("I2S pin release", bsp_audio_prepare_deep_sleep());
        log_shutdown_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());

        // 5) 拿 LVGL 锁阻止再刷屏,删掉心跳,再关屏。
        if (!bsp_lvgl_lock(1000)) {
            ESP_LOGE(TAG, "深睡前无法停止 LVGL 刷屏,重启恢复外设");
            esp_restart();
        }
        if (s_tick_timer) {
            lv_timer_delete(s_tick_timer);
            s_tick_timer = NULL;
        }
        log_shutdown_step("ST7789 suspend", bsp_display_prepare_deep_sleep());

        ESP_LOGI(TAG, "进入深睡(剩余 %us,%s 唤醒)", wake_after_s,
                 wake_after_s > 0 ? "timer/按键" : "仅按键");
        esp_deep_sleep_start();
        ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启恢复外设");
        esp_restart();
    }
}

// ===== 启动 =====

void pomodoro_app_start(void) {
    // 0) NVS:设置持久化;失败则用默认值运行(不蜂鸣不重试,功能不受影响)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败(%s),设置不持久化", esp_err_to_name(nvs_err));
    }
    settings_load();
    // BSP 的 s_volume 初始为 0,set_format 打开 codec 时会应用它;
    // 不在此处同步用户音量,开机后首次录音试听将完全无声。
    bsp_audio_set_volume(s_settings.volume);

    // 1) 唤醒恢复:用 esp_sleep_get_wakeup_cause() 精确区分冷启动与深睡唤醒
    //    (比 esp_reset_reason() 更直接,demo_low_power 同款用法)。
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool beep_on_boot = false;
    if ((cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_TIMER) &&
        s_sleep_state.magic == SLEEP_STATE_MAGIC &&
        s_sleep_state.version == SLEEP_STATE_VERSION) {
        const uint64_t slept_us = esp_clk_rtc_time() - s_sleep_state.rtc_us_at_sleep;
        portENTER_CRITICAL(&s_core_mux);
        const pomodoro_state_t restored = pomodoro_core_restore(
            &s_core, (pomodoro_state_t)s_sleep_state.state,
            s_sleep_state.duration_s, s_sleep_state.elapsed_s,
            slept_us, now_ms());
        portEXIT_CRITICAL(&s_core_mux);
        // 睡穿了的计时:唤醒后按设置补一次提醒。
        beep_on_boot = restored == POMODORO_STATE_FINISHED &&
                       (pomodoro_state_t)s_sleep_state.state == POMODORO_STATE_RUNNING;
        s_input_grace_until_ms = now_ms() + 1000;
        ESP_LOGI(TAG, "深睡唤醒恢复:cause=%d slept=%llus state=%d",
                 (int)cause, (unsigned long long)(slept_us / 1000000ull),
                 (int)restored);
    } else {
        portENTER_CRITICAL(&s_core_mux);
        pomodoro_core_init(&s_core);
        portEXIT_CRITICAL(&s_core_mux);
    }
    s_sleep_state.magic = 0;   // 现场一次性消费,防异常重启后误恢复
    portENTER_CRITICAL(&s_core_mux);
    s_last_activity_ms = now_ms();
    pomodoro_core_set_step(&s_core, s_settings.step_min);
    portEXIT_CRITICAL(&s_core_mux);

    // 2) worker 与输入分发(单项失败降级:不蜂鸣/不入睡,其余照常)。
    esp_err_t err = audio_worker_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "蜂鸣 worker 创建失败: %s", esp_err_to_name(err));
    if (xTaskCreate(sleep_task, "pomo_sleep", 3072, NULL, 4, &s_sleep_task) != pdPASS) {
        s_sleep_task = NULL;
        ESP_LOGE(TAG, "深睡 worker 创建失败:闲置不会自动入睡");
    }

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (s_input_queue &&
        xTaskCreate(input_task, "pomo_input", 4096, NULL, 5, &s_input_task) == pdPASS) {
        if (bsp_button_init(on_key, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "按键初始化失败:三键不可用");
        }
    } else {
        ESP_LOGE(TAG, "输入任务创建失败:三键不可用");
    }

    // 3) UI 与心跳(持锁建屏;tick 定时器归 LVGL 任务所有)。
    if (bsp_lvgl_lock(1000)) {
        build_screen();
        apply_ui();
        update_battery_label();
        s_tick_timer = lv_timer_create(tick_cb, POMO_TICK_MS, NULL);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,UI 未创建(计时逻辑仍在运行)");
    }

    if (beep_on_boot) alert_start();
}
