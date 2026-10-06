#include "pomodoro_core.h"

#include <stdio.h>

static uint32_t clamp_duration(uint32_t seconds) {
    const uint32_t low = POMODORO_MIN_MINUTES * 60u;
    const uint32_t high = POMODORO_MAX_MINUTES * 60u;
    if (seconds < low) return low;
    if (seconds > high) return high;
    return seconds;
}

// RUNNING 态已累计的秒数:anchor 前的存量 + 无符号差值,天然防翻转。
static uint32_t elapsed_total_s(const pomodoro_core_t *core, uint64_t now_ms) {
    if (core->state != POMODORO_STATE_RUNNING) return core->elapsed_s;
    const uint64_t diff_ms = now_ms - core->anchor_ms;
    const uint64_t total = core->elapsed_s + diff_ms / 1000u;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

void pomodoro_core_init(pomodoro_core_t *core) {
    core->state = POMODORO_STATE_SETUP;
    core->duration_s = POMODORO_DEFAULT_MINUTES * 60u;
    core->elapsed_s = 0;
    core->anchor_ms = 0;
    core->step_min = 5;
}

void pomodoro_core_set_step(pomodoro_core_t *core, uint32_t step_min) {
    core->step_min = step_min ? step_min : 5;
}

uint32_t pomodoro_core_minutes(const pomodoro_core_t *core) {
    return core->duration_s / 60u;
}

void pomodoro_core_step_minutes(pomodoro_core_t *core, int direction,
                                uint32_t step_min) {
    // 按设置步幅步进,越界时夹紧到 1..180 边界(如 5 分钟按 DOWN 步幅 5 → 1)。
    int32_t minutes = (int32_t)pomodoro_core_minutes(core)
                    + direction * (int32_t)step_min;
    if (minutes < POMODORO_MIN_MINUTES) minutes = POMODORO_MIN_MINUTES;
    if (minutes > POMODORO_MAX_MINUTES) minutes = POMODORO_MAX_MINUTES;
    core->duration_s = (uint32_t)minutes * 60u;
}

const uint8_t pomodoro_step_choices[POMODORO_STEP_CHOICES_COUNT] = { 1, 5, 10, 30 };

void pomodoro_settings_defaults(pomodoro_settings_t *settings) {
    settings->step_min = 5;
    settings->alert = POMODORO_ALERT_BEEP_SHORT;
    settings->volume = 80;
    settings->sleep_min = 3;
}

bool pomodoro_settings_load_valid(const pomodoro_settings_t *settings) {
    bool ok = false;
    for (uint32_t i = 0; i < POMODORO_STEP_CHOICES_COUNT; i++) {
        ok = ok || settings->step_min == pomodoro_step_choices[i];
    }
    const bool sleep_ok = settings->sleep_min == POMODORO_SLEEP_WHEN_DONE ||
                          settings->sleep_min == 1u ||
                          settings->sleep_min == 3u ||
                          settings->sleep_min == 5u;
    return ok && sleep_ok && settings->alert < POMODORO_ALERT_COUNT &&
           settings->volume <= 100 && settings->volume % POMODORO_VOLUME_STEP == 0;
}

bool pomodoro_settings_cycle(pomodoro_settings_t *settings,
                             pomodoro_set_item_t item) {
    switch (item) {
    case POMODORO_SET_ITEM_STEP: {
        uint32_t idx = 0;
        for (uint32_t i = 0; i < POMODORO_STEP_CHOICES_COUNT; i++) {
            if (settings->step_min == pomodoro_step_choices[i]) idx = i;
        }
        idx = (idx + 1u) % POMODORO_STEP_CHOICES_COUNT;
        settings->step_min = pomodoro_step_choices[idx];
        return true;
    }
    case POMODORO_SET_ITEM_ALERT:
        settings->alert = (uint8_t)((settings->alert + 1u) % POMODORO_ALERT_COUNT);
        return true;
    case POMODORO_SET_ITEM_VOLUME: {
        const uint32_t v = settings->volume + POMODORO_VOLUME_STEP;
        settings->volume = (uint8_t)(v > 100u ? 0u : v);
        return true;
    }
    case POMODORO_SET_ITEM_SLEEP: {
        static const uint8_t sleep_choices[4] = { 1u, 3u, 5u,
                                                  POMODORO_SLEEP_WHEN_DONE };
        uint32_t idx = 0;
        for (uint32_t i = 0; i < 4u; i++) {
            if (settings->sleep_min == sleep_choices[i]) idx = i;
        }
        settings->sleep_min = sleep_choices[(idx + 1u) % 4u];
        return true;
    }
    default:
        return false;
    }
}

uint32_t pomodoro_core_remaining_s(const pomodoro_core_t *core, uint64_t now_ms) {
    if (core->state == POMODORO_STATE_SETUP) return core->duration_s;
    const uint32_t elapsed = elapsed_total_s(core, now_ms);
    return elapsed >= core->duration_s ? 0 : core->duration_s - elapsed;
}

uint32_t pomodoro_core_progress_permille(const pomodoro_core_t *core,
                                         uint64_t now_ms) {
    if (core->state == POMODORO_STATE_SETUP) return 0;
    if (core->state == POMODORO_STATE_FINISHED) return 1000;
    const uint32_t elapsed = elapsed_total_s(core, now_ms);
    if (elapsed >= core->duration_s) return 1000;
    // duration ≤ 3600s,乘 1000 不会溢出 uint32(上限 3.6e6)。
    return elapsed * 1000u / core->duration_s;
}

pomodoro_action_t pomodoro_core_key(pomodoro_core_t *core, pomodoro_key_t key,
                                    uint64_t now_ms) {
    switch (core->state) {
    case POMODORO_STATE_SETUP:
        if (key == POMODORO_KEY_UP || key == POMODORO_KEY_DOWN) {
            const uint32_t before = core->duration_s;
            pomodoro_core_step_minutes(core, key == POMODORO_KEY_UP ? 1 : -1,
                                       core->step_min);
            return core->duration_s != before ? POMODORO_ACTION_DURATION
                                              : POMODORO_ACTION_NONE;
        }
        if (key == POMODORO_KEY_OK) {
            core->state = POMODORO_STATE_RUNNING;
            core->elapsed_s = 0;
            core->anchor_ms = now_ms;
            return POMODORO_ACTION_STARTED;
        }
        return POMODORO_ACTION_NONE;
    case POMODORO_STATE_RUNNING:
        if (key == POMODORO_KEY_OK) {
            pomodoro_core_freeze(core, now_ms);
            core->state = POMODORO_STATE_PAUSED;
            return POMODORO_ACTION_PAUSED;
        }
        if (key == POMODORO_KEY_OK_LONG) {
            core->state = POMODORO_STATE_SETUP;
            core->elapsed_s = 0;
            return POMODORO_ACTION_RESET;
        }
        return POMODORO_ACTION_NONE;
    case POMODORO_STATE_PAUSED:
        if (key == POMODORO_KEY_OK) {
            core->state = POMODORO_STATE_RUNNING;
            core->anchor_ms = now_ms;
            return POMODORO_ACTION_RESUMED;
        }
        if (key == POMODORO_KEY_OK_LONG) {
            core->state = POMODORO_STATE_SETUP;
            core->elapsed_s = 0;
            return POMODORO_ACTION_RESET;
        }
        return POMODORO_ACTION_NONE;
    case POMODORO_STATE_FINISHED:
        if (key == POMODORO_KEY_OK || key == POMODORO_KEY_OK_LONG) {
            core->state = POMODORO_STATE_SETUP;
            core->elapsed_s = 0;
            return POMODORO_ACTION_RESET;
        }
        return POMODORO_ACTION_NONE;
    }
    return POMODORO_ACTION_NONE;
}

bool pomodoro_core_tick(pomodoro_core_t *core, uint64_t now_ms) {
    if (core->state != POMODORO_STATE_RUNNING) return false;
    if (pomodoro_core_remaining_s(core, now_ms) > 0) return false;
    // 记满全程,PAUSED/FINISHED 态的进度读数保持精确的 100%。
    core->elapsed_s = core->duration_s;
    core->state = POMODORO_STATE_FINISHED;
    return true;
}

void pomodoro_core_freeze(pomodoro_core_t *core, uint64_t now_ms) {
    if (core->state != POMODORO_STATE_RUNNING) return;
    const uint32_t elapsed = elapsed_total_s(core, now_ms);
    core->elapsed_s = elapsed > core->duration_s ? core->duration_s : elapsed;
    core->anchor_ms = now_ms;
}

pomodoro_state_t pomodoro_core_restore(pomodoro_core_t *core,
                                       pomodoro_state_t saved_state,
                                       uint32_t duration_s, uint32_t elapsed_s,
                                       uint64_t slept_us, uint64_t now_ms) {
    if (saved_state != POMODORO_STATE_SETUP &&
        saved_state != POMODORO_STATE_RUNNING &&
        saved_state != POMODORO_STATE_PAUSED &&
        saved_state != POMODORO_STATE_FINISHED) {
        pomodoro_core_init(core);
        return core->state;
    }

    core->state = saved_state;
    core->duration_s = clamp_duration(duration_s);
    core->elapsed_s = elapsed_s > core->duration_s ? core->duration_s : elapsed_s;
    core->anchor_ms = now_ms;

    if (saved_state == POMODORO_STATE_RUNNING) {
        // 睡眠期间倒计时照走:补扣 RTC 时长,扣穿即结束(蜂鸣由唤醒方触发)。
        const uint64_t slept_s = slept_us / 1000000u;
        const uint64_t total = core->elapsed_s + slept_s;
        core->elapsed_s = total >= core->duration_s ? core->duration_s
                                                    : (uint32_t)total;
        core->state = core->elapsed_s >= core->duration_s
                    ? POMODORO_STATE_FINISHED
                    : POMODORO_STATE_RUNNING;
    }
    return core->state;
}

void pomodoro_format_mmss(uint32_t remaining_s, char *buf, size_t size) {
    // 本应用时长上限 180 分钟;钳制后 "%02u:%02u" 最长 "180:00",
    // GCC -Wformat-truncation 才能证明 8 字节缓冲足够。
    if (remaining_s > 180u * 60u + 59u) remaining_s = 180u * 60u + 59u;
    snprintf(buf, size, "%02u:%02u",
             (unsigned)(remaining_s / 60u), (unsigned)(remaining_s % 60u));
}
