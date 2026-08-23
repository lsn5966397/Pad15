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
 *   [1] 次上
 *   [2] 次下
 *   [3] 最下
 *
 * 向下滑：
 *   0 -> 1 -> 2 -> 3
 *
 * 向上滑：
 *   3 -> 2 -> 1 -> 0
 *
 * 当前实现：
 *   每跨越一个触摸区域 = 1 个鼠标滚轮单位
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
 * 连续两次得到相同状态才确认。
 *
 * 20 ms × 2 = 40 ms
 */
#define TOUCH_STABLE_COUNT 2


/*
 * 每跨越一个触摸区域产生几个滚轮单位。
 *
 * 先固定为 1，确认能正常工作后再调大。
 */
#define SCROLL_TICKS_PER_STEP 1


#define STACK_SIZE 1024
#define PRIORITY 7


/* ============================================================
 * 3. 当前状态
 * ============================================================
 *
 * -1 = 没有触摸
 *  0 = 最上
 *  1 = 次上
 *  2 = 次下
 *  3 = 最下
 */

static int current_pad = -1;


/*
 * 去抖候选状态
 */
static int candidate_pad = -1;
static int candidate_count = 0;


/* ============================================================
 * 4. Input Event 来源
 * ============================================================
 *
 * 你的 Pad15.overlay 已经存在：
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
 * 因此这里使用 &joystick 作为 input event 的 dev。
 *
 * 这样：
 *
 *     custom_touch
 *          ↓
 *     input_report_rel(joystick, ...)
 *          ↓
 *     joystick_listener
 *          ↓
 *     ZMK pointing
 *          ↓
 *     HID mouse wheel
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
         * 不能使用：
         *
         *     input_report_rel(NULL, ...)
         *
         * 因为 ZMK input listener 需要根据 dev
         * 来判断这个事件来自哪个输入设备。
         */

        int err = input_report_rel(
            touch_input_device,
            INPUT_REL_WHEEL,
            direction,
            true,
            K_NO_WAIT
        );

        /*
         * 这里不使用 LOG_*。
         *
         * 你的上一版编译失败就是因为 LOG_ERR()
         * 触发了 __log_level undeclared。
         *
         * 失败时暂时什么都不做，避免引入 logging
         * 依赖。
         */
        (void)err;

        /*
         * 如果以后：
         *
         * SCROLL_TICKS_PER_STEP > 1
         *
         * 则略微错开多个 wheel event。
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
 * 如果多个相邻通道同时有效，则取平均位置。
 */

static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;


    for (int i = 0; i < NUM_PADS; i++) {

        int state = gpio_pin_get_dt(&pads[i]);

        /*
         * GPIO 读取失败：
         * 忽略这个通道。
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
     * 一个通道有效。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个通道同时有效：
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
     * 只记录起点。
     *
     * 不滚动。
     */

    if (current_pad == -1) {
        current_pad = new_pad;
        return;
    }


    /*
     * --------------------------------------------------------
     * 没移动
     * --------------------------------------------------------
     */

    if (new_pad == current_pad) {
        return;
    }


    /*
     * --------------------------------------------------------
     * 计算位移
     * --------------------------------------------------------
     */

    int delta = new_pad - current_pad;


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

            candidate_pad =
                detected_pad;

            candidate_count = 1;
        }
        else {

            candidate_count++;
        }


        /*
         * 状态稳定之后，
         * 才交给触摸状态机。
         */
        if (candidate_count >= TOUCH_STABLE_COUNT) {

            process_touch_position(
                candidate_pad
            );

            /*
             * 防止计数继续增长。
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
    /*
     * 初始化四个触摸 GPIO。
     *
     * 这里不再检查 joystick device 是否 ready。
     *
     * 因为这里使用 joystick 只是作为 Input Event
     * 的设备来源，而不是启动 joystick ADC。
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
