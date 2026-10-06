// main/main.c —— 三键番茄钟(衍生应用)引导:初始化板级外设后直接进入应用。
//
// 不进入 BSP 演示菜单(demo_*.c 留在仓库作参考范本,不入本次固件)。
// 按键语义(应用内):
//   上/下 短按   设置页=±5 分钟(5..60 夹紧);运行页无操作
//   确定  短按   设置页=开始;运行/暂停页=暂停/继续;结束页=回设置页
//   确定  长按   运行/暂停页=放弃本次,回设置页(长按下限 500ms,BSP 硬约束)
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "pomodoro_app.h"

#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "三键番茄钟启动");
    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", (int)wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本应用唯一的 UI 载体,失败就没有可用界面,打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 蜂鸣/电量单项失败不阻塞:各自降级(无蜂鸣/不显示电量),计时照常。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败:结束蜂鸣不可用");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败:右上角不显示电量");
    }

    pomodoro_app_start();
}
