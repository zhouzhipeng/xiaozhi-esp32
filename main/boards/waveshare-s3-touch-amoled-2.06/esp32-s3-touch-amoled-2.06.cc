#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_sh8601.h"
#include "font_awesome_symbols.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"
#include <wifi_station.h>

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <esp_timer.h>

#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <errno.h>
#include <string.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
#include <esp_rom_sys.h>
#include "jpeg_decoder.h"
#include <ff.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>

// 包含必要的头文件

#define TAG "WaveshareEsp32s3TouchAMOLED2inch06"

LV_FONT_DECLARE(font_puhui_20_4);  // 使用更小的字体以节省内存
LV_FONT_DECLARE(font_awesome_20_4);

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off
        
        // 启用PWRON按键中断
        // 0x40: IRQ使能寄存器1
        // Bit 3: PWRON短按中断使能
        uint8_t irq_enable = ReadReg(0x40);
        irq_enable |= 0x08;  // 启用PWRON短按中断
        WriteReg(0x40, irq_enable);

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x03);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
    
    // 公共方法来读取IRQ状态
    uint8_t GetIRQStatus1() {
        return ReadReg(0x48);
    }
    
    // 公共方法来清除IRQ标志
    void ClearIRQStatus1(uint8_t mask) {
        WriteReg(0x48, mask);
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x2A, (uint8_t []){0x00,0x16,0x01,0xAF}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xF5}, 4, 0},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

// SD卡GPIO配置
#define SD_MOSI_PIN GPIO_NUM_1
#define SD_MISO_PIN GPIO_NUM_3
#define SD_SCK_PIN  GPIO_NUM_2
#define SD_CS_PIN   GPIO_NUM_17

// 在waveshare_amoled_2_06类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    sdmmc_card_t* sd_card_ = nullptr;
    
private:
    lv_obj_t* sd_button_ = nullptr;
    lv_obj_t* sd_image_ = nullptr;
    lv_timer_t* clear_timer_ = nullptr;  // 添加清屏定时器
    bool sd_initialized_ = false;
    uint8_t* image_buffer_ = nullptr;
    size_t image_buffer_size_ = 0;
    int image_width_ = 0;
    int image_height_ = 0;
    
    static void sd_button_event_cb(lv_event_t* e) {
        CustomLcdDisplay* display = (CustomLcdDisplay*)lv_event_get_user_data(e);
        display->DisplaySDCardImage();
    }
    
    static void clear_timer_cb(lv_timer_t* timer) {
        CustomLcdDisplay* display = (CustomLcdDisplay*)lv_timer_get_user_data(timer);
        display->ClearImage();
    }
    
    void ClearImage() {
        DisplayLockGuard lock(this);
        
        // 删除图片
        if (sd_image_ != nullptr) {
            lv_obj_del(sd_image_);
            sd_image_ = nullptr;
        }
        
        // 释放图片缓冲区
        if (image_buffer_ != nullptr) {
            free(image_buffer_);
            image_buffer_ = nullptr;
        }
        
        // 删除定时器
        if (clear_timer_ != nullptr) {
            lv_timer_del(clear_timer_);
            clear_timer_ = nullptr;
        }
        
        // 清除所有可能的标签（PNG/BMP提示等）
        uint32_t child_cnt = lv_obj_get_child_count(container_);
        for(int i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t* child = lv_obj_get_child(container_, i);
            // 只删除标签和图片，不删除状态栏等重要元素
            if(lv_obj_check_type(child, &lv_label_class) && child != status_bar_) {
                lv_obj_del(child);
            }
        }
        
        ESP_LOGI("CustomLcdDisplay", "Screen cleared after 5 seconds");
    }
    
    void SetupClearTimer() {
        // 如果已有定时器，先删除
        if (clear_timer_ != nullptr) {
            lv_timer_del(clear_timer_);
            clear_timer_ = nullptr;
        }
        
        // 创建5秒后清屏的定时器
        clear_timer_ = lv_timer_create(clear_timer_cb, 5000, this);
        lv_timer_set_repeat_count(clear_timer_, 1);  // 只执行一次
        
        ESP_LOGI("CustomLcdDisplay", "Clear timer set for 5 seconds");
    }
    
    
public:
    void DisplayJpegFile(const char* filepath) {
        DisplayLockGuard lock(this);
        
        // 如果已有图片显示，先删除
        if (sd_image_ != nullptr) {
            lv_obj_del(sd_image_);
            sd_image_ = nullptr;
        }
        
        // 释放之前的图片缓冲区
        if (image_buffer_ != nullptr) {
            free(image_buffer_);
            image_buffer_ = nullptr;
        }
        
        // 检查文件扩展名
        std::string file_path_str(filepath);
        std::string extension = "";
        size_t dot_pos = file_path_str.rfind('.');
        if (dot_pos != std::string::npos) {
            extension = file_path_str.substr(dot_pos + 1);
            // 转换为小写
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        }
        
        ESP_LOGI("CustomLcdDisplay", "File extension: %s", extension.c_str());
        
        if (extension == "png") {
            // PNG文件处理
            DisplayPngFile(filepath);
            return;
        } else if (extension == "bmp") {
            // BMP文件处理
            DisplayBmpFile(filepath);
            return;
        }
        
        // 默认作为JPEG处理
        // 读取JPEG文件
        FILE* fp = fopen(filepath, "rb");
        if (!fp) {
            ESP_LOGE("CustomLcdDisplay", "Failed to open image file: %s", filepath);
            ShowNotification("无法打开图片", 3000);
            return;
        }
        
        // 获取文件大小
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        ESP_LOGI("CustomLcdDisplay", "JPEG file size: %ld bytes", file_size);
        
        // 限制读取大小，防止内存不足（最多500KB用于JPEG数据）
        size_t read_size = (file_size > 512000) ? 512000 : file_size;
        
        // 分配JPEG数据缓冲区
        uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(read_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg_data) {
            jpeg_data = (uint8_t*)malloc(read_size);
            if (!jpeg_data) {
                ESP_LOGE("CustomLcdDisplay", "Failed to allocate JPEG buffer");
                fclose(fp);
                ShowNotification("内存不足", 3000);
                return;
            }
        }
        
        // 读取JPEG数据
        size_t bytes_read = fread(jpeg_data, 1, read_size, fp);
        fclose(fp);
        
        if (bytes_read == 0) {
            ESP_LOGE("CustomLcdDisplay", "Failed to read JPEG data");
            free(jpeg_data);
            ShowNotification("读取图片失败", 3000);
            return;
        }
        
        ESP_LOGI("CustomLcdDisplay", "Read %d bytes of JPEG data", bytes_read);
        
        // 首先获取图片信息来决定缩放比例
        esp_jpeg_image_cfg_t jpeg_cfg = {};
        jpeg_cfg.indata = jpeg_data;
        jpeg_cfg.indata_size = bytes_read;
        jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_0;  // 先不缩放获取原始尺寸
        
        esp_jpeg_image_output_t img_info = {};
        esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &img_info);
        
        if (ret != ESP_OK) {
            ESP_LOGE("CustomLcdDisplay", "Failed to get JPEG info: %s", esp_err_to_name(ret));
            free(jpeg_data);
            ShowNotification("解析图片失败", 3000);
            return;
        }
        
        ESP_LOGI("CustomLcdDisplay", "Original image size: %dx%d", img_info.width, img_info.height);
        
        // 根据图片大小决定缩放比例
        // 屏幕大约是480x502，我们希望图片能适应屏幕
        if (img_info.width > 960 || img_info.height > 1000) {
            jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;  // 缩放到1/4
            ESP_LOGI("CustomLcdDisplay", "Using 1/4 scale for large image");
        } else if (img_info.width > 480 || img_info.height > 500) {
            jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;  // 缩放到1/2
            ESP_LOGI("CustomLcdDisplay", "Using 1/2 scale for medium image");
        } else {
            jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_0;    // 不缩放
            ESP_LOGI("CustomLcdDisplay", "No scaling for small image");
        }
        
        // 重新获取缩放后的尺寸
        ret = esp_jpeg_get_image_info(&jpeg_cfg, &img_info);
        if (ret != ESP_OK) {
            ESP_LOGE("CustomLcdDisplay", "Failed to get scaled JPEG info");
            free(jpeg_data);
            ShowNotification("解析图片失败", 3000);
            return;
        }
        
        ESP_LOGI("CustomLcdDisplay", "Scaled image size: %dx%d, output_len: %d bytes", 
                 img_info.width, img_info.height, img_info.output_len);
        
        // 分配输出缓冲区
        image_buffer_ = (uint8_t*)heap_caps_malloc(img_info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!image_buffer_) {
            image_buffer_ = (uint8_t*)malloc(img_info.output_len);
            if (!image_buffer_) {
                ESP_LOGE("CustomLcdDisplay", "Failed to allocate output buffer (%d bytes)", img_info.output_len);
                free(jpeg_data);
                ShowNotification("内存不足", 3000);
                return;
            }
        }
        
        // 设置输出缓冲区并解码
        jpeg_cfg.outbuf = image_buffer_;
        jpeg_cfg.outbuf_size = img_info.output_len;
        jpeg_cfg.flags.swap_color_bytes = 0;  // RGB565不需要交换字节
        
        ESP_LOGI("CustomLcdDisplay", "Starting JPEG decode...");
        ret = esp_jpeg_decode(&jpeg_cfg, &img_info);
        
        // 释放JPEG数据缓冲区
        free(jpeg_data);
        
        if (ret != ESP_OK) {
            ESP_LOGE("CustomLcdDisplay", "JPEG decode failed: %s", esp_err_to_name(ret));
            free(image_buffer_);
            image_buffer_ = nullptr;
            ShowNotification("解码图片失败", 3000);
            return;
        }
        
        ESP_LOGI("CustomLcdDisplay", "JPEG decode successful! Image: %dx%d", img_info.width, img_info.height);
        
        image_width_ = img_info.width;
        image_height_ = img_info.height;
        
        // 创建LVGL图片对象
        sd_image_ = lv_img_create(container_);
        
        // 创建LVGL图片描述符
        static lv_img_dsc_t img_dsc;
        img_dsc.header.reserved_2 = 0;
        img_dsc.header.w = image_width_;
        img_dsc.header.h = image_height_;
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc.data_size = img_info.output_len;
        img_dsc.data = image_buffer_;
        
        lv_img_set_src(sd_image_, &img_dsc);
        
        // 如果图片太大，允许滚动
        if (image_width_ > LV_HOR_RES || image_height_ > LV_VER_RES - 100) {
            // 创建一个可滚动的容器
            lv_obj_set_size(sd_image_, image_width_, image_height_);
            lv_obj_align(sd_image_, LV_ALIGN_TOP_MID, 0, 60);  // 留出状态栏空间
            lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_AUTO);
            ESP_LOGI("CustomLcdDisplay", "Image is larger than screen, scrolling enabled");
        } else {
            // 居中显示图片
            lv_obj_align(sd_image_, LV_ALIGN_CENTER, 0, -30);
            ESP_LOGI("CustomLcdDisplay", "Image fits on screen, centered display");
        }
        
        ESP_LOGI("CustomLcdDisplay", "Image displayed successfully!");
        
        // 设置5秒后清屏
        SetupClearTimer();
    }
    
    // PNG文件显示函数
    void DisplayPngFile(const char* filepath) {
        ESP_LOGI("CustomLcdDisplay", "Displaying PNG file: %s", filepath);
        
        // 当前暂不支持PNG格式，提示用户转换格式
        ESP_LOGW("CustomLcdDisplay", "PNG format is not fully supported. Please convert to JPEG for best results.");
        ShowNotification("PNG格式暂不支持，请转换为JPEG格式", 5000);
        
        // 显示一个友好的占位符图像
        DisplayLockGuard lock(this);
        
        // 如果已有图片显示，先删除
        if (sd_image_ != nullptr) {
            lv_obj_del(sd_image_);
            sd_image_ = nullptr;
        }
        
        // 释放之前的图片缓冲区
        if (image_buffer_ != nullptr) {
            free(image_buffer_);
            image_buffer_ = nullptr;
        }
        
        // 创建一个带PNG标识的占位符
        int img_width = 240;
        int img_height = 240;
        size_t img_size = img_width * img_height * 2;  // RGB565
        
        image_buffer_ = (uint8_t*)heap_caps_malloc(img_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!image_buffer_) {
            image_buffer_ = (uint8_t*)malloc(img_size);
            if (!image_buffer_) {
                ESP_LOGE("CustomLcdDisplay", "Failed to allocate placeholder buffer");
                return;
            }
        }
        
        // 创建一个渐变背景作为PNG占位符
        uint16_t* pixels = (uint16_t*)image_buffer_;
        for (int y = 0; y < img_height; y++) {
            for (int x = 0; x < img_width; x++) {
                // 创建蓝色渐变效果
                uint8_t blue = 128 + (y * 127 / img_height);
                uint8_t green = 64 + (x * 64 / img_width);
                uint8_t red = 32;
                
                uint16_t color = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3);
                pixels[y * img_width + x] = color;
            }
        }
        
        image_width_ = img_width;
        image_height_ = img_height;
        
        // 创建LVGL图片对象
        sd_image_ = lv_img_create(container_);
        
        // 创建LVGL图片描述符
        static lv_img_dsc_t img_dsc;
        img_dsc.header.reserved_2 = 0;
        img_dsc.header.w = image_width_;
        img_dsc.header.h = image_height_;
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc.data_size = img_size;
        img_dsc.data = image_buffer_;
        
        lv_img_set_src(sd_image_, &img_dsc);
        lv_obj_align(sd_image_, LV_ALIGN_CENTER, 0, -50);
        
        // 添加PNG标签
        lv_obj_t* label = lv_label_create(container_);
        lv_label_set_text(label, "PNG 图片");
        lv_obj_set_style_text_font(label, fonts_.text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 100);
        
        // 添加提示信息
        lv_obj_t* hint_label = lv_label_create(container_);
        lv_label_set_text(hint_label, "请使用JPEG格式");
        lv_obj_set_style_text_font(hint_label, fonts_.text_font, 0);
        lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFFF00), 0);
        lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 130);
        
        ESP_LOGI("CustomLcdDisplay", "PNG placeholder displayed");
        
        // 设置5秒后清屏
        SetupClearTimer();
    }
    
    // BMP文件显示函数  
    void DisplayBmpFile(const char* filepath) {
        ESP_LOGI("CustomLcdDisplay", "Displaying BMP file: %s", filepath);
        
        // BMP是相对简单的格式，可以手动解析
        FILE* fp = fopen(filepath, "rb");
        if (!fp) {
            ESP_LOGE("CustomLcdDisplay", "Failed to open BMP file");
            ShowNotification("无法打开BMP文件", 3000);
            return;
        }
        
        // 读取BMP文件头
        uint8_t header[54];
        if (fread(header, 1, 54, fp) != 54) {
            ESP_LOGE("CustomLcdDisplay", "Invalid BMP header");
            fclose(fp);
            ShowNotification("无效的BMP文件", 3000);
            return;
        }
        
        // 检查是否是BMP文件
        if (header[0] != 'B' || header[1] != 'M') {
            ESP_LOGE("CustomLcdDisplay", "Not a valid BMP file");
            fclose(fp);
            ShowNotification("不是有效的BMP文件", 3000);
            return;
        }
        
        // 解析BMP信息
        int width = *(int*)&header[18];
        int height = *(int*)&header[22];
        int bits_per_pixel = *(short*)&header[28];
        
        ESP_LOGI("CustomLcdDisplay", "BMP: %dx%d, %d bpp", width, height, bits_per_pixel);
        
        // 目前只支持24位BMP
        if (bits_per_pixel != 24) {
            ESP_LOGE("CustomLcdDisplay", "Only 24-bit BMP supported, got %d-bit", bits_per_pixel);
            fclose(fp);
            ShowNotification("仅支持24位BMP图片", 3000);
            return;
        }
        
        fclose(fp);
        
        // 显示占位符
        DisplayPlaceholderImage("BMP Image");
        ShowNotification("BMP支持开发中", 3000);
    }
    
    // 显示占位符图像
    void DisplayPlaceholderImage(const char* text) {
        DisplayLockGuard lock(this);
        
        // 创建一个简单的占位符图像
        int img_width = 200;
        int img_height = 200;
        size_t img_size = img_width * img_height * 2;  // RGB565
        
        image_buffer_ = (uint8_t*)heap_caps_malloc(img_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!image_buffer_) {
            image_buffer_ = (uint8_t*)malloc(img_size);
            if (!image_buffer_) {
                ESP_LOGE("CustomLcdDisplay", "Failed to allocate placeholder buffer");
                return;
            }
        }
        
        // 填充灰色背景
        uint16_t* pixels = (uint16_t*)image_buffer_;
        uint16_t gray = ((0x80 & 0xF8) << 8) | ((0x80 & 0xFC) << 3) | (0x80 >> 3);
        for (int i = 0; i < img_width * img_height; i++) {
            pixels[i] = gray;
        }
        
        image_width_ = img_width;
        image_height_ = img_height;
        
        // 创建LVGL图片对象
        sd_image_ = lv_img_create(container_);
        
        // 创建LVGL图片描述符
        static lv_img_dsc_t img_dsc;
        img_dsc.header.reserved_2 = 0;
        img_dsc.header.w = image_width_;
        img_dsc.header.h = image_height_;
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc.data_size = img_size;
        img_dsc.data = image_buffer_;
        
        lv_img_set_src(sd_image_, &img_dsc);
        lv_obj_align(sd_image_, LV_ALIGN_CENTER, 0, -30);
        
        // 添加文字标签
        lv_obj_t* label = lv_label_create(container_);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, fonts_.text_font, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 100);
        
        // 设置5秒后清屏
        SetupClearTimer();
    }
    
    bool InitializeSDCard() {
        if (sd_initialized_) {
            return true;
        }
        
        esp_err_t ret;
        const char mount_point[] = "/sdcard";
        
        ESP_LOGI("CustomLcdDisplay", "=== SD Card Initialization Start ===");
        ESP_LOGI("CustomLcdDisplay", "Using pins - MOSI:%d, MISO:%d, SCK:%d, CS:%d", 
                 SD_MOSI_PIN, SD_MISO_PIN, SD_SCK_PIN, SD_CS_PIN);
        
        // 首先复位GPIO状态
        gpio_reset_pin(SD_MOSI_PIN);
        gpio_reset_pin(SD_MISO_PIN);
        gpio_reset_pin(SD_SCK_PIN);
        gpio_reset_pin(SD_CS_PIN);
        
        // 配置CS引脚为输出，初始为高电平（重要！）
        gpio_set_direction(SD_CS_PIN, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(SD_CS_PIN, GPIO_PULLUP_ONLY);
        gpio_set_level(SD_CS_PIN, 1);
        
        // 配置其他引脚的上拉（SD卡需要）
        gpio_set_pull_mode(SD_MOSI_PIN, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(SD_MISO_PIN, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(SD_SCK_PIN, GPIO_PULLUP_ONLY);
        
        // 等待电源稳定
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // SD卡挂载配置
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,  // 0 = 自动选择
            .disk_status_check_enable = false
        };
        
        // 检查并初始化SPI总线
        static bool spi_bus_initialized = false;
        if (!spi_bus_initialized) {
            ESP_LOGI("CustomLcdDisplay", "Initializing SPI bus for SD card...");
            
            spi_bus_config_t bus_cfg = {
                .mosi_io_num = SD_MOSI_PIN,
                .miso_io_num = SD_MISO_PIN,
                .sclk_io_num = SD_SCK_PIN,
                .quadwp_io_num = GPIO_NUM_NC,
                .quadhd_io_num = GPIO_NUM_NC,
                .max_transfer_sz = 4000,
                .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
                .intr_flags = 0
            };
            
            // 尝试初始化SPI3总线
            ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
            if (ret == ESP_ERR_INVALID_STATE) {
                ESP_LOGW("CustomLcdDisplay", "SPI bus already initialized");
            } else if (ret != ESP_OK) {
                ESP_LOGE("CustomLcdDisplay", "Failed to initialize SPI bus: %s (0x%x)", esp_err_to_name(ret), ret);
                return false;
            }
            spi_bus_initialized = true;
        }
        
        ESP_LOGI("CustomLcdDisplay", "Configuring SD card host...");
        
        // SD卡主机配置
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI3_HOST;
        host.max_freq_khz = 400;  // 使用400kHz初始化（标准SD卡初始化速度）
        host.flags = SDMMC_HOST_FLAG_SPI | SDMMC_HOST_FLAG_DEINIT_ARG;
        
        // SD卡SPI设备配置
        sdspi_device_config_t slot_config = {};
        slot_config.gpio_cs = SD_CS_PIN;
        slot_config.host_id = SPI3_HOST;
        slot_config.gpio_cd = GPIO_NUM_NC;  // 不使用卡检测引脚
        slot_config.gpio_wp = GPIO_NUM_NC;  // 不使用写保护引脚
        slot_config.gpio_int = GPIO_NUM_NC; // 不使用中断引脚
        
        ESP_LOGI("CustomLcdDisplay", "Attempting to mount SD card...");
        
        // 首次尝试挂载
        ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &sd_card_);
        
        if (ret != ESP_OK) {
            ESP_LOGW("CustomLcdDisplay", "First mount attempt failed: %s (0x%x)", esp_err_to_name(ret), ret);
            
            // 如果失败，尝试手动初始化序列
            ESP_LOGI("CustomLcdDisplay", "Attempting manual SD card initialization sequence...");
            
            // 发送至少74个时钟周期（SD卡规范要求）
            gpio_set_level(SD_CS_PIN, 1);
            for(int i = 0; i < 10; i++) {
                gpio_set_level(SD_SCK_PIN, 0);
                esp_rom_delay_us(10);
                gpio_set_level(SD_SCK_PIN, 1);
                esp_rom_delay_us(10);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // 再次尝试挂载，使用更低速度
            host.max_freq_khz = 200;  // 200kHz重试
            ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &sd_card_);
            
            if (ret != ESP_OK) {
                ESP_LOGW("CustomLcdDisplay", "Second attempt failed, trying 100kHz...");
                host.max_freq_khz = 100;  // 100kHz最后尝试
                ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &sd_card_);
            }
        }
        
        if (ret == ESP_OK) {
            ESP_LOGI("CustomLcdDisplay", "SD card mount SUCCESS!");
        }
        
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE("CustomLcdDisplay", "Failed to mount filesystem. Make sure SD card is inserted properly.");
            } else {
                ESP_LOGE("CustomLcdDisplay", "Failed to initialize SD card: %s (0x%x)", esp_err_to_name(ret), ret);
                if (ret == ESP_ERR_TIMEOUT) {
                    ESP_LOGE("CustomLcdDisplay", "SD card initialization timeout. Check card and connections.");
                } else if (ret == ESP_ERR_NOT_FOUND) {
                    ESP_LOGE("CustomLcdDisplay", "SD card not detected. Check if card is inserted and CS pin connection.");
                }
            }
            // 不要释放SPI总线，因为可能会被其他设备使用
            return false;
        }
        
        // 打印SD卡信息
        sdmmc_card_print_info(stdout, sd_card_);
        
        // 成功挂载后，逐步提高速度
        ESP_LOGI("CustomLcdDisplay", "SD card mounted successfully, gradually increasing speed...");
        
        // 先尝试1MHz
        sd_card_->max_freq_khz = 1000;
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 测试读取根目录
        DIR* test_dir = opendir(mount_point);
        if (test_dir) {
            closedir(test_dir);
            // 如果1MHz稳定，尝试提高到5MHz
            sd_card_->max_freq_khz = 5000;
            ESP_LOGI("CustomLcdDisplay", "SD card speed increased to 5MHz");
        } else {
            ESP_LOGW("CustomLcdDisplay", "Keeping SD card at low speed for stability");
        }
        
        sd_initialized_ = true;
        return true;
    }
    
    void DisplaySDCardImage() {
        ESP_LOGI("CustomLcdDisplay", "Displaying SD card image...");
        
        // 尝试初始化SD卡
        if (!InitializeSDCard()) {
            ShowNotification("SD卡初始化失败,请确保已格式化为FAT32", 5000);
            ESP_LOGW("CustomLcdDisplay", "Note: 64GB cards must be formatted as FAT32, not exFAT!");
            ESP_LOGW("CustomLcdDisplay", "Use a tool like 'FAT32 Format' on Windows or 'Disk Utility' on Mac");
            return;
        }
        
        // 指定要显示的图片文件
        const char* image_path = "/sdcard/_C3A8676.jpg";
        
        // 检查文件是否存在
        struct stat file_stat;
        if (stat(image_path, &file_stat) != 0) {
            ESP_LOGE("CustomLcdDisplay", "Image file not found: %s", image_path);
            ShowNotification("找不到图片: _C3A8676.jpg", 3000);
            return;
        }
        
        ESP_LOGI("CustomLcdDisplay", "Found image file, size: %ld bytes", (long)file_stat.st_size);
        
        // 读取并解码JPEG文件显示缩略图
        ESP_LOGI("CustomLcdDisplay", "Loading and decoding JPEG file: %s (%.2f MB)", 
                 image_path, file_stat.st_size / (1024.0 * 1024.0));
        
        // 显示真实的JPEG图片缩略图
        DisplayJpegFile(image_path);
        
        ESP_LOGI("CustomLcdDisplay", "Image displayed successfully");
        ShowNotification("图片显示成功", 2000);
    }
    
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                        {
                            .text_font = &font_puhui_20_4,  // 使用更小的字体
                            .icon_font = &font_awesome_20_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                            .emoji_font = font_emoji_32_init(),
#else
                            .emoji_font = font_emoji_64_init(),
#endif
                        })
    {
        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.15, 0);  // 增加到15%避免圆角遮挡
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.15, 0); // 增加到15%避免圆角遮挡
        lv_obj_set_style_pad_top(status_bar_, 20, 0);  // 添加顶部边距让状态栏往下移
        lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height + 15); // 增加状态栏高度
        
        // 创建SD卡按钮
        // 注释掉原来的SD卡按钮，因为现在通过MCP控制
        // sd_button_ = lv_btn_create(container_);
        // lv_obj_set_size(sd_button_, LV_HOR_RES * 0.6, 50);
        // lv_obj_align(sd_button_, LV_ALIGN_BOTTOM_MID, 0, -30);
        // lv_obj_set_style_bg_color(sd_button_, lv_color_hex(0x2196F3), 0);
        // lv_obj_set_style_radius(sd_button_, 10, 0);
        // 
        // lv_obj_t* label = lv_label_create(sd_button_);
        // lv_label_set_text(label, "显示SD卡图片");
        // lv_obj_set_style_text_font(label, fonts_.text_font, 0);
        // lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        // lv_obj_center(label);
        // 
        // lv_obj_add_event_cb(sd_button_, sd_button_event_cb, LV_EVENT_CLICKED, this);
        
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}
    
    // 保存当前亮度
    void SaveCurrentBrightness() {
        saved_brightness_ = brightness();
    }
    
    // 恢复保存的亮度
    void RestoreSavedBrightness() {
        if (saved_brightness_ > 0) {
            SetBrightness(saved_brightness_);
        } else {
            RestoreBrightness();  // 使用默认恢复
        }
    }

protected:
    esp_lcd_panel_io_handle_t panel_io_;
    uint8_t saved_brightness_ = 50;  // 保存的亮度值

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch06 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    PowerSaveTimer* power_save_timer_;
    bool screen_on_ = true;  // 屏幕状态标志
    esp_timer_handle_t pwr_button_timer_ = nullptr;  // 用于轮询PWR按键状态

    void InitializePowerSaveTimer() {
        // 临时禁用电源管理定时器，防止自动关机
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);  // 设置所有定时器为-1（禁用）
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(false);  // 禁用电源管理定时器
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // 检查PWR按键状态的回调函数
    static void CheckPwrButtonTimer(void* arg) {
        auto self = static_cast<WaveshareEsp32s3TouchAMOLED2inch06*>(arg);
        self->CheckPwrButtonStatus();
    }
    
    // 检查PWR按键状态（通过AXP2101）
    void CheckPwrButtonStatus() {
        // 读取AXP2101的IRQ状态寄存器1 (0x48)
        // Bit 3: PWRON短按
        // Bit 4: PWRON长按
        uint8_t irq_status1 = pmic_->GetIRQStatus1();
        
        // 检查PWRON按键短按事件
        if (irq_status1 & 0x08) {  // Bit 3: PWRON短按
            ESP_LOGI(TAG, "PWR button short press detected");
            ToggleScreen();
            // 清除IRQ标志
            pmic_->ClearIRQStatus1(0x08);
        }
        
        // 注意：长按由PMIC硬件处理关机，无需软件干预
    }
    
    void InitializeButtons() {
        // BOOT按键（GPIO0）：聊天状态切换
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "BOOT button clicked");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        // BOOT按键三击：AEC模式切换（如果启用）
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        }, 3);
#endif
        
        // 设置定时器轮询PWR按键状态（通过AXP2101）
        ESP_LOGI(TAG, "Setting up PWR button polling timer for AXP2101");
        esp_timer_create_args_t timer_args = {
            .callback = CheckPwrButtonTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pwr_button_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &pwr_button_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pwr_button_timer_, 100000));  // 100ms轮询一次
        
        // PWR按键长按4秒由PMIC硬件处理关机，无需软件干预
    }
    
    // 切换屏幕显示
    void ToggleScreen() {
        if (screen_on_) {
            // 关闭屏幕
            TurnOffScreen();
        } else {
            // 打开屏幕
            TurnOnScreen();
        }
    }
    
    // 关闭屏幕
    void TurnOffScreen() {
        if (!screen_on_) return;
        
        ESP_LOGI(TAG, "Turning off screen");
        
        // 保存当前亮度
        backlight_->SaveCurrentBrightness();
        
        // 关闭背光
        backlight_->SetBrightness(0);
        
        // 设置显示器进入省电模式
        if (display_) {
            display_->SetPowerSaveMode(true);
        }
        
        screen_on_ = false;
        
        ESP_LOGI(TAG, "Screen turned off, press power button to turn on");
    }
    
    // 打开屏幕
    void TurnOnScreen() {
        if (screen_on_) return;
        
        ESP_LOGI(TAG, "Turning on screen");
        
        // 退出省电模式
        if (display_) {
            display_->SetPowerSaveMode(false);
        }
        
        // 恢复背光亮度
        backlight_->RestoreSavedBrightness();
        
        screen_on_ = true;
        
        // 显示通知
        display_->ShowNotification("屏幕已开启", 1000);
        
        ESP_LOGI(TAG, "Screen turned on");
    }

    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x16, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        // SH8601不支持swap_xy，所以传false
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, false);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    // SD卡MCP工具实现
    std::string ListSDCardFiles(const std::string& path) {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        DIR* dir = opendir(path.c_str());
        if (!dir) {
            return "{\"success\": false, \"error\": \"Failed to open directory\"}";
        }
        
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddStringToObject(result, "path", path.c_str());
        
        cJSON* files_array = cJSON_CreateArray();
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            cJSON* file_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(file_obj, "name", entry->d_name);
            
            // 获取文件信息
            std::string full_path = path + "/" + entry->d_name;
            struct stat file_stat;
            if (stat(full_path.c_str(), &file_stat) == 0) {
                cJSON_AddBoolToObject(file_obj, "is_directory", S_ISDIR(file_stat.st_mode));
                cJSON_AddNumberToObject(file_obj, "size", file_stat.st_size);
                cJSON_AddNumberToObject(file_obj, "modified", file_stat.st_mtime);
            }
            
            cJSON_AddItemToArray(files_array, file_obj);
        }
        closedir(dir);
        
        cJSON_AddItemToObject(result, "files", files_array);
        
        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        
        return result_str;
    }
    
    std::string ReadSDCardFile(const std::string& filepath, int max_size) {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        FILE* fp = fopen(filepath.c_str(), "r");
        if (!fp) {
            return "{\"success\": false, \"error\": \"Failed to open file\"}";
        }
        
        // 获取文件大小
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        // 限制读取大小
        int read_size = (file_size > max_size) ? max_size : file_size;
        
        char* buffer = (char*)malloc(read_size + 1);
        if (!buffer) {
            fclose(fp);
            return "{\"success\": false, \"error\": \"Memory allocation failed\"}";
        }
        
        size_t bytes_read = fread(buffer, 1, read_size, fp);
        buffer[bytes_read] = '\0';
        fclose(fp);
        
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddStringToObject(result, "filepath", filepath.c_str());
        cJSON_AddNumberToObject(result, "size", file_size);
        cJSON_AddNumberToObject(result, "bytes_read", bytes_read);
        cJSON_AddStringToObject(result, "content", buffer);
        
        free(buffer);
        
        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        
        return result_str;
    }
    
    std::string WriteSDCardFile(const std::string& filepath, const std::string& content, bool append) {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        FILE* fp = fopen(filepath.c_str(), append ? "a" : "w");
        if (!fp) {
            return "{\"success\": false, \"error\": \"Failed to open file for writing\"}";
        }
        
        size_t bytes_written = fwrite(content.c_str(), 1, content.length(), fp);
        fclose(fp);
        
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddStringToObject(result, "filepath", filepath.c_str());
        cJSON_AddNumberToObject(result, "bytes_written", bytes_written);
        
        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        
        return result_str;
    }
    
    std::string DeleteSDCardFile(const std::string& path) {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) != 0) {
            return "{\"success\": false, \"error\": \"File or directory not found\"}";
        }
        
        int result;
        if (S_ISDIR(file_stat.st_mode)) {
            result = rmdir(path.c_str());
        } else {
            result = unlink(path.c_str());
        }
        
        if (result == 0) {
            return "{\"success\": true, \"message\": \"File deleted successfully\"}";
        } else {
            return "{\"success\": false, \"error\": \"Failed to delete file\"}";
        }
    }
    
    std::string GetSDCardInfo() {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        // 获取SD卡信息
        FATFS* fs;
        DWORD fre_clust, fre_sect, tot_sect;
        
        if (f_getfree("/sdcard", &fre_clust, &fs) != FR_OK) {
            return "{\"success\": false, \"error\": \"Failed to get SD card info\"}";
        }
        
        tot_sect = (fs->n_fatent - 2) * fs->csize;
        fre_sect = fre_clust * fs->csize;
        
        uint64_t total_bytes = (uint64_t)tot_sect * 512;
        uint64_t free_bytes = (uint64_t)fre_sect * 512;
        uint64_t used_bytes = total_bytes - free_bytes;
        
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddNumberToObject(result, "total_bytes", total_bytes);
        cJSON_AddNumberToObject(result, "free_bytes", free_bytes);
        cJSON_AddNumberToObject(result, "used_bytes", used_bytes);
        cJSON_AddNumberToObject(result, "total_mb", total_bytes / (1024 * 1024));
        cJSON_AddNumberToObject(result, "free_mb", free_bytes / (1024 * 1024));
        cJSON_AddNumberToObject(result, "used_mb", used_bytes / (1024 * 1024));
        
        // 获取SD卡类型信息
        if (display_->sd_card_ != nullptr) {
            cJSON_AddStringToObject(result, "name", display_->sd_card_->cid.name);
            cJSON_AddNumberToObject(result, "capacity_mb", display_->sd_card_->csd.capacity / 2048);
            cJSON_AddBoolToObject(result, "is_mmc", display_->sd_card_->is_mmc);
            cJSON_AddBoolToObject(result, "is_sdio", display_->sd_card_->is_sdio);
        }
        
        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        
        return result_str;
    }
    
    std::string DisplaySDCardImageFile(const std::string& filepath) {
        if (!display_->InitializeSDCard()) {
            return "{\"success\": false, \"error\": \"SD card not initialized\"}";
        }
        
        // 检查文件是否存在
        struct stat file_stat;
        if (stat(filepath.c_str(), &file_stat) != 0) {
            return "{\"success\": false, \"error\": \"Image file not found\"}";
        }
        
        // 检测文件格式
        std::string file_path_str(filepath);
        std::string extension = "";
        size_t dot_pos = file_path_str.rfind('.');
        if (dot_pos != std::string::npos) {
            extension = file_path_str.substr(dot_pos + 1);
            // 转换为小写
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        }
        
        std::string format = "unknown";
        if (extension == "jpg" || extension == "jpeg") {
            format = "JPEG";
        } else if (extension == "png") {
            format = "PNG";
        } else if (extension == "bmp") {
            format = "BMP";
        }
        
        // 调用显示图片的函数
        display_->DisplayJpegFile(filepath.c_str());
        
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddStringToObject(result, "message", "Image displayed successfully");
        cJSON_AddStringToObject(result, "filepath", filepath.c_str());
        cJSON_AddStringToObject(result, "format", format.c_str());
        cJSON_AddNumberToObject(result, "size", file_stat.st_size);
        
        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        
        return result_str;
    }
    
    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_9,
            .int_gpio_num = GPIO_NUM_38,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "Reboot the device and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                ResetWifiConfiguration();
                return true;
            });
        
        // 添加SD卡相关的MCP工具
        mcp_server.AddTool("self.sdcard.list_files",
            "List files and directories in the SD card.\n"
            "Args:\n"
            "  `path`: The directory path to list (default: /sdcard)\n"
            "Return:\n"
            "  A JSON object containing the list of files and directories.",
            PropertyList({
                Property("path", kPropertyTypeString, "/sdcard")
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return ListSDCardFiles(properties["path"].value<std::string>());
            });
        
        mcp_server.AddTool("self.sdcard.read_file",
            "Read the content of a text file from the SD card.\n"
            "Args:\n"
            "  `filepath`: The full path of the file to read\n"
            "  `max_size`: Maximum bytes to read (default: 4096)\n"
            "Return:\n"
            "  The content of the file as a string.",
            PropertyList({
                Property("filepath", kPropertyTypeString),
                Property("max_size", kPropertyTypeInteger, 4096, 1, 102400)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return ReadSDCardFile(properties["filepath"].value<std::string>(),
                                     properties["max_size"].value<int>());
            });
        
        mcp_server.AddTool("self.sdcard.write_file",
            "Write content to a file on the SD card.\n"
            "Args:\n"
            "  `filepath`: The full path of the file to write\n"
            "  `content`: The content to write to the file\n"
            "  `append`: Whether to append to existing file (default: false)\n"
            "Return:\n"
            "  Success status message.",
            PropertyList({
                Property("filepath", kPropertyTypeString),
                Property("content", kPropertyTypeString),
                Property("append", kPropertyTypeBoolean, false)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return WriteSDCardFile(properties["filepath"].value<std::string>(),
                                      properties["content"].value<std::string>(),
                                      properties["append"].value<bool>());
            });
        
        mcp_server.AddTool("self.sdcard.delete_file",
            "Delete a file or empty directory from the SD card.\n"
            "Args:\n"
            "  `path`: The full path of the file or directory to delete\n"
            "Return:\n"
            "  Success status message.",
            PropertyList({
                Property("path", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return DeleteSDCardFile(properties["path"].value<std::string>());
            });
        
        mcp_server.AddTool("self.sdcard.get_info",
            "Get SD card information including capacity and usage.\n"
            "Return:\n"
            "  A JSON object containing SD card information.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return GetSDCardInfo();
            });
        
        mcp_server.AddTool("self.sdcard.display_image",
            "Display an image file from the SD card on the screen.\n"
            "Supported formats: JPEG (.jpg, .jpeg), PNG (.png), BMP (.bmp)\n"
            "Args:\n"
            "  `filepath`: The full path of the image file\n"
            "Return:\n"
            "  Success status message with format info.",
            PropertyList({
                Property("filepath", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return DisplaySDCardImageFile(properties["filepath"].value<std::string>());
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch06() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeSpi();
        InitializeSH8601Display();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }
    
    ~WaveshareEsp32s3TouchAMOLED2inch06() {
        if (pwr_button_timer_) {
            esp_timer_stop(pwr_button_timer_);
            esp_timer_delete(pwr_button_timer_);
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            // 临时禁用：即使在电池供电模式下也不启用电源管理
            // power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled)
        {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch06);
