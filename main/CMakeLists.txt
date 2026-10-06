# 三键番茄钟(衍生应用):demo_*.c / ui_pixel*.c 留在仓库作参考范本,
# 不再编入本固件(host 测试仍直接从 tests/ 引用它们)。
idf_component_register(
    SRCS "main.c"
         "pomodoro_core.c"
         "pomodoro_app.c"
         "pomo_cjk_16.c"
         "pomo_cjk_48.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer nvs_flash
)
