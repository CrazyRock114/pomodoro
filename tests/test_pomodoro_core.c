#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pomodoro_core.h"

static void test_defaults_and_duration_steps(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    assert(core.state == POMODORO_STATE_SETUP);
    assert(pomodoro_core_minutes(&core) == 25);

    // UP 连按到 180 后夹紧,DOWN 回到 1(越界夹边),再回 25。
    for (int i = 0; i < 60; i++) {
        pomodoro_action_t a = pomodoro_core_key(&core, POMODORO_KEY_UP, 0);
        assert(a == POMODORO_ACTION_DURATION || a == POMODORO_ACTION_NONE);
    }
    assert(pomodoro_core_minutes(&core) == 180);
    assert(pomodoro_core_key(&core, POMODORO_KEY_UP, 0) == POMODORO_ACTION_NONE);
    assert(pomodoro_core_minutes(&core) == 180);   // 已在边界,钳制不动返回 NONE
    for (int i = 0; i < 60; i++) {
        (void)pomodoro_core_key(&core, POMODORO_KEY_DOWN, 0);
    }
    assert(pomodoro_core_minutes(&core) == 1);
    assert(pomodoro_core_key(&core, POMODORO_KEY_DOWN, 0) == POMODORO_ACTION_NONE);
    assert(pomodoro_core_minutes(&core) == 1);
    // 1 分钟 + 步幅 5 → 夹紧回 1? 不:UP 是 +5 → 6;DOWN 从 1 → 夹回 1
    (void)pomodoro_core_key(&core, POMODORO_KEY_UP, 0);
    assert(pomodoro_core_minutes(&core) == 6);
    (void)pomodoro_core_key(&core, POMODORO_KEY_DOWN, 0);
    assert(pomodoro_core_minutes(&core) == 1);
    // 步幅 30:1 → UP 直接 31,再 UP 夹到 180
    pomodoro_core_set_step(&core, 30);
    (void)pomodoro_core_key(&core, POMODORO_KEY_UP, 0);
    assert(pomodoro_core_minutes(&core) == 31);
    (void)pomodoro_core_key(&core, POMODORO_KEY_UP, 0);
    assert(pomodoro_core_minutes(&core) == 61);
    pomodoro_core_set_step(&core, 5);
    core.duration_s = POMODORO_DEFAULT_MINUTES * 60u;   // 复位到 25 供后续用例

    // 运行中 UP/DOWN 不改时长。
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 1000) == POMODORO_ACTION_STARTED);
    assert(core.state == POMODORO_STATE_RUNNING);
    assert(pomodoro_core_key(&core, POMODORO_KEY_UP, 2000) == POMODORO_ACTION_NONE);
    assert(pomodoro_core_minutes(&core) == 25);
}

static void test_settings_cycles(void) {
    pomodoro_settings_t st;
    pomodoro_settings_defaults(&st);
    assert(pomodoro_settings_load_valid(&st));
    assert(st.step_min == 5 && st.alert == POMODORO_ALERT_BEEP_SHORT &&
           st.volume == 80);

    // 步幅循环 5→10→30→1→5
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_STEP));
    assert(st.step_min == 10);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_STEP));
    assert(st.step_min == 30);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_STEP));
    assert(st.step_min == 1);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_STEP));
    assert(st.step_min == 5);

    // 提醒循环按枚举序 +1 取模:短蜂(2)→持续(3)→语音(4)→闪5(0)→闪循环(1)→短蜂
    assert(st.alert == POMODORO_ALERT_BEEP_SHORT);
    for (int i = 0; i < POMODORO_ALERT_COUNT; i++) {
        (void)pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_ALERT);
    }
    assert(st.alert == POMODORO_ALERT_BEEP_SHORT);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_ALERT));
    assert(st.alert == POMODORO_ALERT_BEEP_LOOP);

    // 音量:80→100→0→20(对 101 取模回绕)
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_VOLUME));
    assert(st.volume == 100);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_VOLUME));
    assert(st.volume == 0);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_VOLUME));
    assert(st.volume == 20);
    assert(pomodoro_settings_load_valid(&st));

    // 休眠循环:3→5→待倒计时结束(0)→1→3
    assert(st.sleep_min == 3);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_SLEEP));
    assert(st.sleep_min == 5);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_SLEEP));
    assert(st.sleep_min == POMODORO_SLEEP_WHEN_DONE);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_SLEEP));
    assert(st.sleep_min == 1);
    assert(pomodoro_settings_cycle(&st, POMODORO_SET_ITEM_SLEEP));
    assert(st.sleep_min == 3);

    // 非法持久化数据(脏 NVS)必须被识别,交由上层回退默认
    pomodoro_settings_t bad = st;
    bad.step_min = 7;
    assert(!pomodoro_settings_load_valid(&bad));
    bad = st; bad.alert = POMODORO_ALERT_COUNT;
    assert(!pomodoro_settings_load_valid(&bad));
    bad = st; bad.volume = 55;
    assert(!pomodoro_settings_load_valid(&bad));
    bad = st; bad.sleep_min = 2;
    assert(!pomodoro_settings_load_valid(&bad));
}

static void test_remaining_and_wraparound(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    core.duration_s = 25u * 60u;

    // 设置页:剩余 == 设定时长。
    assert(pomodoro_core_remaining_s(&core, 0) == 25u * 60u);

    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 1000) == POMODORO_ACTION_STARTED);
    assert(pomodoro_core_remaining_s(&core, 61000) == 25u * 60u - 60);

    // anchor 邻近 uint64 上限,now 越过翻转点:无符号差值仍然正确。
    const uint64_t anchor = UINT64_MAX - 4000u;   // 差 4 秒翻转
    core.anchor_ms = anchor;
    const uint64_t now = 1000;                    // 翻转后 1 秒
    assert(pomodoro_core_remaining_s(&core, now) == 25u * 60u - 5);
    assert(pomodoro_core_progress_permille(&core, now) > 0);
}

static void test_pause_resume_freeze_exactness(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    core.duration_s = 10u * 60u;

    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 0) == POMODORO_ACTION_STARTED);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 90000) == POMODORO_ACTION_PAUSED);
    // 暂停后时间不再流逝。
    assert(pomodoro_core_remaining_s(&core, 999999) == 10u * 60u - 90);
    // 继续后从暂停点接续。
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 100000) == POMODORO_ACTION_RESUMED);
    assert(pomodoro_core_remaining_s(&core, 160000) == 10u * 60u - 150);
}

static void test_tick_finishes_once_and_progress_full(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    core.duration_s = 60u;

    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 0) == POMODORO_ACTION_STARTED);
    assert(!pomodoro_core_tick(&core, 59999));
    assert(pomodoro_core_tick(&core, 60000));
    assert(core.state == POMODORO_STATE_FINISHED);
    assert(pomodoro_core_remaining_s(&core, 60000) == 0);
    assert(pomodoro_core_progress_permille(&core, 60000) == 1000);
    // FINISHED 心跳不再触发。
    assert(!pomodoro_core_tick(&core, 120000));
    // OK 短按/长按都回设置页。
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 121000) == POMODORO_ACTION_RESET);
    assert(core.state == POMODORO_STATE_SETUP);
    assert(pomodoro_core_remaining_s(&core, 121000) == 60u);
}

static void test_long_press_reset_from_running_and_paused(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    core.duration_s = 30u * 60u;

    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 0) == POMODORO_ACTION_STARTED);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK_LONG, 20000)
               == POMODORO_ACTION_RESET);
    assert(core.state == POMODORO_STATE_SETUP);
    assert(core.elapsed_s == 0);
    assert(pomodoro_core_remaining_s(&core, 20000) == 30u * 60u);

    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 21000) == POMODORO_ACTION_STARTED);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 22000) == POMODORO_ACTION_PAUSED);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK_LONG, 26000)
               == POMODORO_ACTION_RESET);
    assert(core.state == POMODORO_STATE_SETUP);
}

static void test_freeze_then_restore_across_sleep(void) {
    pomodoro_core_t core;
    pomodoro_core_init(&core);
    core.duration_s = 25u * 60u;

    // 跑 5 分钟后入睡:冻结把 anchor 差折叠进 elapsed。
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 0) == POMODORO_ACTION_STARTED);
    pomodoro_core_freeze(&core, 5u * 60u * 1000u);
    assert(core.elapsed_s == 5u * 60u);

    // 睡 10 分钟后唤醒:剩余 25-5-10=10 分钟,仍是 RUNNING。
    pomodoro_state_t restored = pomodoro_core_restore(
        &core, POMODORO_STATE_RUNNING, core.duration_s, core.elapsed_s,
        10u * 60u * 1000000ull, 12345);
    assert(restored == POMODORO_STATE_RUNNING);
    assert(core.state == POMODORO_STATE_RUNNING);
    assert(pomodoro_core_remaining_s(&core, 12345) == 10u * 60u);

    // 再睡 11 分钟扣穿:恢复即 FINISHED(唤醒方据此补蜂鸣)。
    pomodoro_core_freeze(&core, 13000);
    restored = pomodoro_core_restore(
        &core, POMODORO_STATE_RUNNING, core.duration_s, core.elapsed_s,
        11u * 60u * 1000000ull, 20000);
    assert(restored == POMODORO_STATE_FINISHED);
    assert(pomodoro_core_remaining_s(&core, 20000) == 0);

    // 暂停入睡:睡眠时长不应计入。
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 21000) == POMODORO_ACTION_RESET);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 22000) == POMODORO_ACTION_STARTED);
    assert(pomodoro_core_key(&core, POMODORO_KEY_OK, 23000) == POMODORO_ACTION_PAUSED);
    pomodoro_core_freeze(&core, 23000);
    restored = pomodoro_core_restore(
        &core, POMODORO_STATE_PAUSED, core.duration_s, core.elapsed_s,
        60u * 60u * 1000000ull, 30000);
    assert(restored == POMODORO_STATE_PAUSED);
    // 睡了 1 小时不计入:剩余仍是"暂停时刻的 25 分钟 - 起跑后的 1 秒"。
    assert(pomodoro_core_remaining_s(&core, 30000) == 25u * 60u - 1);

    // 非法保存态与越界数据:回退为干净的 SETUP 默认值。
    restored = pomodoro_core_restore(&core, (pomodoro_state_t)77, 99999, 99999, 0, 1);
    assert(restored == POMODORO_STATE_SETUP);
    assert(pomodoro_core_minutes(&core) == POMODORO_DEFAULT_MINUTES);
    restored = pomodoro_core_restore(&core, POMODORO_STATE_PAUSED,
                                     30u, 30u, 0, 1);
    assert(restored == POMODORO_STATE_PAUSED);
    assert(pomodoro_core_minutes(&core) == 1);   // 30 秒 < 1 分钟下限,夹紧
}

static void test_format(void) {
    char buf[8];
    pomodoro_format_mmss(0, buf, sizeof buf);
    assert(strcmp(buf, "00:00") == 0);
    pomodoro_format_mmss(59, buf, sizeof buf);
    assert(strcmp(buf, "00:59") == 0);
    pomodoro_format_mmss(60, buf, sizeof buf);
    assert(strcmp(buf, "01:00") == 0);
    pomodoro_format_mmss(60u * 60u, buf, sizeof buf);
    assert(strcmp(buf, "60:00") == 0);
    pomodoro_format_mmss(180u * 60u, buf, sizeof buf);
    assert(strcmp(buf, "180:00") == 0);
    pomodoro_format_mmss(0xFFFFFFFF, buf, sizeof buf);
    assert(strcmp(buf, "180:59") == 0);   // 越界钳制到上限
}

int main(void) {
    test_defaults_and_duration_steps();
    test_settings_cycles();
    test_remaining_and_wraparound();
    test_pause_resume_freeze_exactness();
    test_tick_finishes_once_and_progress_full();
    test_long_press_reset_from_running_and_paused();
    test_freeze_then_restore_across_sleep();
    test_format();
    puts("pomodoro core state machine / sleep restore / wake suppress: PASS");
    return 0;
}
