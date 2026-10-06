// main/pomodoro_core.h —— 三键番茄钟纯逻辑:状态机 + 倒计时数学 + 深睡恢复。
// 无 ESP-IDF/LVGL 依赖,host 测试直接编译(tests/test_pomodoro_core.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POMODORO_MIN_MINUTES      1
#define POMODORO_MAX_MINUTES    180
#define POMODORO_DEFAULT_MINUTES 25

// 可配置的时长步幅(分钟)与提醒方式;设置持久化在 NVS,结构体本身是纯数据。
typedef enum {
    POMODORO_ALERT_FLASH_5 = 0,   // 静音:白屏闪 5 次
    POMODORO_ALERT_FLASH_LOOP,    // 静音:白屏闪烁直到按键
    POMODORO_ALERT_BEEP_SHORT,    // 蜂鸣:三短声(默认)
    POMODORO_ALERT_BEEP_LOOP,     // 蜂鸣:持续直到按键
    POMODORO_ALERT_VOICE,         // 语音:播放设备录制的提示(深睡唤醒后回退蜂鸣)
    POMODORO_ALERT_COUNT,
} pomodoro_alert_t;

typedef enum {
    POMODORO_SET_ITEM_STEP = 0,   // 步幅:1/5/10/30 分钟
    POMODORO_SET_ITEM_ALERT,      // 提醒方式
    POMODORO_SET_ITEM_VOLUME,     // 音量:0..100(步进 20)
    POMODORO_SET_ITEM_SLEEP,      // 自动休眠:1/3/5 分钟;0=待倒计时结束
    POMODORO_SET_ITEM_COUNT,
} pomodoro_set_item_t;

typedef struct {
    uint8_t step_min;             // 步幅分钟数,取值见 pomodoro_settings_step_choices
    uint8_t alert;                // pomodoro_alert_t
    uint8_t volume;               // 0..100
    uint8_t sleep_min;            // 1/3/5 分钟;0 = 待倒计时结束才休眠
} pomodoro_settings_t;

extern const uint8_t pomodoro_step_choices[4];
#define POMODORO_STEP_CHOICES_COUNT 4
#define POMODORO_VOLUME_STEP    20u
#define POMODORO_SLEEP_WHEN_DONE 0u  // "待倒计时结束":倒计时中不休眠,结束后 3 分钟休眠
#define POMODORO_DONE_IDLE_MIN   3u  // 待倒计时结束模式下,非运行态的闲置休眠分钟

void pomodoro_settings_defaults(pomodoro_settings_t *settings);
bool pomodoro_settings_load_valid(const pomodoro_settings_t *settings);
// 循环切换某项的取值;返回 true 表示取值发生变化。
bool pomodoro_settings_cycle(pomodoro_settings_t *settings,
                             pomodoro_set_item_t item);

typedef enum {
    POMODORO_STATE_SETUP = 0,   // 设置时长:UP/DOWN ±5 分钟,OK 开始
    POMODORO_STATE_RUNNING,     // 计时中:OK 暂停
    POMODORO_STATE_PAUSED,      // 暂停:OK 继续
    POMODORO_STATE_FINISHED,    // 计时结束(已蜂鸣):OK 回设置页
} pomodoro_state_t;

typedef enum {
    POMODORO_ACTION_NONE = 0,
    POMODORO_ACTION_DURATION,   // 设置页时长变化,刷新大数字
    POMODORO_ACTION_STARTED,    // 进入运行页
    POMODORO_ACTION_RESUMED,
    POMODORO_ACTION_PAUSED,
    POMODORO_ACTION_RESET,      // 运行/暂停/结束页返回设置页
} pomodoro_action_t;

typedef enum {
    POMODORO_KEY_NONE = 0,
    POMODORO_KEY_UP,
    POMODORO_KEY_DOWN,
    POMODORO_KEY_OK,
    POMODORO_KEY_OK_LONG,       // OK 长按(≥500ms,BSP Kconfig 下限)
} pomodoro_key_t;

// 计时基准全部用 (now_ms - anchor_ms) 无符号差值,不做绝对时刻比较,
// 毫秒计数翻转(Tasmota/WLED 的 49.7 天经典事故)不会反转判定。
typedef struct {
    pomodoro_state_t state;
    uint32_t duration_s;        // 设定总时长(秒)
    uint32_t elapsed_s;         // anchor 时刻之前已累计的秒数
    uint64_t anchor_ms;         // 最近一次 start/resume 的毫秒基准
    uint32_t step_min;          // 设置页步幅(分钟),由应用随设置同步
} pomodoro_core_t;

void pomodoro_core_init(pomodoro_core_t *core);
uint32_t pomodoro_core_minutes(const pomodoro_core_t *core);
void pomodoro_core_step_minutes(pomodoro_core_t *core, int direction,
                                uint32_t step_min);
void pomodoro_core_set_step(pomodoro_core_t *core, uint32_t step_min);

uint32_t pomodoro_core_remaining_s(const pomodoro_core_t *core, uint64_t now_ms);
// 已计时占比,0..1000,供圆弧进度使用。
uint32_t pomodoro_core_progress_permille(const pomodoro_core_t *core, uint64_t now_ms);

// 按键驱动状态机;返回 UI 需要执行的动作。
pomodoro_action_t pomodoro_core_key(pomodoro_core_t *core, pomodoro_key_t key,
                                    uint64_t now_ms);
// 周期心跳:RUNNING 扣到 0 时迁移到 FINISHED。返回 true 表示状态发生变化
// (调用方据此触发蜂鸣)。仅在 RUNNING 态可能返回 true。
bool pomodoro_core_tick(pomodoro_core_t *core, uint64_t now_ms);

// 深睡前冻结:把 anchor 之后的运行时长折叠进 elapsed_s,状态保持不变
// (RUNNING 冻结后 remaining 不再增长,唤醒时用睡眠时长补扣)。
void pomodoro_core_freeze(pomodoro_core_t *core, uint64_t now_ms);

// 唤醒恢复:RUNNING 时用 slept_us(深睡期间 RTC 计数)补扣,扣穿即 FINISHED;
// PAUSED/SETUP/FINISHED 原样恢复。非法入参回退为干净的 SETUP 默认态。
pomodoro_state_t pomodoro_core_restore(pomodoro_core_t *core,
                                       pomodoro_state_t saved_state,
                                       uint32_t duration_s, uint32_t elapsed_s,
                                       uint64_t slept_us, uint64_t now_ms);

// "MM:SS"(最多 "60:00");size 至少 8 字节。
void pomodoro_format_mmss(uint32_t remaining_s, char *buf, size_t size);
