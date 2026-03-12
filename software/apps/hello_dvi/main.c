#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "testcard_320x240_rgb332.h"

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_640x480p_60hz

// LED pins (A2 = GPIO28, A3 = GPIO29, 24 = GPIO24)
#define PIN_R 28
#define PIN_G 29
#define PIN_B 24
#define PIN_SW 25
#define MAX_COLORS 3 

static uint8_t frame[FRAME_WIDTH * FRAME_HEIGHT];
struct dvi_inst dvi0;

static inline uint16_t read_adc(uint ch) {
    adc_select_input(ch);
    return adc_read();
}

static inline int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid)) __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_8bpp(&dvi0);
}

int main() {
    stdio_init_all();
    sleep_ms(800);

    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);
    gpio_init(PIN_SW);
    gpio_set_dir(PIN_SW, GPIO_IN);
    gpio_pull_up(PIN_SW); 


    // PWM init for RGB LED
    gpio_set_function(PIN_R, GPIO_FUNC_PWM);
    gpio_set_function(PIN_G, GPIO_FUNC_PWM);
    gpio_set_function(PIN_B, GPIO_FUNC_PWM);

    uint slice_r = pwm_gpio_to_slice_num(PIN_R);
    uint slice_g = pwm_gpio_to_slice_num(PIN_G);
    uint slice_b = pwm_gpio_to_slice_num(PIN_B);

    pwm_set_wrap(slice_r, 255);
    pwm_set_wrap(slice_g, 255);
    pwm_set_wrap(slice_b, 255);

    pwm_set_enabled(slice_r, true);
    pwm_set_enabled(slice_g, true);
    pwm_set_enabled(slice_b, true);

    // DVI init
    dvi0.timing  = &DVI_TIMING;
    dvi0.ser_cfg = adafruit_feather_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    multicore_launch_core1(core1_main);

    // Cursor start position
    int mx = FRAME_WIDTH  / 2;
    int my = FRAME_HEIGHT / 2;

    // Joystick calibration
    uint32_t sx=0, sy=0;
    for (int i = 0; i < 30; i++) {
        sx += read_adc(0);
        sy += read_adc(1);
        sleep_ms(5);
    }
    int x0 = sx / 30;
    int y0 = sy / 30;

    uint8_t colors[MAX_COLORS] = {0};
    int color_count = 0;           // ile kolorów zapisano
    uint32_t last_btn_time = 0;    // debounce
    uint32_t blink_time = 0;
    int blink_index = 0;

    while (1) {

//obsluga joysticka
    int xr = read_adc(0);
    int yr = read_adc(1);
    int dx = x0 - xr;
    int dy = yr - y0;    
//    int dx = xr - x0;
//    int dy = y0 - yr;

    int dead = 80;
    if (dx > -dead && dx < dead) dx = 0;
    if (dy > -dead && dy < dead) dy = 0;
//czułośc
    dx /= 500;
    dy /= 500;

    mx = clamp(mx + dx, 0, FRAME_WIDTH  - 1);
    my = clamp(my + dy, 0, FRAME_HEIGHT - 1);

    memcpy(frame, testcard_320x240, FRAME_WIDTH * FRAME_HEIGHT);

    for (int j = -2; j <= 2; j++) {
      if (j==-2 || j==2){
        int yy = clamp(my + j, 0, FRAME_HEIGHT - 1);
        frame[yy * FRAME_WIDTH + mx] = 0xFF;  
      }else{
        int yy = clamp(my + j, 0, FRAME_HEIGHT - 1);
        frame[yy * FRAME_WIDTH + mx] = 0x00;        
      } 
    }
    for (int i = -2; i <= 2; i++) {
      if (i==-2 || i==2){
        int xx = clamp(mx + i, 0, FRAME_WIDTH - 1);
        frame[my * FRAME_WIDTH + xx] = 0xFF;
    }else{
        int xx = clamp(mx + i, 0, FRAME_WIDTH - 1);
        frame[my * FRAME_WIDTH + xx] = 0x00;      
    }
    }
    uint8_t col = testcard_320x240[my * FRAME_WIDTH + mx];


// przycik srodek
    bool pressed = !gpio_get(PIN_SW);    // aktywny stan 0
    uint32_t now = time_us_32();

    if (pressed && (now - last_btn_time > 200000)) {  // 200 ms
        last_btn_time = now;

        // Zapisujemy kolor z kursora (col)
        if (color_count < MAX_COLORS) {
            colors[color_count] = col;
            color_count++;
        } else {
            // przesuwamy w lewo
            for (int i = 1; i < MAX_COLORS; i++)
                colors[i-1] = colors[i];
            colors[MAX_COLORS - 1] = col;
        }
    }


//tabela kolorów
    if (color_count > 0) {

        if (now - blink_time > 300000) { // mryganie co 300 ms
            blink_time = now;
            blink_index = (blink_index + 1) % color_count;
        }

        uint8_t cur = colors[blink_index];

        uint8_t r3 = (cur >> 5) & 0x07;
        uint8_t g3 = (cur >> 2) & 0x07;
        uint8_t b2 =  cur       & 0x03;

        uint8_t R = (r3 * 255) / 7;
        uint8_t G = (g3 * 255) / 7;
        uint8_t B = (b2 * 255) / 3;

        R = 255 - R;
        G = 255 - G;
        B = 255 - B;

        pwm_set_gpio_level(PIN_R, R);
        pwm_set_gpio_level(PIN_G, G);
        pwm_set_gpio_level(PIN_B, B);
    } 
    else {
    
        pwm_set_gpio_level(PIN_R, 0);
        pwm_set_gpio_level(PIN_G, 0);
        pwm_set_gpio_level(PIN_B, 0);
    }


//print obrazu 
    for (uint yy = 0; yy < FRAME_HEIGHT; ++yy) {
        uint8_t *scanline = &frame[yy * FRAME_WIDTH];
        queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
        while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline)) {}
    }
  }
}