#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>

/*
 * ============================================================
 * Pad15 四通道电容触摸滚轮
 * ============================================================
 *
 * 触摸板物理顺序：
 *
 *     [0] 最上
 *     [1] 次上
 *     [2] 次下
 *     [3] 最下
 *
 * 手指向下滑：
 *
 *     0 -> 1 -> 2 -> 3
 *         ↓
 *       页面向下滚
 *
 * 手指向上滑：
 *
 *     3 -> 2 -> 1 -> 0
 *         ↓
 *       页面向上滚
 *
 * 本实现不直接调用 ZMK HID API。
 *
 * 而是借用现有 joystick input device 作为合法的
 * Zephyr input event 来源：
 *
 *     custom_touch
 *          ↓
 *     input_report_rel(joystick, ...)
 *          ↓
 *     joystick_listener
 *          ↓
 *     ZMK pointing
 *          ↓
 *     USB / BLE HID Mouse
 *
 * 这样可以避免不同 ZMK 版本之间 HID API 名称变化。
 */


/* ============================================================
 * 1. 四个触摸 GPIO
 * ============================================================
 *
 * 按照“从上到下”的物理顺序排列。
 */

static const struct gpio_dt_spec pads[] = {
    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 0,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
        .pin = 11,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 4,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 6,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },
};

#define NUM_PADS ARRAY_SIZE(pads)


/* ============================================================
 * 2. 参数
 * ============================================================
 */

/*
 * GPIO 扫描周期：
 *
 * 20 ms = 50 Hz
 */
#define TOUCH_SCAN_INTERVAL_MS 20


/*
 * 简单去抖：
 *
 * 连续两次得到相同状态以后才认为状态稳定。
 *
 * 2 × 20ms = 40ms
 */
#define TOUCH_STABLE_COUNT 2


/*
 * 每跨过一个触摸区域发送几个滚轮单位。
 *
 * 初始设置为 1。
 *
 * 后面如果觉得太慢，可以改成 2 或 3。
 */
#define SCROLL_TICKS_PER_STEP 1


#define STACK_SIZE 1024
#define PRIORITY 7


/* ============================================================
 * 3. 当前状态
 * ============================================================
 *
 * -1 = 没有触摸
 *
 *  0 = 最上
 *  1 = 次上
 *  2 = 次下
 *  3 = 最下
 */

static int current_pad = -1;


/*
 * 去抖候选状态。
 */
static int candidate_pad = -1;
static int candidate_count = 0;


/* ============================================================
 * 4. 使用现有 joystick 设备作为 input event 来源
 * ============================================================
 *
 * 你的 Pad15.overlay 已经有：
 *
 *     joystick: analog_input_0
 *
 * 以及：
 *
 *     joystick_listener {
 *         compatible = "zmk,input-listener";
 *         device = <&joystick>;
 *     };
 *
 * 所以我们直接让触摸板产生“来自 joystick device”的
 * INPUT_REL_WHEEL 事件。
 *
 * 这样 ZMK 会正常把事件送入 pointing HID。
 */

static const struct device *touch_input_device =
    DEVICE_DT_GET(DT_NODELABEL(joystick));


/* ============================================================
 * 5. 发送一个鼠标滚轮事件
 * ============================================================
 *
 * 注意：
 *
 * 你的原始代码定义：
 *
 *     +1 = Scroll UP
 *     -1 = Scroll DOWN
 *
 * 我保持这个方向不变。
 */

static void send_scroll(int direction)
{
    if (direction == 0) {
        return;
    }


    for (int i = 0; i < SCROLL_TICKS_PER_STEP; i++) {

        /*
         * 注意这里最重要的变化：
         *
         * 不能再写：
         *
         *     input_report_rel(NULL, ...)
         *
         * 因为 ZMK input listener 会拒绝 dev == NULL。
         *
         * 现在使用：
         *
         *     touch_input_device
         *
         * 它就是你的 joystick device。
         */
        int err = input_report_rel(
            touch_input_device,
            INPUT_REL_WHEEL,
            direction,
            true,
            K_NO_WAIT
        );


        if (err != 0) {
            LOG_ERR(
                "Failed to send scroll event: %d",
                err
            );
        }
        else {
            if (direction > 0) {
                LOG_INF("Scroll UP (+%d)", direction);
            }
            else {
                LOG_INF("Scroll DOWN (%d)", direction);
            }
        }


        /*
         * 如果以后一个区域对应多个 tick，
         * 稍微错开事件。
         */
        if (i + 1 < SCROLL_TICKS_PER_STEP) {
            k_msleep(2);
        }
    }
}


/* ============================================================
 * 6. 读取四个触摸通道
 * ============================================================
 *
 * 返回：
 *
 *   -1 : 没有触摸
 *    0 : 触摸最上方
 *    1 : 触摸次上方
 *    2 : 触摸次下方
 *    3 : 触摸最下方
 *
 * 正常情况下 AI04 应该只有一个输出有效。
 *
 * 如果滑动过程中出现两个相邻通道同时为高：
 *
 *     0 + 1
 *
 * 或：
 *
 *     1 + 2
 *
 * 或：
 *
 *     2 + 3
 *
 * 则取平均位置，减少边界跳变。
 */

static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;


    for (int i = 0; i < NUM_PADS; i++) {

        int state = gpio_pin_get_dt(&pads[i]);


        /*
         * GPIO 读取错误。
         */
        if (state < 0) {
            LOG_ERR(
                "Failed to read touch pad %d: %d",
                i,
                state
            );

            continue;
        }


        if (state > 0) {
            active_count++;
            active_sum += i;
        }
    }


    /*
     * 没有任何触摸。
     */
    if (active_count == 0) {
        return -1;
    }


    /*
     * 只有一个通道有效。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个通道同时有效。
     *
     * 使用平均位置。
     */
    int position =
        (active_sum + active_count / 2) / active_count;


    /*
     * 安全限制。
     */
    if (position < 0) {
        position = 0;
    }

    if (position >= NUM_PADS) {
        position = NUM_PADS - 1;
    }


    return position;
}


/* ============================================================
 * 7. 处理触摸位置变化
 * ============================================================
 */

static void process_touch_position(int new_pad)
{
    /*
     * --------------------------------------------------------
     * A. 松开
     * --------------------------------------------------------
     */

    if (new_pad < 0) {

        if (current_pad != -1) {
            LOG_INF(
                "Touch released from pad %d",
                current_pad
            );
        }

        current_pad = -1;

        return;
    }


    /*
     * --------------------------------------------------------
     * B. 第一次触摸
     * --------------------------------------------------------
     *
     * 第一次碰到触摸板只建立起点。
     *
     * 不滚动。
     */

    if (current_pad == -1) {

        current_pad = new_pad;

        LOG_INF(
            "Touch started at pad %d",
            current_pad
        );

        return;
    }


    /*
     * --------------------------------------------------------
     * C. 位置没有变化
     * --------------------------------------------------------
     */

    if (new_pad == current_pad) {
        return;
    }


    /*
     * --------------------------------------------------------
     * D. 计算位移
     * --------------------------------------------------------
     */

    int delta = new_pad - current_pad;


    LOG_INF(
        "Touch moved: %d -> %d",
        current_pad,
        new_pad
    );


    /*
     * --------------------------------------------------------
     * 向下移动
     *
     *     0 -> 1
     *     1 -> 2
     *     2 -> 3
     *
     * 页面向下滚。
     *
     * Scroll DOWN = -1
     * --------------------------------------------------------
     */

    if (delta > 0) {

        for (int i = 0; i < delta; i++) {

            send_scroll(-1);
        }
    }


    /*
     * --------------------------------------------------------
     * 向上移动
     *
     *     3 -> 2
     *     2 -> 1
     *     1 -> 0
     *
     * 页面向上滚。
     *
     * Scroll UP = +1
     * --------------------------------------------------------
     */

    else {

        for (int i = 0; i < -delta; i++) {

            send_scroll(+1);
        }
    }


    /*
     * 更新当前位置。
     */
    current_pad = new_pad;
}


/* ============================================================
 * 8. 主扫描线程
 * ============================================================
 */

static void touch_slider_thread(void)
{
    LOG_INF("========================================");
    LOG_INF("Pad15 Touch Wheel Started");
    LOG_INF("Pads: P1.00 / P0.11 / P1.04 / P1.06");
    LOG_INF("========================================");


    while (1) {

        /*
         * 读取当前触摸位置。
         */
        int detected_pad =
            read_touch_position();


        /*
         * ----------------------------------------------------
         * 软件去抖
         * ----------------------------------------------------
         */

        if (detected_pad != candidate_pad) {

            /*
             * 新状态出现。
             */
            candidate_pad = detected_pad;
            candidate_count = 1;
        }
        else {

            /*
             * 状态保持。
             */
            candidate_count++;
        }


        /*
         * 状态稳定以后交给主状态机。
         */
        if (candidate_count >= TOUCH_STABLE_COUNT) {

            process_touch_position(candidate_pad);

            /*
             * 防止无限增加。
             */
            candidate_count =
                TOUCH_STABLE_COUNT;
        }


        /*
         * 50 Hz 扫描。
         */
        k_msleep(
            TOUCH_SCAN_INTERVAL_MS
        );
    }
}


/* ============================================================
 * 9. GPIO 初始化
 * ============================================================
 */

static int touch_slider_init(void)
{
    LOG_INF(
        "Initializing Pad15 touch GPIO..."
    );


    /*
     * 检查 joystick input device。
     */
    if (!device_is_ready(touch_input_device)) {

        LOG_ERR(
            "Joystick input device is not ready"
        );

        return -ENODEV;
    }


    /*
     * 初始化四个触摸 GPIO。
     */
    for (int i = 0; i < NUM_PADS; i++) {

        if (!gpio_is_ready_dt(&pads[i])) {

            LOG_ERR(
                "Touch GPIO %d is not ready",
                i
            );

            return -ENODEV;
        }


        int err =
            gpio_pin_configure_dt(
                &pads[i],
                GPIO_INPUT
            );


        if (err != 0) {

            LOG_ERR(
                "Failed to configure touch GPIO %d: %d",
                i,
                err
            );

            return err;
        }


        LOG_INF(
            "Touch GPIO %d initialized",
            i
        );
    }


    LOG_INF(
        "Pad15 touch GPIO initialization complete"
    );


    return 0;
}


/* ============================================================
 * 10. Zephyr 初始化
 * ============================================================
 */

SYS_INIT(
    touch_slider_init,
    APPLICATION,
    CONFIG_APPLICATION_INIT_PRIORITY
);


/* ============================================================
 * 11. 创建触摸扫描线程
 * ============================================================
 */

K_THREAD_DEFINE(
    touch_slider_tid,
    STACK_SIZE,
    touch_slider_thread,
    NULL,
    NULL,
    NULL,
    PRIORITY,
    0,
    0
);
