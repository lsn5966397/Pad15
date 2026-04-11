#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <stdlib.h> // 提供 abs()
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define NUM_PIXELS 16
#define STATUS_LED_IDX 15
#define MAX_EFFECTS 3

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_PIXELS];

static uint8_t current_layer = 0;
static uint8_t battery_level = 100;
static int status_display_frames = 0; 
static uint8_t current_effect = 0; 

// ==========================================
// 1.  uint8_t 二维坐标表
// ==========================================
struct led_coord {
    uint8_t x;
    uint8_t y;
};

static const struct led_coord coords[NUM_PIXELS] = {
    {0, 0},  {10, 0},  {20, 0},     // 第一排
    {0, 10}, {10, 10}, {20, 10},    // 第二排
    {0, 20}, {10, 20}, {20, 20},    // 第三排
    {0, 30}, {10, 30}, {20, 30},    // 第四排
    {0, 40}, {10, 40}, {20, 40},    // 第五排
    {30, 20}                        // 第 16 颗 (状态灯)
};

// HSV 转 RGB 算法
static struct led_rgb wheel(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85) return (struct led_rgb){255 - pos * 3, 0, pos * 3};
    else if (pos < 170) { pos -= 85; return (struct led_rgb){0, pos * 3, 255 - pos * 3}; }
    else { pos -= 170; return (struct led_rgb){pos * 3, 255 - pos * 3, 0}; }
}

// ==========================================
// 2. 核心动画渲染线程
// ==========================================
void custom_led_thread_main(void) {
    if (!device_is_ready(led_strip)) return;

    uint32_t tick = 0; 

    while (1) {
        // --- 渲染前 15 颗矩阵灯 ---
        for (int i = 0; i < STATUS_LED_IDX; i++) {
            uint8_t cx = coords[i].x;
            uint8_t cy = coords[i].y;

            if (current_effect == 0) {
                // 【特效 0：X轴彩虹波浪】
                // 因为 cx 只有 0, 10, 20, 30... 直接加到 tick 上就形成极佳的色差阶梯
                pixels[i] = wheel((uint8_t)(tick * 2 + (cx * 2)));
            } 
            else if (current_effect == 1) {
                // 【特效 1：Y轴雷达扫描线】
                // 坐标变小了，扫描参数同步调整：循环范围 0~60
                int scanline_y = (tick % 60); 
                int distance = abs(scanline_y - cy);
                
                if (distance < 12) { // 光晕宽度控制在 12
                    uint8_t intensity = 255 - (distance * 20); // 距离越近越亮
                    pixels[i] = (struct led_rgb){0, intensity, intensity / 2}; // 青蓝色
                } else {
                    pixels[i] = (struct led_rgb){0, 0, 0}; 
                }
            }
            else {
                // 【特效 2：全部熄灭】
                pixels[i] = (struct led_rgb){0, 0, 0};
            }
        }

        // --- 渲染第 16 颗状态灯 (优先级不变) ---
        if (battery_level < 20) {
            if (tick % 30 < 15) pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x00, 0x00};
            else pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0};
        } 
        else if (status_display_frames > 0) {
            switch (current_layer) {
                case 0: pixels[STATUS_LED_IDX] = (struct led_rgb){0x80, 0x80, 0x80}; break; 
                case 1: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0x00, 0xFF}; break; 
                case 2: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0xFF, 0x00}; break; 
                default: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x00, 0xFF}; break;
            }
            status_display_frames--; 
        } 
        else {
            pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0}; 
        }

        led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
        tick++;
        k_sleep(K_MSEC(30)); 
    }
}
K_THREAD_DEFINE(custom_led_tid, 1024, custom_led_thread_main, NULL, NULL, NULL, 7, 0, 0);

// ==========================================
// 3. 事件监听器
// ==========================================
static int keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state) {
        // F15 的键码 0x6A
        if (ev->keycode == 0x6A) { 
            current_effect++;
            if (current_effect >= MAX_EFFECTS) current_effect = 0;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(keycode_status, keycode_listener);
ZMK_SUBSCRIPTION(keycode_status, zmk_keycode_state_changed);

static int layer_status_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev && ev->state) { 
        current_layer = ev->layer;
        status_display_frames = 66; 
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(layer_status, layer_status_listener);
ZMK_SUBSCRIPTION(layer_status, zmk_layer_state_changed);

static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(battery_status, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status, zmk_battery_state_changed);
