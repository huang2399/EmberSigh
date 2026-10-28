/*
 * ESP32-S3-EYE DreamState Monitor
 * Audio via ES8311 Codec (BSP) + ST7789 LCD (SPI)
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp32_s3_eye.h"
#include "esp_codec_dev.h"
static const char *TAG = "dreamstate";
#define SAMPLE_RATE 16000
#define BUFFER_SAMPLES 512
#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK (40*1000*1000)
#define LCD_MOSI GPIO_NUM_13
#define LCD_SCLK GPIO_NUM_14
#define LCD_CS GPIO_NUM_10
#define LCD_DC GPIO_NUM_5
#define LCD_RST GPIO_NUM_9
#define LCD_BACKLIGHT GPIO_NUM_15
#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define COLOR_BG 0x0010
#define COLOR_TEXT 0xFFFF
#define COLOR_ALERT_BG 0xF800
#define COLOR_ALERT_TEXT 0xFFFF
#define NOISE_THRESHOLD 500.0f
#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define SCALE 4
static const uint8_t font5x7[95][5]={{0,0,0,0,0},{0,0,95,0,0},{0,7,0,7,0},{20,127,20,127,20},{36,42,127,42,18},{35,19,8,100,98},{54,73,85,34,80},{0,5,3,0,0},{0,28,34,65,0},{0,65,34,28,0},{8,42,28,42,8},{8,8,62,8,8},{0,80,48,0,0},{8,8,8,8,8},{0,96,96,0,0},{32,16,8,4,2},{62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},{24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30},{0,54,54,0,0},{0,86,54,0,0},{0,8,20,34,65},{20,20,20,20,20},{65,34,20,8,0},{2,1,81,9,6},{50,73,121,65,62},{126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},{127,9,9,1,1},{62,65,65,81,50},{127,8,8,8,127},{0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},{127,2,4,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},{63,64,64,64,63},{31,32,64,32,31},{127,32,24,32,127},{99,20,8,20,99},{3,4,120,4,3},{97,81,73,69,67},{0,0,127,65,65},{2,4,8,16,32},{65,65,127,0,0},{4,2,1,2,4},{64,64,64,64,64},{0,1,2,4,0},{32,84,84,84,120},{127,72,68,68,56},{56,68,68,68,32},{56,68,68,72,127},{56,84,84,84,24},{8,126,9,1,2},{8,20,84,84,60},{127,8,4,4,120},{0,68,125,64,0},{32,64,68,61,0},{0,127,16,40,68},{0,65,127,64,0},{124,4,24,4,120},{124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},{8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,32},{4,63,68,64,32},{60,64,64,32,124},{28,32,64,32,28},{60,64,48,64,60},{68,40,16,40,68},{12,80,80,80,60},{68,100,84,76,68},{0,8,54,65,0},{0,0,127,0,0},{0,65,54,8,0},{8,8,42,28,8},{8,28,42,8,8},{0,0,0,0,0}};
static esp_lcd_panel_io_handle_t io_handle=NULL;
static esp_lcd_panel_handle_t panel_handle=NULL;
static uint16_t cb[240];
static float calc_rms_stereo_l(const int16_t*s,int n){float sq=0;int c=0;for(int i=0;i<n;i+=2){float v=(float)s[i];sq+=v*v;c++;}return c>0?sqrtf(sq/c):0;}
static void lcd_win(int x0,int y0,int x1,int y1){uint8_t d[4];esp_lcd_panel_io_tx_param(io_handle,0x2A,NULL,0);d[0]=x0>>8;d[1]=x0;d[2]=x1>>8;d[3]=x1;esp_lcd_panel_io_tx_param(io_handle,-1,d,4);esp_lcd_panel_io_tx_param(io_handle,0x2B,NULL,0);d[0]=y0>>8;d[1]=y0;d[2]=y1>>8;d[3]=y1;esp_lcd_panel_io_tx_param(io_handle,-1,d,4);esp_lcd_panel_io_tx_param(io_handle,0x2C,NULL,0);}
static void fl(uint16_t c,int n){for(int i=0;i<240;i++)cb[i]=c;while(n>0){int k=n<240?n:240;esp_lcd_panel_io_tx_color(io_handle,-1,(uint8_t*)cb,k*2);n-=k;}}
static void dc(int x,int y,char ch,uint16_t fg,uint16_t bg){if(ch<32||ch>126)ch=32;const uint8_t*g=font5x7[ch-32];lcd_win(x,y,x+20-1,y+28-1);for(int r=0;r<7;r++)for(int sy=0;sy<4;sy++)for(int c=0;c<5;c++){uint16_t cl=(g[r]&(1<<c))?fg:bg;for(int sx=0;sx<4;sx++)esp_lcd_panel_io_tx_color(io_handle,-1,(uint8_t*)&cl,2);}}
static void dsc(const char*s,uint16_t fg,uint16_t bg){int n=(int)strlen(s);int x=(240-n*20)/2;int y=(240-28)/2;lcd_win(0,0,239,239);fl(bg,240*240);for(int i=0;i<n;i++)dc(x+i*20,y,s[i],fg,bg);}
static void dm(const char*m,uint16_t bg,uint16_t fg){ESP_LOGI(TAG,"LCD: %s",m);dsc(m,fg,bg);}
static void init_lcd(void){
ESP_LOGI(TAG,"Init LCD ST7789...");
gpio_config_t bl={.pin_bit_mask=1ULL<<LCD_BACKLIGHT,.mode=GPIO_MODE_OUTPUT};
gpio_config(&bl);gpio_set_level(LCD_BACKLIGHT,1);
ESP_LOGI(TAG,"Backlight ON");
spi_bus_config_t bc={.mosi_io_num=LCD_MOSI,.miso_io_num=-1,.sclk_io_num=LCD_SCLK,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=240*80*2};
ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST,&bc,SPI_DMA_CH_AUTO));
esp_lcd_panel_io_spi_config_t ic={.cs_gpio_num=LCD_CS,.dc_gpio_num=LCD_DC,.spi_mode=0,.pclk_hz=LCD_PIXEL_CLOCK,.trans_queue_depth=10,.lcd_cmd_bits=8,.lcd_param_bits=8};
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,&ic,&io_handle));
esp_lcd_panel_dev_config_t pc={.reset_gpio_num=LCD_RST,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle,&pc,&panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
esp_lcd_panel_invert_color(panel_handle,true);
esp_lcd_panel_mirror(panel_handle,true,false);
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle,true));
ESP_LOGI(TAG,"LCD ready");
dm("Listening...",COLOR_BG,COLOR_TEXT);
}
static esp_codec_dev_handle_t mic_dev=NULL;
static void init_audio(void){
ESP_LOGI(TAG,"Init audio via BSP...");
esp_err_t r=bsp_audio_init(NULL);
if(r!=ESP_OK){ESP_LOGE(TAG,"bsp_audio_init: %s",esp_err_to_name(r));return;}
mic_dev=bsp_audio_codec_microphone_init();
if(!mic_dev){ESP_LOGE(TAG,"Microphone init failed");return;}
ESP_LOGI(TAG,"Audio ready");
}
void app_main(void){
printf("\n=== DreamState BSP Audio ===\n\n");fflush(stdout);
init_lcd();
init_audio();
int16_t*a=malloc(BUFFER_SAMPLES*sizeof(int16_t));
assert(a);
bool wn=false;
ESP_LOGI(TAG,"Loop running (thresh=%.0f)...",NOISE_THRESHOLD);
while(1){
int rb=esp_codec_dev_read(mic_dev,a,BUFFER_SAMPLES*sizeof(int16_t));
if(rb>0){
int n=rb/2;
float rms=calc_rms_stereo_l(a,n);
bool ns=(rms>NOISE_THRESHOLD);
static int dc=0;
if(++dc==5){dc=0;printf("[L] %d %d %d | [R] %d %d | RMS=%.2f\n",a[0],a[2],a[4],a[1],a[3],rms);fflush(stdout);}
if(ns!=wn){wn=ns;
if(ns){printf("[Noise!] RMS=%.2f\n",rms);dm("Noise!",COLOR_ALERT_BG,COLOR_ALERT_TEXT);}
else{printf("[Quiet] RMS=%.2f\n",rms);dm("Listening...",COLOR_BG,COLOR_TEXT);}
fflush(stdout);}
}else{ESP_LOGW(TAG,"read: %d bytes",rb);}
vTaskDelay(pdMS_TO_TICKS(10));
}
}
