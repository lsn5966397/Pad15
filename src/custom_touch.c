#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/devicetree.h>
#include <stdint.h>


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
 * 向下：
 *
 *   0 -> 1 -> 2 -> 3
 *
 * 向上：
 *
 *   3 -> 2 -> 1 -> 0
 *
 *
 * 速度感：
 *
 *   手指移动越慢
 *       ↓
 *   滚轮 tick 越少
 *
 *   手指移动越快
 *       ↓
 *   滚轮 tick 越多
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

    /* [1] 次上 */
    {
        .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
        .pin = 11,
        .dt_flags = GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN
    },

    /* [2] 次下 */
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

/*
 * GPIO 扫描周期。
 *
 * 10ms = 100Hz
 *
 * 对速度检测来说比较合适。
 */
#define TOUCH_SCAN_INTERVAL_MS 10


/*
 * 状态确认次数。
 *
 * 1 = 立即响应。
 */
#define TOUCH_STABLE_COUNT 1


/*
 * 最低滚轮速度。
 *
 * 慢速滑动时，每跨过一个触摸区域至少产生 1 tick。
 */
#define MIN_SCROLL_TICKS 1


/*
 * 最大滚轮速度。
 *
 * 快速滑动时，每跨过一个触摸区域最多产生 10 tick。
 */
#define MAX_SCROLL_TICKS 10


/*
 * 时间阈值。
 *
 * 单位：毫秒。
 *
 * 两次触摸位置变化之间的时间越短，
 * 说明手指移动越快。
 *
 * 速度等级：
 *
 * <= 35ms   → 10 tick
 * <= 60ms   → 8 tick
 * <= 90ms   → 6 tick
 * <= 130ms  → 4 tick
 * <= 200ms  → 2 tick
 * > 200ms   → 1 tick
 */
#define SPEED_T1_MS 35
#define SPEED_T2_MS 60
#define SPEED_T3_MS 90
#define SPEED_T4_MS 130
#define SPEED_T5_MS 200


#define STACK_SIZE 1024
#define PRIORITY 7


/* ============================================================
 * 3. 当前触摸状态
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
 * 去抖候选状态
 */
static int candidate_pad = -1;
static int candidate_count = 0;


/*
 * 上一次“有效位置变化”的时间。
 *
 * 用于计算手指移动速度。
 */
static uint32_t last_move_time = 0;


/*
 * 表示是否已经拥有有效的速度测量基准。
 *
 * 第一次触摸没有速度信息，
 * 第二次位置变化以后才开始计算速度。
 */
static bool have_last_move_time = false;


/* ============================================================
 * 4. Input Event 来源
 * ============================================================
 *
 * 使用已经存在的 joystick device。
 *
 * Pad15.overlay 中已有：
 *
 *     joystick: analog_input_0
 *
 *     joystick_listener {
 *         compatible = "zmk,input-listener";
 *         device = <&joystick>;
 *     };
 *
 * 因此这里借用 joystick 作为合法 input event device。
 */

static const struct device *touch_input_device =
    DEVICE_DT_GET(DT_NODELABEL(joystick));


/* ============================================================
 * 5. 根据移动时间计算滚轮速度
 * ============================================================
 *
 * elapsed_ms：
 *
 *     两次触摸区域变化之间经过的时间。
 *
 * 返回：
 *
 *     每跨越一个触摸区域需要发送多少个 wheel tick。
 *
 * elapsed 越小：
 *
 *     手指越快
 *         ↓
 *     tick 越多
 *
 * elapsed 越大：
 *
 *     手指越慢
 *         ↓
 *     tick 越少
 * ============================================================
 */

static int calculate_scroll_ticks(uint32_t elapsed_ms)
{
    /*
     * 极快移动。
     */
    if (elapsed_ms <= SPEED_T1_MS) {
        return 10;
    }


    /*
     * 很快。
     */
    if (elapsed_ms <= SPEED_T2_MS) {
        return 8;
    }


    /*
     * 较快。
     */
    if (elapsed_ms <= SPEED_T3_MS) {
        return 6;
    }


    /*
     * 中等速度。
     */
    if (elapsed_ms <= SPEED_T4_MS) {
        return 4;
    }


    /*
     * 较慢。
     */
    if (elapsed_ms <= SPEED_T5_MS) {
        return 2;
    }


    /*
     * 很慢。
     */
    return 1;
}


/* ============================================================
 * 6. 发送滚轮事件
 * ============================================================
 */

static void send_scroll(int direction, int ticks)
{
    /*
     * 防止异常值。
     */
    if (direction == 0) {
        return;
    }


    if (ticks < MIN_SCROLL_TICKS) {
        ticks = MIN_SCROLL_TICKS;
    }


    if (ticks > MAX_SCROLL_TICKS) {
        ticks = MAX_SCROLL_TICKS;
    }


    /*
     * 发送多个 wheel tick。
     */
    for (int i = 0; i < ticks; i++) {

        int err = input_report_rel(
            touch_input_device,
            INPUT_REL_WHEEL,
            direction,
            true,
            K_NO_WAIT
        );


        /*
         * 当前不使用 LOG_*。
         *
         * 避免再次触发之前的 logging 编译问题。
         */
        (void)err;


        /*
         * 多个 tick 之间稍微错开。
         *
         * 这里不能太长，否则快速滑动会产生明显延迟。
         */
        if (i + 1 < ticks) {
            k_msleep(1);
        }
    }
}


/* ============================================================
 * 7. 读取当前触摸位置
 * ============================================================
 */

static int read_touch_position(void)
{
    int active_count = 0;
    int active_sum = 0;


    for (int i = 0; i < NUM_PADS; i++) {

        int state =
            gpio_pin_get_dt(&pads[i]);


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
     * 单个触摸区域。
     */
    if (active_count == 1) {
        return active_sum;
    }


    /*
     * 多个区域同时激活。
     *
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
 * 8. 处理触摸位置变化
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

        /*
         * 一次滑动结束。
         *
         * 下一次触摸重新建立速度基准。
         */
        have_last_move_time = false;

        return;
    }


    /*
     * --------------------------------------------------------
     * 第一次触摸
     * --------------------------------------------------------
     *
     * 不滚动。
     *
     * 这里只记录起点。
     */

    if (current_pad == -1) {

        current_pad = new_pad;

        /*
         * 记录第一次触摸时间。
         *
         * 但这只是建立时间基准，
         * 不马上拿它计算速度。
         */
        last_move_time =
            k_uptime_get_32();

        have_last_move_time = false;

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
     * 当前时间
     * --------------------------------------------------------
     */

    uint32_t now =
        k_uptime_get_32();


    /*
     * --------------------------------------------------------
     * 计算位置变化
     * --------------------------------------------------------
     */

    int delta =
        new_pad - current_pad;


    /*
     * --------------------------------------------------------
     * 计算两次位置变化之间的时间
     * --------------------------------------------------------
     */

    uint32_t elapsed_ms = 2000;


    if (have_last_move_time) {

        elapsed_ms =
            now - last_move_time;
    }


    /*
     * --------------------------------------------------------
     * 第一次真正的位置变化：
     *
     * 没有上一段移动速度。
     *
     * 使用一个比较保守的速度。
     * --------------------------------------------------------
     */

    int scroll_ticks = 2;


    if (have_last_move_time) {

        scroll_ticks =
            calculate_scroll_ticks(elapsed_ms);
    }


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

        /*
         * delta 可能一次跳过多个区域。
         *
         * 例如：
         *
         * 0 -> 3
         *
         * 那么需要：
         *
         * 3 个触摸步 × 当前速度 tick
         */
        for (int i = 0; i < delta; i++) {

            send_scroll(
                -1,
                scroll_ticks
            );
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

            send_scroll(
                +1,
                scroll_ticks
            );
        }
    }


    /*
     * --------------------------------------------------------
     * 更新状态
     * --------------------------------------------------------
     */

    current_pad =
        new_pad;


    /*
     * 当前移动已经成为下一次速度计算的基准。
     */
    last_move_time =
        now;

    have_last_move_time = true;
}


/* ============================================================
 * 9. 主线程
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
         * ====================================================
         * 软件去抖
         * ====================================================
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
         * 状态稳定以后进行处理。
         */
        if (candidate_count >= TOUCH_STABLE_COUNT) {

            process_touch_position(
                candidate_pad
            );


            /*
             * 防止计数无限增加。
             */
            candidate_count =
                TOUCH_STABLE_COUNT;
        }


        /*
         * 100Hz 扫描。
         */
        k_msleep(
            TOUCH_SCAN_INTERVAL_MS
        );
    }
}


/* ============================================================
 * 10. GPIO 初始化
 * ============================================================
 */

static int touch_slider_init(void)
{
    /*
     * 初始化四个触摸 GPIO。
     */
    for (int i = 0; i < NUM_PADS; i++) {

        /*
         * 检查 GPIO controller。
         */
        if (!gpio_is_ready_dt(&pads[i])) {
            return -ENODEV;
        }


        /*
         * 配置为输入。
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
 * 11. Zephyr 初始化
 * ============================================================
 */

SYS_INIT(
    touch_slider_init,
    APPLICATION,
    CONFIG_APPLICATION_INIT_PRIORITY
);


/* ============================================================
 * 12. 创建线程
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
