#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>

LOG_MODULE_REGISTER(custom_touch_slider, LOG_LEVEL_INF);

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
 * 工作方式：
 *
 *   0 -> 1 -> 2 -> 3    页面向下滚
 *   3 -> 2 -> 1 -> 0    页面向上滚
 *
 * 第一次触摸只建立起始位置，不立即滚动。
 *
 * 每跨过一个触摸区域，就产生一个鼠标滚轮 tick。
 *
 * 例如：
 *
 *   触摸 0
 *      ↓
 *   触摸 1       -> Scroll Down 1
 *      ↓
 *   触摸 2       -> Scroll Down 1
 *      ↓
 *   触摸 3       -> Scroll Down 1
 *
 * 反方向同理。
 *
 * ============================================================
 */


/* ============================================================
 * 1. GPIO 定义
 * ============================================================
 *
 * 必须按照“从上到下”的物理顺序排列。
 *
 * [0] P1.00
 * [1] P0.11
 * [2] P1.04
 * [3] P1.06
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
 * 扫描周期。
 *
 * 20 ms = 50 Hz
 *
 * 对于这种简单的四通道触摸区域来说已经足够。
 */
#define TOUCH_SCAN_INTERVAL_MS 20


/*
 * 连续多少次检测到相同状态后才确认。
 *
 * 2 次 × 20ms = 40ms
 *
 * 可以有效过滤触摸芯片边缘跳变。
 */
#define TOUCH_STABLE_COUNT 2


/*
 * 一次触摸区域移动，产生多少个滚轮 tick。
 *
 * 1 = 最自然、最保守。
 *
 * 后面觉得滚得慢，可以改成 2。
 */
#define SCROLL_TICKS_PER_STEP 1


#define STACK_SIZE 1024
#define PRIORITY 7


/* ============================================================
 * 3. 当前触摸状态
 * ============================================================
 *
 * -1 = 当前没有触摸
 *  0 = 最上
 *  1 = 次上
 *  2 = 次下
 *  3 = 最下
 */

static int current_pad = -1;


/*
 * 候选状态。
 *
 * 用来做简单的软件去抖。
 */
static int candidate_pad = -1;
static int candidate_count = 0;


/* ============================================================
 * 4. 读取触摸区域
 * ============================================================
 *
 * 返回：
 *
 *  -1  = 没有触摸
 *   0~3 = 触摸位置
 *
 * 正常情况下应该只有一个 GPIO 为高。
 *
 * 如果因为电容触摸过渡导致多个 GPIO 同时为高，
 * 这里采用“多个激活区域的平均位置”作为当前区域。
 *
 * 例如：
 *
 *   0 + 1 同时激活 -> 认为接近 1
 *   1 + 2 同时激活 -> 认为接近 2
 *
 * 这样可以减少触摸滑动过程中跳变的问题。
 */
static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;

    for (int i = 0; i < NUM_PADS; i++) {

        int state = gpio_pin_get_dt(&pads[i]);

        if (state < 0) {
            LOG_ERR(
                "Failed to read touch pad %d, error=%d",
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
     * 只有一个触摸区域。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个触摸区域同时激活。
     *
     * 使用平均位置。
     *
     * +0.5 的效果通过整数除法实现四舍五入。
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
 * 5. 发送鼠标滚轮
 * ============================================================
 *
 * 根据你的原始代码约定：
 *
 *   +1 = 向上滚
 *   -1 = 向下滚
 *
 * 所以：
 *
 *   pad 3 -> pad 2
 *   是向上滑
 *   => +1
 *
 *   pad 0 -> pad 1
 *   是向下滑
 *   => -1
 */
static void send_scroll(int direction)
{
    if (direction == 0) {
        return;
    }


    for (int i = 0; i < SCROLL_TICKS_PER_STEP; i++) {

        /*
         * 设置鼠标滚轮 Y。
         *
         * X = 0
         * Y = direction
         */
        zmk_hid_mouse_scroll_set(0, direction);


        /*
         * 立即发送当前鼠标 HID report。
         *
         * 这个 API 同时支持 USB / BLE endpoint。
         */
        int err = zmk_endpoint_send_mouse_report();

        if (err != 0) {
            LOG_ERR(
                "Failed to send mouse scroll report: %d",
                err
            );
        }


        /*
         * 发送完成后必须清零。
         *
         * 否则下一次发送可能继续带着上一次的滚轮值。
         */
        zmk_hid_mouse_scroll_set(0, 0);


        /*
         * 如果未来把：
         *
         * SCROLL_TICKS_PER_STEP
         *
         * 调得比较大，这个小延时可以让滚动更加自然。
         */
        if (i + 1 < SCROLL_TICKS_PER_STEP) {
            k_msleep(2);
        }
    }
}


/* ============================================================
 * 6. 根据位置变化处理滑动
 * ============================================================
 */
static void process_touch_position(int new_pad)
{
    /*
     * --------------------------------------------------------
     * 情况 A：
     * 没有触摸
     * --------------------------------------------------------
     */
    if (new_pad < 0) {

        if (current_pad != -1) {
            LOG_INF(
                "Touch released (pad %d)",
                current_pad
            );
        }

        current_pad = -1;

        return;
    }


    /*
     * --------------------------------------------------------
     * 情况 B：
     * 当前还没有建立滑动起点
     * --------------------------------------------------------
     *
     * 第一次碰到触摸板：
     *
     * 只记录位置。
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
     * 情况 C：
     * 位置没变化
     * --------------------------------------------------------
     */
    if (new_pad == current_pad) {
        return;
    }


    /*
     * --------------------------------------------------------
     * 情况 D：
     * 位置发生变化
     * --------------------------------------------------------
     */

    int delta = new_pad - current_pad;


    LOG_INF(
        "Touch moved: %d -> %d",
        current_pad,
        new_pad
    );


    /*
     * 向下移动：
     *
     * 0 -> 1
     * 1 -> 2
     * 2 -> 3
     *
     * 页面向下滚。
     *
     * 根据你的原始定义：
     *
     * Scroll DOWN = -1
     */
    if (delta > 0) {

        /*
         * 如果一次跳过多个区域，例如：
         *
         * 0 -> 3
         *
         * 那么依次发送：
         *
         * -1
         * -1
         * -1
         */
        for (int i = 0; i < delta; i++) {

            send_scroll(-1);

            LOG_INF(
                "Action: Scroll DOWN"
            );
        }
    }


    /*
     * 向上移动：
     *
     * 3 -> 2
     * 2 -> 1
     * 1 -> 0
     *
     * 页面向上滚。
     *
     * Scroll UP = +1
     */
    else {

        for (int i = 0; i < -delta; i++) {

            send_scroll(+1);

            LOG_INF(
                "Action: Scroll UP"
            );
        }
    }


    /*
     * 最后更新当前位置。
     */
    current_pad = new_pad;
}


/* ============================================================
 * 7. 触摸扫描线程
 * ============================================================
 */
static void touch_slider_thread(void)
{
    LOG_INF(
        "========================================"
    );

    LOG_INF(
        "Pad15 Touch Wheel Started"
    );

    LOG_INF(
        "Pads: P1.00 / P0.11 / P1.04 / P1.06"
    );

    LOG_INF(
        "========================================"
    );


    while (1) {

        /*
         * 读取当前触摸位置。
         */
        int detected_pad = read_touch_position();


        /*
         * ----------------------------------------------------
         * 去抖处理
         * ----------------------------------------------------
         */

        if (detected_pad != candidate_pad) {

            /*
             * 新状态出现。
             *
             * 先记录为候选。
             */
            candidate_pad = detected_pad;

            candidate_count = 1;
        }
        else {

            /*
             * 状态连续保持。
             */
            candidate_count++;
        }


        /*
         * 达到稳定次数以后，
         * 才把候选状态交给主状态机。
         */
        if (candidate_count >= TOUCH_STABLE_COUNT) {

            process_touch_position(candidate_pad);

            /*
             * 防止计数无限增长。
             */
            candidate_count = TOUCH_STABLE_COUNT;
        }


        /*
         * 50 Hz 扫描。
         */
        k_msleep(TOUCH_SCAN_INTERVAL_MS);
    }
}


/* ============================================================
 * 8. GPIO 初始化
 * ============================================================
 */
static int touch_slider_init(void)
{
    LOG_INF(
        "Initializing Pad15 touch GPIO..."
    );


    for (int i = 0; i < NUM_PADS; i++) {

        /*
         * 检查 GPIO controller。
         */
        if (!gpio_is_ready_dt(&pads[i])) {

            LOG_ERR(
                "Touch GPIO %d is not ready",
                i
            );

            return -ENODEV;
        }


        /*
         * 设置为输入。
         *
         * dt_flags 中已经包含：
         *
         * GPIO_ACTIVE_HIGH
         * GPIO_PULL_DOWN
         */
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
        "All touch GPIOs initialized successfully."
    );


    return 0;
}


/* ============================================================
 * 9. Zephyr 初始化
 * ============================================================
 */
SYS_INIT(
    touch_slider_init,
    APPLICATION,
    CONFIG_APPLICATION_INIT_PRIORITY
);


/* ============================================================
 * 10. 创建线程
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
