#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <stdlib.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>
#include <zmk/keymap.h>

#define STRIP_NODE DT_NODELABEL(pad15_leds)
#define NUM_PIXELS 16
#define STATUS_LED_IDX 15
#define MAX_EFFECTS 4                      // 【修改】扩展到4种灯效
#define BRIGHTNESS_PERCENT 30              

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_PIXELS];

// 状态全局变量
static uint8_t battery_level = 100;
static int status_display_frames = 66;     
static uint8_t current_effect = 3;         // 【修改】默认开机展示效果3 (ZMK幻彩灯效4)
static bool is_awake = true;
        
struct led_coord {
    uint8_t x;
    uint8_t y;
};

static const struct led_coord coords[NUM_PIXELS] = {
    {0, 0},  {10, 0},  {20, 0},
    {0, 10}, {10, 10}, {20, 10},
    {0, 20}, {10, 20}, {20, 20},
    {0, 30}, {10, 30}, {20, 30},
    {0, 40}, {10, 40}, {20, 40},
    {30, 20}
};

// ==========================================
// 【新增】真正的 HSV 到 RGB 转换引擎
// 原理：通过 Hue(色相 0-255) 映射完整的赤橙黄绿青蓝紫光谱
// ==========================================
static struct led_rgb hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return (struct led_rgb){v, v, v};

    uint8_t region = h / 43;
    uint8_t remainder = (h - (region * 43)) * 6;

    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0: return (struct led_rgb){v, t, p};
        case 1: return (struct led_rgb){q, v, p};
        case 2: return (struct led_rgb){p, v, t};
        case 3: return (struct led_rgb){p, q, v};
        case 4: return (struct led_rgb){t, p, v};
        default: return (struct led_rgb){v, p, q};
    }
}

// ==========================================
// 核心动画线程
// ==========================================
void custom_led_thread_main(void) {
    uint32_t tick = 0;
    if (!device_is_ready(led_strip)) {
        printk("Custom LED: WS2812 strip not ready!\n");
        return;
    } 

    while (1) {
        if (!is_awake) {
            for (int i = 0; i < NUM_PIXELS; i++) {
                pixels[i] = (struct led_rgb){0, 0, 0};
            }
            led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
            k_sleep(K_MSEC(500)); 
            continue;
        }

        // --- 1. 渲染前 15 颗矩阵灯 ---
        for (int i = 0; i < STATUS_LED_IDX; i++) {
            uint8_t cx = coords[i].x;
            uint8_t cy = coords[i].y;

            if (current_effect == 0) {
                // 效果 0：全局呼吸渐变
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 2), 255, 255); 
            } 
            else if (current_effect == 1) {
                // 效果 1：横向幻彩波浪
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 3 - cx * 4), 255, 255);
            }
            else if (current_effect == 2) {
                // 效果 2：纵向幻彩波浪 (如瀑布)
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 3 - cy * 4), 255, 255);
            }
            else if (current_effect == 3) {
                // 【新增】效果 3：完美复刻 ZMK Effect 4 (Swirl对角线幻彩漩涡)
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 3 - cx * 3 - cy * 3), 255, 255);
            }
        }

        // --- 2. 渲染第 16 颗状态灯 ---
        // 【关键修复】：增加 battery_level > 0 的判断。防止无电池状态(0%)一直报错覆盖层颜色
        if (battery_level > 0 && battery_level < 10) {
            if (tick % 30 < 15) pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x00, 0x00};
            else pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0};
        } 
        else if (status_display_frames > 0) {
            uint8_t active_layer = zmk_keymap_highest_layer_active();

            // 【硬件级调色】：针对 WS2812 特调的颜色，抛弃屏幕HEX码
            switch (active_layer) {
                case 0: 
                    // 特调粉色：红光拉满，混入少量蓝光制造紫粉调，极少绿光防白化
                    pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x14, 0x64}; 
                    break; 
                case 1: 
                    // 特调橙色：红光拉满，绿光压低到十分之一，杜绝发黄
                    pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x2A, 0x00}; 
                    break; 
                case 2: 
                    // 标准绿色
                    pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0xFF, 0x00}; 
                    break; 
                case 3: 
                    // 特调天蓝：防止偏暗，蓝绿混合
                    pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0x80, 0xFF}; 
                    break; 
                default: 
                    pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0xFF, 0xFF}; 
                    break; 
            }
            status_display_frames--; 
        } 
        else {
            pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0}; 
        }
        
        // --- 3. 全局亮度限制 ---
        for (int i = 0; i < NUM_PIXELS; i++) {
            pixels[i].r = (pixels[i].r * BRIGHTNESS_PERCENT) / 100;
            pixels[i].g = (pixels[i].g * BRIGHTNESS_PERCENT) / 100;
            pixels[i].b = (pixels[i].b * BRIGHTNESS_PERCENT) / 100;
        }
                
        led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
        tick++;
        k_sleep(K_MSEC(30));
    }
}
K_THREAD_DEFINE(custom_led_tid, 1024, custom_led_thread_main, NULL, NULL, NULL, 7, 0, 0);

// ==========================================
// 事件监听器
// ==========================================

static int activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev) {
        if (ev->state == ZMK_ACTIVITY_ACTIVE) {
            is_awake = true;
            status_display_frames = 66; 
        } else {
            is_awake = false; 
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(activity_status, activity_listener);
ZMK_SUBSCRIPTION(activity_status, zmk_activity_state_changed);

static int keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state) {
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
    status_display_frames = 66; 
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
