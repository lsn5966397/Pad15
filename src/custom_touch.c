#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <stdlib.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(pad15_leds), okay)

LOG_MODULE_REGISTER(custom_touch_slider, LOG_LEVEL_INF);

/* 
 * 绕过 YAML 绑定，直接在 C 代码中获取内置的 gpio0 和 gpio1 引脚。
 * 注意：这里的引脚顺序要和你的物理布局从上到下保持一致。
 */
static const struct gpio_dt_spec pads[] = {
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 6,  .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 4,  .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin = 11, .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN },
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 0, .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN },
};

#define NUM_PADS ARRAY_SIZE(pads)

/* 状态机变量 */
static int current_step = -1;       // 当前按下的触点索引
static int64_t last_trigger_time = 0; // 上一个触点触发的时间戳

/* 处理触摸逻辑的线程参数 */
#define STACK_SIZE 1024
#define PRIORITY 7

/* ==============================================
 * 核心逻辑：时序与方向判断
 * ============================================== */
void touch_slider_thread(void) {
    int active_pad = -1;

    while (1) {
        active_pad = -1;

        /* 1. 扫描哪个引脚被触发了（高电平） */
        for (int i = 0; i < NUM_PADS; i++) {
            if (gpio_pin_get_dt(&pads[i]) == 1) {
                active_pad = i;
                break; // 假设同一时间主要只有一个手指有效接触
            }
        }

        /* 2. 状态机判断滑动方向和速度 */
        if (active_pad != -1 && active_pad != current_step) {
            int64_t current_time = k_uptime_get(); // 获取当前毫秒数

            /* 如果是连续滑动（相邻的触点） */
            if (current_step != -1 && abs(active_pad - current_step) == 1) {
                
                int64_t time_diff = current_time - last_trigger_time;
                int scroll_speed = 1;

                /* 速度算法：时间差越短（滑得越快），发出的滚轮步数越大 
                   你可以根据实际手感调整这里的毫秒阈值 */
                if (time_diff < 30) {
                    scroll_speed = 5; // 极快
                } else if (time_diff < 80) {
                    scroll_speed = 3; // 较快
                } else {
                    scroll_speed = 1; // 正常速度
                }

                /* 发送鼠标滚轮事件到 Zephyr 系统 */
                if (active_pad > current_step) {
                    /* 向下滑动 (索引增大 0->1->2->3) */
                    input_report_rel(NULL, INPUT_REL_WHEEL, -scroll_speed, true, K_NO_WAIT);
                    LOG_INF("Scroll DOWN, Speed: %d, TimeDiff: %lld", scroll_speed, time_diff);
                } else {
                    /* 向上滑动 (索引减小 3->2->1->0) */
                    input_report_rel(NULL, INPUT_REL_WHEEL, scroll_speed, true, K_NO_WAIT);
                    LOG_INF("Scroll UP, Speed: %d, TimeDiff: %lld", scroll_speed, time_diff);
                }
            }

            /* 更新状态 */
            current_step = active_pad;
            last_trigger_time = current_time;
        } 
        else if (active_pad == -1) {
            /* 手指离开，重置状态机 */
            current_step = -1;
        }

        /* 睡眠 10ms，决定了采样的帧率 (100Hz) */
        k_msleep(10); 
    }
}

/* ==============================================
 * 初始化过程
 * ============================================== */
static int touch_slider_init(void) {
    for (int i = 0; i < NUM_PADS; i++) {
        if (!gpio_is_ready_dt(&pads[i])) {
            LOG_ERR("GPIO pad %d not ready", i);
            return -ENODEV;
        }
        gpio_pin_configure_dt(&pads[i], GPIO_INPUT);
    }
    LOG_INF("Custom touch slider initialized successfully.");
    return 0;
}

/* 在系统启动时执行初始化 */
SYS_INIT(touch_slider_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* 定义并启动一个后台线程来专门运行我们的扫描逻辑 */
K_THREAD_DEFINE(touch_slider_tid, STACK_SIZE,
                (k_thread_entry_t)touch_slider_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);

#endif /* DT_NODE_HAS_STATUS */
