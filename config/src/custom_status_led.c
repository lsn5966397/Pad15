#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>

/* 定义灯带节点和数量 */
#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define NUM_PIXELS 16
#define STATUS_LED_IDX 15 // 第 16 个灯（数组索引从 0 开始）

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_PIXELS];

/* 定义全局状态变量 */
static uint8_t current_layer = 0;
static uint8_t battery_level = 100;
static bool is_blinking = false;

/* 声明一个延时工作队列（用于处理 1 秒后熄灭和闪烁） */
struct k_work_delayable status_led_work;

/* =======================================
 * 核心执行逻辑：刷新灯带状态
 * ======================================= */
static void update_status_led(struct k_work *work) {
    if (!device_is_ready(led_strip)) {
        return;
    }

    // 1. 低电量优先处理 (例如低于 20% 快速闪烁红灯)
    if (battery_level < 20) {
        if (is_blinking) {
            pixels[STATUS_LED_IDX].r = 0x00; // 灭
        } else {
            pixels[STATUS_LED_IDX].r = 0xFF; // 纯红
            pixels[STATUS_LED_IDX].g = 0x00;
            pixels[STATUS_LED_IDX].b = 0x00;
        }
        is_blinking = !is_blinking;
        // 每 300 毫秒执行一次，实现快闪
        k_work_reschedule(&status_led_work, K_MSEC(300)); 
        led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
        return;
    }

    // 2. 正常切层显示逻辑
    // 默认先全部清零（熄灭）
    pixels[STATUS_LED_IDX].r = 0;
    pixels[STATUS_LED_IDX].g = 0;
    pixels[STATUS_LED_IDX].b = 0;

    // 如果 is_blinking 为 true，说明是刚刚切层，我们需要亮起颜色
    if (is_blinking) {
        switch (current_layer) {
            case 0: // Base 层：白色
                pixels[STATUS_LED_IDX].r = 0x80;
                pixels[STATUS_LED_IDX].g = 0x80;
                pixels[STATUS_LED_IDX].b = 0x80;
                break;
            case 1: // 第 1 层：蓝色
                pixels[STATUS_LED_IDX].r = 0x00;
                pixels[STATUS_LED_IDX].g = 0x00;
                pixels[STATUS_LED_IDX].b = 0xFF;
                break;
            case 2: // 第 2 层：绿色
                pixels[STATUS_LED_IDX].r = 0x00;
                pixels[STATUS_LED_IDX].g = 0xFF;
                pixels[STATUS_LED_IDX].b = 0x00;
                break;
            default: // 其他层：紫色
                pixels[STATUS_LED_IDX].r = 0xFF;
                pixels[STATUS_LED_IDX].g = 0x00;
                pixels[STATUS_LED_IDX].b = 0xFF;
                break;
        }
        is_blinking = false; // 标记下次执行时熄灭
        
        // 1秒后重新执行这个函数，将其熄灭
        k_work_reschedule(&status_led_work, K_SECONDS(1)); 
    }

    // 将数据发送给硬件
    led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
}

/* =======================================
 * 事件监听器：层切换
 * ======================================= */
static int layer_status_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev && ev->state) { 
        current_layer = ev->layer;
        
        // 触发工作队列，点亮灯光
        is_blinking = true; 
        k_work_reschedule(&status_led_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
// 注册监听器
ZMK_LISTENER(layer_status, layer_status_listener);
ZMK_SUBSCRIPTION(layer_status, zmk_layer_state_changed);

/* =======================================
 * 事件监听器：电池状态
 * ======================================= */
static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
        
        // 如果电量低于20%，立刻唤醒工作队列开始闪烁
        if (battery_level < 20) {
            k_work_reschedule(&status_led_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
// 注册监听器
ZMK_LISTENER(battery_status, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status, zmk_battery_state_changed);

/* =======================================
 * 系统初始化
 * ======================================= */
static int custom_status_led_init(const struct device *dev) {
    // 初始化延时工作队列
    k_work_init_delayable(&status_led_work, update_status_led);
    return 0;
}
// 设定初始化优先级，确保在设备树解析后执行
SYS_INIT(custom_status_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
