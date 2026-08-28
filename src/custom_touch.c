#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/devicetree.h>


/*
 * ============================================================
 * Pad15 四通道电容触摸滚轮
 * ============================================================
 *
 * [0] 最上
 * [1] 次上
 * [2] 次下
 * [3] 最下
 *
 * 0 -> 1 -> 2 -> 3
 *       ↓
 *    向下滚动
 *
 * 3 -> 2 -> 1 -> 0
 *       ↓
 *    向上滚动
 *
 * ============================================================
 */


/* ============================================================
 * 1. 四个触摸 GPIO
 * ============================================================
 */

static const struct gpio_dt_spec pads[] = {

    /* [0] 最上 */
    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 0,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    /* [1] */
    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
        .pin = 11,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    /* [2] */
    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
        .pin = 4,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    /* [3] 最下 */
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

#define TOUCH_SCAN_INTERVAL_MS 20
#define TOUCH_STABLE_COUNT 2
#define SCROLL_TICKS_PER_STEP 1

#define STACK_SIZE 1024
#define PRIORITY 7


/* ============================================================
 * 3. 当前触摸状态
 * ============================================================
 */

static int current_pad = -1;

static int candidate_pad = -1;
static int candidate_count = 0;


/* ============================================================
 * 4. 使用 joystick 作为 Input Event device
 * ============================================================
 *
 * Pad15.overlay 中：
 *
 *   joystick: analog_input_0
 *
 *   joystick_listener {
 *       compatible = "zmk,input-listener";
 *       device = <&joystick>;
 *   };
 *
 * 因此触摸产生的 INPUT_REL_WHEEL 事件，
 * 可以通过这个合法的 input device 进入 ZMK pointing。
 */

static const struct device *touch_input_device =
    DEVICE_DT_GET(DT_NODELABEL(joystick));


/* ============================================================
 * 5. 发送滚轮事件
 * ============================================================
 */

static void send_scroll(int direction)
{
    if (direction == 0) {
        return;
    }


    for (int i = 0; i < SCROLL_TICKS_PER_STEP; i++) {

        /*
         * 注意：
         *
         * 不能使用 NULL。
         *
         * 必须给 input event 一个有效的 device。
         */
        int err = input_report_rel(
            touch_input_device,
            INPUT_REL_WHEEL,
            direction,
            true,
            K_NO_WAIT
        );


        /*
         * 当前不使用 LOG_*，
         * 避免引入 logging module 相关编译问题。
         */
        (void)err;


        if (i + 1 < SCROLL_TICKS_PER_STEP) {
            k_msleep(2);
        }
    }
}


/* ============================================================
 * 6. 读取触摸位置
 * ============================================================
 */

static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;


    for (int i = 0; i < NUM_PADS; i++) {

        int state = gpio_pin_get_dt(&pads[i]);


        /*
         * GPIO 读取失败。
         */
        if (state < 0) {
            continue;
        }


        if (state > 0) {
            active_count++;
            active_sum += i;
        }
    }


    /*
     * 没有触摸。
     */
    if (active_count == 0) {
        return -1;
    }


    /*
     * 单个触摸通道。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个通道同时有效时，
     * 使用平均位置。
     */
    int position =
        (active_sum + active_count / 2) /
        active_count;


    if (position < 0) {
        position = 0;
    }


    if (position >= NUM_PADS) {
        position = NUM_PADS - 1;
    }


    return position;
}


/* ============================================================
 * 7. 处理触摸位置
 * ============================================================
 */

static void process_touch_position(int new_pad)
{
    /*
     * 松手。
     */
    if (new_pad < 0) {
        current_pad = -1;
        return;
    }


    /*
     * 第一次触摸：
     * 只记录起点。
     */
    if (current_pad == -1) {
        current_pad = new_pad;
        return;
    }


    /*
     * 没有移动。
     */
    if (new_pad == current_pad) {
        return;
    }


    /*
     * 计算位移。
     */
    int delta =
        new_pad - current_pad;


    /*
     * 向下：
     *
     * 0 -> 1
     * 1 -> 2
     * 2 -> 3
     */
    if (delta > 0) {

        for (int i = 0; i < delta; i++) {
            send_scroll(-1);
        }
    }


    /*
     * 向上：
     *
     * 3 -> 2
     * 2 -> 1
     * 1 -> 0
     */
    else {

        for (int i = 0; i < -delta; i++) {
            send_scroll(+1);
        }
    }


    current_pad = new_pad;
}


/* ============================================================
 * 8. 主线程
 * ============================================================
 */

static void touch_slider_thread(void)
{
    while (1) {

        int detected_pad =
            read_touch_position();


        /*
         * 软件去抖。
         */
        if (detected_pad != candidate_pad) {

            candidate_pad =
                detected_pad;

            candidate_count = 1;
        }
        else {

            candidate_count++;
        }


        /*
         * 状态稳定以后处理。
         */
        if (candidate_count >= TOUCH_STABLE_COUNT) {

            process_touch_position(
                candidate_pad
            );

            candidate_count =
                TOUCH_STABLE_COUNT;
        }


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
    for (int i = 0; i < NUM_PADS; i++) {

        /*
         * GPIO controller 是否存在。
         */
        if (!gpio_is_ready_dt(&pads[i])) {
            return -ENODEV;
        }


        /*
         * 设置输入。
         */
        int err =
            gpio_pin_configure_dt(
                &pads[i],
                GPIO_INPUT
            );


        if (err != 0) {
            return err;
        }
    }


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
 * 11. 创建线程
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
