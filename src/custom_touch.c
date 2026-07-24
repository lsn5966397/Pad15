#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <stdlib.h>

/* 注意：这里你借用了 pad15_leds 节点来做编译隔离，虽然是权宜之计但很管用 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(pad15_leds), okay)

LOG_MODULE_REGISTER(custom_touch_slider, LOG_LEVEL_INF);

/* 
 * 物理与逻辑映射：
 * 这里的数组索引 0, 1, 2, 3 直接代表了 触摸板的从上到下。
 * 以后引脚怎么变，只要按“最上 -> 次上 -> 次下 -> 最下”的顺序填入这里，
 * 下面的业务逻辑一行都不用改！
 */
static const struct gpio_dt_spec pads[] = {
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 0,  .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN }, // [0] 最上方触点
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin = 11, .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN }, // [1] 
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 4,  .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN }, // [2] 
    { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 6,  .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN }, // [3] 最下方触点
};

#define NUM_PADS ARRAY_SIZE(pads)

/* 状态机变量 */
static int current_step = -1;       // 当前按下的触点索引
static int64_t last_trigger_time = 0; // 上一个触点触发的时间戳

#define STACK_SIZE 1024
#define PRIORITY 7

/* ==============================================
 * 核心逻辑：高容错时序与方向判断
 * ============================================== */
void touch_slider_thread(void) {
    int active_pad = -1;

    while (1) {
        active_pad = -1;

        /* 1. 扫描哪个引脚被触发了 */
        for (int i = 0; i < NUM_PADS; i++) {
            if (gpio_pin_get_dt(&pads[i]) == 1) {
                active_pad = i;
                break; 
            }
        }

        /* 2. 状态机判断滑动方向和速度 */
        if (active_pad != -1 && active_pad != current_step) {
            int64_t current_time = k_uptime_get(); 

            /* 只要有上一个状态，且发生了位置移动（即使跳过了坏引脚） */
            if (current_step != -1) {
                int64_t time_diff = current_time - last_trigger_time;
                int step_diff = active_pad - current_step; // 正数说明向下，负数说明向上
                int base_speed = 1;

                /* 基础速度计算 */
                if (time_diff < 30) {
                    base_speed = 5; // 极快
                } else if (time_diff < 80) {
                    base_speed = 3; // 较快
                } else {
                    base_speed = 1; // 正常
                }

                /* 
                 * 核心容错修正：如果跨越了坏引脚（比如直接从 0 滑到 2），
                 * 步幅倍率会增大，保证滚轮反馈不丢失！
                 */
                int actual_scroll = base_speed * abs(step_diff);

                if (step_diff > 0) {
                    /* 向下滑动 */
                    input_report_rel(NULL, INPUT_REL_WHEEL, -actual_scroll, true, K_NO_WAIT);
                    LOG_INF("Scroll DOWN, Scroll: %d, TimeDiff: %lld", actual_scroll, time_diff);
                } else if (step_diff < 0) {
                    /* 向上滑动 */
                    input_report_rel(NULL, INPUT_REL_WHEEL, actual_scroll, true, K_NO_WAIT);
                    LOG_INF("Scroll UP, Scroll: %d, TimeDiff: %lld", actual_scroll, time_diff);
                }
            }

            /* 更新状态 */
            current_step = active_pad;
            last_trigger_time = current_time;
        } 
        else if (active_pad == -1) {
            /* 手指离开，重置状态机，防止下一次触摸误判为滑动 */
            current_step = -1;
        }

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
        
        /* 配置为输入模式。由于我们在数组定义里写了 GPIO_PULL_DOWN，这里会自动应用下拉 */
        int err = gpio_pin_configure_dt(&pads[i], GPIO_INPUT);
        if (err != 0) {
            LOG_ERR("Failed to configure GPIO pad %d (err: %d)", i, err);
            return err;
        }
    }
    LOG_INF("Custom touch slider initialized successfully.");
    return 0;
}

SYS_INIT(touch_slider_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

K_THREAD_DEFINE(touch_slider_tid, STACK_SIZE,
                (k_thread_entry_t)touch_slider_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);

#endif /* DT_NODE_HAS_STATUS */
