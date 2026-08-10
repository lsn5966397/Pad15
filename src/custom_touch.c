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

/* 记录上一次扫描时每个引脚的状态，用于边缘检测（防止按住时无限触发） */
static int prev_state[4] = {0, 0, 0, 0};

#define STACK_SIZE 1024
#define PRIORITY 7

/* ==============================================
 * 极简测试逻辑：点按即触发
 * ============================================== */
void touch_test_thread(void) {
    LOG_INF("--- Touch Simple Test Started ---");

    while (1) {
        for (int i = 0; i < NUM_PADS; i++) {
            /* 1. 读取当前引脚电平 */
            int current_state = gpio_pin_get_dt(&pads[i]);

            /* 2. 只有当状态从 0(没摸) 变成 1(摸了) 时，才执行动作 */
            if (current_state == 1 && prev_state[i] == 0) {
                
                LOG_INF(">>> Pad [%d] TOUCHED! <<<", i);

                /* 上边两个按钮: Scroll UP */
                if (i == 0 || i == 1) {
                    input_report_rel(NULL, INPUT_REL_WHEEL, 1, true, K_NO_WAIT);
                    LOG_INF("Action: Sent Scroll UP (+1)");
                } 
                /* 下边两个按钮: Scroll DOWN */
                else if (i == 2 || i == 3) {
                    input_report_rel(NULL, INPUT_REL_WHEEL, -1, true, K_NO_WAIT);
                    LOG_INF("Action: Sent Scroll DOWN (-1)");
                }
            }

            /* 3. 更新状态，供下一次循环比对 */
            prev_state[i] = current_state;
        }

        /* 睡眠 20ms，兼顾响应速度与简单的软件防抖 */
        k_msleep(20); 
    }
}

/* ==============================================
 * 初始化过程 (不变)
 * ============================================== */
static int touch_slider_init(void) {
    for (int i = 0; i < NUM_PADS; i++) {
        if (!gpio_is_ready_dt(&pads[i])) {
            LOG_ERR("GPIO pad %d not ready", i);
            return -ENODEV;
        }
        
        int err = gpio_pin_configure_dt(&pads[i], GPIO_INPUT);
        if (err != 0) {
            LOG_ERR("Failed to configure GPIO pad %d (err: %d)", i, err);
            return err;
        }
    }
    LOG_INF("GPIO configured for simple touch test.");
    return 0;
}

SYS_INIT(touch_slider_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

K_THREAD_DEFINE(touch_slider_tid, STACK_SIZE,
                (k_thread_entry_t)touch_test_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);

#endif /* DT_NODE_HAS_STATUS */
