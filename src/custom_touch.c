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
 * 物理顺序：
 *
 *   [0] 最上
 *   [1]
 *   [2]
 *   [3] 最下
 *
 *
 * 向下滑：
 *
 *   0 -> 1 -> 2 -> 3
 *
 *   = 鼠标向下滚
 *
 *
 * 向上滑：
 *
 *   3 -> 2 -> 1 -> 0
 *
 *   = 鼠标向上滚
 *
 * ============================================================
 */


/* ============================================================
 * 1. 四个触摸 GPIO
 * ============================================================
 */

static const struct gpio_dt_spec pads[] = {

    /* [0] 最上方 */
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

    /* [3] 最下方 */
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

/*
 * 连续两次检测到相同状态才确认。
 *
 * 20ms × 2 = 40ms
 */
#define TOUCH_STABLE_COUNT 2


/*
 * 每跨过一个触摸区域产生几个滚轮单位。
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
 * 去抖状态
 */
static int candidate_pad = -1;
static int candidate_count = 0;


/* ============================================================
 * 4. 使用现有 joystick device 作为 Input Event 来源
 * ============================================================
 *
 * Pad15.overlay 中已经有：
 *
 *     joystick: analog_input_0
 *
 * 并且已经有：
 *
 *     joystick_listener {
 *         compatible = "zmk,input-listener";
 *         device = <&joystick>;
 *     };
 *
 * 因此这里直接使用 joystick 这个合法的 input device。
 */

static const struct device *touch_input_device =
    DEVICE_DT_GET(DT_NODELABEL(joystick));


/* ============================================================
 * 5. 发送一个滚轮事件
 * ============================================================
 */

static void send_scroll(int direction)
{
    if (direction == 0) {
        return;
    }

    for (int i = 0; i < SCROLL_TICKS_PER_STEP; i++) {

        /*
         * 不再使用：
         *
         *     input_report_rel(NULL, ...)
         *
         * 必须提供一个有效的 input device。
         */

        input_report_rel(
            touch_input_device,
            INPUT_REL_WHEEL,
            direction,
            true,
            K_NO_WAIT
        );

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
 * 6. 读取触摸位置
 * ============================================================
 *
 * 返回：
 *
 *   -1 = 没有触摸
 *    0 = 最上
 *    1 = 次上
 *    2 = 次下
 *    3 = 最下
 *
 * 如果两个相邻触摸通道同时有效，
 * 则取平均位置。
 */

static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;


    for (int i = 0; i < NUM_PADS; i++) {

        int state = gpio_pin_get_dt(&pads[i]);


        /*
         * 读取失败时直接忽略这个 GPIO。
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
     * 没有任何触摸。
     */
    if (active_count == 0) {
        return -1;
    }


    /*
     * 一个触摸区域有效。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个区域同时有效：
     *
     * 计算平均位置。
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
 * 7. 处理触摸位置变化
 * ============================================================
 */

static void process_touch_position(int new_pad)
{
    /*
     * --------------------------------------------------------
     * 松手
     * --------------------------------------------------------
     */

    if (new_pad < 0) {

        current_pad = -1;

        return;
    }


    /*
     * --------------------------------------------------------
     * 第一次触摸
     * --------------------------------------------------------
     *
     * 只建立起点，不滚动。
     */

    if (current_pad == -1) {

        current_pad = new_pad;

        return;
    }


    /*
     * --------------------------------------------------------
     * 位置没有变化
     * --------------------------------------------------------
     */

    if (new_pad == current_pad) {
        return;
    }


    /*
     * --------------------------------------------------------
     * 计算移动距离
     * --------------------------------------------------------
     */

    int delta =
        new_pad - current_pad;


    /*
     * --------------------------------------------------------
     * 向下滑
     *
     * 0 -> 1
     * 1 -> 2
     * 2 -> 3
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
     * 向上滑
     *
     * 3 -> 2
     * 2 -> 1
     * 1 -> 0
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
    while (1) {

        /*
         * 读取触摸位置。
         */
        int detected_pad =
            read_touch_position();


        /*
         * ----------------------------------------------------
         * 去抖
         * ----------------------------------------------------
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


        /*
         * 50Hz 扫描。
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
    /*
     * 检查 joystick device。
     */
    if (!device_is_ready(touch_input_device)) {
        return -ENODEV;
    }


    /*
     * 初始化四个 GPIO。
     */
    for (int i = 0; i < NUM_PADS; i++) {

        if (!gpio_is_ready_dt(&pads[i])) {
            return -ENODEV;
        }


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
