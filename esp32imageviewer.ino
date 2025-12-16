/* Author: Aung Naing Oo
 * SD GALLERY: SMART SCALE EDITION
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <JPEGDEC.h> 

// --- BOARD HEADERS ---
#include <esp_display_panel.hpp> 
#include "lvgl_v8_port.h"
#include "waveshare_sd_card.h"

// ROM Function to flush cache
extern "C" void Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// ================= CONFIGURATION =================
#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 480

Board *board = nullptr;
JPEGDEC jpeg;
String current_path = "/"; // Start at Root
uint16_t *framebuffer = NULL; 

// UI Objects
lv_obj_t * scr_browser;
lv_obj_t * list_filemanager;
lv_obj_t * label_path;

// Global to store the *calculated* size for centering
int g_scaled_width = 0;
int g_scaled_height = 0;

// ================= PROTOTYPES =================
void load_browser(String path);
void view_image_smart(String filename);

// ================= PIXEL PUMPER =================
int JPEGDraw(JPEGDRAW *pDraw) {
    // USE THE SCALED DIMENSIONS FOR CENTERING
    int off_x = (SCREEN_WIDTH - g_scaled_width) / 2;
    int off_y = (SCREEN_HEIGHT - g_scaled_height) / 2;

    for (int y = 0; y < pDraw->iHeight; y++) {
        int screen_y = pDraw->y + y + off_y;
        if (screen_y < 0 || screen_y >= SCREEN_HEIGHT) continue;

        int screen_x_start = pDraw->x + off_x;
        int copy_w = pDraw->iWidth;
        
        uint16_t * src = pDraw->pPixels + (y * pDraw->iWidth);
        uint16_t * dst = &framebuffer[(screen_y * SCREEN_WIDTH) + screen_x_start];

        if (screen_x_start < 0) { 
            src -= screen_x_start; 
            dst -= screen_x_start; 
            copy_w += screen_x_start; 
        }
        if (screen_x_start + copy_w > SCREEN_WIDTH) {
            copy_w = SCREEN_WIDTH - screen_x_start;
        }

        if (copy_w > 0) {
            memcpy(dst, src, copy_w * 2);
        }
    }
    return 1;
}

// ================= DEBUG FILL =================
void debug_fill(uint16_t color) {
    for(int i=0; i<SCREEN_WIDTH*SCREEN_HEIGHT; i++) framebuffer[i] = color;
    Cache_WriteBack_Addr((uint32_t)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    board->getLCD()->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (const uint8_t*)framebuffer);
}

// ================= VIEWER LOGIC (SMART SCALE) =================
void view_image_smart(String filename) {
    String full_path = current_path;
    if (!full_path.endsWith("/")) full_path += "/";
    full_path += filename;
    
    Serial.println("Viewing: " + full_path);

    // 1. Start Blue (Loading)
    debug_fill(0x001F); 

    File jpgFile = SD.open(full_path, FILE_READ);
    if (!jpgFile) {
        debug_fill(0xF800); // RED = Error
        delay(1000);
        return; 
    }

    size_t fileSize = jpgFile.size();
    
    // Safety: Cap at 6MB buffer
    if (fileSize > 6 * 1024 * 1024) {
         Serial.println("File too massive for RAM");
         debug_fill(0xF800);
         jpgFile.close();
         delay(1000);
         return;
    }

    // 2. Load to RAM
    uint8_t *jpg_data = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!jpg_data) {
        Serial.println("Malloc Fail");
        debug_fill(0xF800); 
        jpgFile.close();
        delay(1000);
        return;
    }
    
    jpgFile.read(jpg_data, fileSize);
    jpgFile.close();

    // Cache Flush Source Buffer
    Cache_WriteBack_Addr((uint32_t)jpg_data, fileSize);

    // Clear Screen to Black
    memset(framebuffer, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    Cache_WriteBack_Addr((uint32_t)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    board->getLCD()->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (const uint8_t*)framebuffer);

    // 3. Open & Calculate Scale
    if (jpeg.openRAM(jpg_data, fileSize, JPEGDraw)) {
        
        int w = jpeg.getWidth();
        int h = jpeg.getHeight();
        int scale = 0; // Default 1:1

        // --- SMART SCALING LOGIC ---
        if (w > 3000 || h > 3000) {
            scale = JPEG_SCALE_EIGHTH; // 1/8
            g_scaled_width = w / 8;
            g_scaled_height = h / 8;
            Serial.println("Scale: 1/8");
        } 
        else if (w > 1500 || h > 1500) {
            scale = JPEG_SCALE_QUARTER; // 1/4
            g_scaled_width = w / 4;
            g_scaled_height = h / 4;
            Serial.println("Scale: 1/4");
        }
        else if (w > 800 || h > 600) {
            scale = JPEG_SCALE_HALF; // 1/2
            g_scaled_width = w / 2;
            g_scaled_height = h / 2;
            Serial.println("Scale: 1/2");
        }
        else {
            scale = 0; // 1:1
            g_scaled_width = w;
            g_scaled_height = h;
            Serial.println("Scale: 1:1");
        }

        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        
        // Pass the 'scale' parameter here!
        if (jpeg.decode(0, 0, scale)) {
            // Success
            Cache_WriteBack_Addr((uint32_t)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
            board->getLCD()->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (const uint8_t*)framebuffer);
        } else {
             Serial.println("Decode Failed (Might be Progressive JPEG)");
             debug_fill(0xF800); // Red
        }
        jpeg.close();
        
    } else {
        Serial.println("OpenRAM Failed");
        debug_fill(0xF800); 
    }

    free(jpg_data);

    // Wait for touch
    TouchPoint point;
    while(board->getTouch()->readPoints(&point, 1) > 0) delay(50);
    while(true) {
        if (board->getTouch()->readPoints(&point, 1) > 0) break;
        delay(50);
    }
    while(board->getTouch()->readPoints(&point, 1) > 0) delay(50);
    
    lv_obj_invalidate(lv_scr_act());
}

// ================= BROWSER LOGIC =================
static void browser_event_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    const char * txt = lv_list_get_btn_text(list_filemanager, btn);
    String selected_name = String(txt);
    
    if (selected_name == "..") {
        int last_slash = current_path.lastIndexOf('/');
        if (last_slash <= 0) current_path = "/";
        else current_path = current_path.substring(0, last_slash);
        load_browser(current_path);
        return;
    }

    String full_path = current_path;
    if (!full_path.endsWith("/")) full_path += "/";
    full_path += selected_name;

    File check = SD.open(full_path);
    if (check) {
        bool isDir = check.isDirectory();
        check.close();
        if (isDir) {
            current_path = full_path;
            load_browser(current_path);
            return;
        }
    }

    String lower = selected_name; lower.toLowerCase();
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        view_image_smart(selected_name);
    }
}

void load_browser(String path) {
    lv_obj_clean(list_filemanager);
    lv_label_set_text_fmt(label_path, "DIR: %s", path.c_str());

    if (path != "/") {
        lv_obj_t * btn = lv_list_add_btn(list_filemanager, LV_SYMBOL_UP, "..");
        lv_obj_add_event_cb(btn, browser_event_cb, LV_EVENT_CLICKED, NULL);
    }

    File dir = SD.open(path);
    if (!dir) return;

    struct Item { String name; bool isDir; };
    std::vector<Item> items;

    File file = dir.openNextFile();
    while (file) {
        String fname = String(file.name());
        if (!fname.startsWith(".")) {
            if (file.isDirectory()) {
                items.push_back({fname, true});
            } else {
                String lo = fname; lo.toLowerCase();
                if (lo.endsWith(".jpg") || lo.endsWith(".jpeg")) {
                    items.push_back({fname, false});
                }
            }
        }
        file = dir.openNextFile();
    }

    std::sort(items.begin(), items.end(), [](Item a, Item b){
        if (a.isDir != b.isDir) return a.isDir;
        return a.name < b.name;
    });

    for (auto &it : items) {
        lv_obj_t * btn;
        if (it.isDir) {
            btn = lv_list_add_btn(list_filemanager, LV_SYMBOL_DIRECTORY, it.name.c_str());
            lv_obj_set_style_text_color(btn, lv_color_hex(0xFFD700), 0);
        } else {
            btn = lv_list_add_btn(list_filemanager, LV_SYMBOL_IMAGE, it.name.c_str());
            lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        }
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_add_event_cb(btn, browser_event_cb, LV_EVENT_CLICKED, NULL);
    }
}

void setup() {
    Serial.begin(115200);
    board = new Board();
    board->init();
    board->begin();
    lvgl_port_init(board->getLCD(), board->getTouch());
    lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE); 
    auto expander = static_cast<esp_expander::CH422G*>(board->getIO_Expander()->getBase());
    expander->digitalWrite(SD_CS, LOW);
    SPI.setHwCs(false);
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_SS);
    SD.begin(SD_SS);

    framebuffer = (uint16_t *)heap_caps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!framebuffer) while(1);

    lvgl_port_lock(-1);
    scr_browser = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_browser, lv_color_hex(0x101010), 0);
    label_path = lv_label_create(scr_browser);
    lv_obj_align(label_path, LV_ALIGN_TOP_LEFT, 10, 5);
    lv_label_set_text(label_path, "DIR: /");
    lv_obj_set_style_text_color(label_path, lv_color_hex(0x00FF00), 0);
    list_filemanager = lv_list_create(scr_browser);
    lv_obj_set_size(list_filemanager, 800, 440);
    lv_obj_align(list_filemanager, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list_filemanager, lv_color_hex(0x000000), 0);
    load_browser("/");
    lv_scr_load(scr_browser);
    lvgl_port_unlock();
}

void loop() {
    delay(1000);
}