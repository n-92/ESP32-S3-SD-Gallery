/* Author: Aung Naing Oo
 * SD GALLERY and TXT reader: SMART SCALE EDITION
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <JPEGDEC.h> 

#include <esp_display_panel.hpp> 
#include "lvgl_v8_port.h"
#include "waveshare_sd_card.h"

extern "C" void Cache_WriteBack_Addr(uint32_t addr, uint32_t size);
using namespace esp_panel::drivers;
using namespace esp_panel::board;

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 480
#define LOAD_BUFFER_SIZE 4096 

// --- FONT CONFIG ---
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

const lv_font_t * font_levels[] = { &lv_font_montserrat_14, &lv_font_montserrat_20, &lv_font_montserrat_28 };
int current_font_idx = 2; // Size 28

Board *board = nullptr;
JPEGDEC jpeg;
String current_path = "/"; 
uint16_t *framebuffer = NULL; 

// --- PAGING GLOBALS ---
String g_txt_filename = "";     
size_t g_file_offset = 0;       
size_t g_total_file_size = 0; 

// History Stack
std::vector<size_t> g_page_history; 
int g_history_index = 0;

// UI Objects
lv_obj_t * scr_browser;
lv_obj_t * list_filemanager;
lv_obj_t * label_path;

lv_obj_t * scr_text;
lv_obj_t * ta_text_viewer;
lv_obj_t * btn_text_back;
lv_obj_t * label_text_filename;
lv_obj_t * label_debug; 

lv_obj_t * btn_font_inc;
lv_obj_t * btn_font_dec;
lv_obj_t * btn_page_prev; 
lv_obj_t * btn_page_next; 

int g_scaled_width = 0;
int g_scaled_height = 0;

// ================= PROTOTYPES =================
void load_browser(String path);
void view_image_smart(String filename);
void view_text_initial(String filename);
void load_text_at_offset(size_t offset);

// ================= SPLASH SCREEN =================
void show_splash() {
   // 1. Create a black screen
    lv_obj_t * scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(0x000000), 0);
    lv_scr_load(scr_splash);

    // 2. Main Title (Moved UP)
    lv_obj_t * title = lv_label_create(scr_splash);
    lv_label_set_text(title, "N-92 ESP32 READER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0); // Use Size 28
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0); // Cyan
    // Align Center, but shift UP by 30 pixels
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30); 

    // 3. Subtitle (Anchored BELOW Title)
    lv_obj_t * sub = lv_label_create(scr_splash);
    lv_label_set_text(sub, "Ultimate Edition v1.0");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0); // Use Size 20
    lv_obj_set_style_text_color(sub, lv_color_white(), 0); // White
    
    // This line prevents overlap: Align OUTSIDE the bottom of 'title', with 10px spacing
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    // 4. Loading Spinner
    lv_obj_t * spin = lv_spinner_create(scr_splash, 1000, 60);
    lv_obj_set_size(spin, 40, 40);
    lv_obj_align(spin, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x00FFFF), LV_PART_INDICATOR); 
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x333333), LV_PART_MAIN);     

    // 5. Hold for 6 Seconds
    for(int i=0; i<60; i++) { 
        lv_timer_handler();
        delay(100);
    }
}

// ================= JPEG ENGINE =================
int JPEGDraw(JPEGDRAW *pDraw) {
    int off_x = (SCREEN_WIDTH - g_scaled_width) / 2;
    int off_y = (SCREEN_HEIGHT - g_scaled_height) / 2;

    for (int y = 0; y < pDraw->iHeight; y++) {
        int screen_y = pDraw->y + y + off_y;
        if (screen_y < 0 || screen_y >= SCREEN_HEIGHT) continue;
        int screen_x_start = pDraw->x + off_x;
        int copy_w = pDraw->iWidth;
        uint16_t * src = pDraw->pPixels + (y * pDraw->iWidth);
        if (screen_x_start < 0) { src -= screen_x_start; copy_w += screen_x_start; screen_x_start = 0; }
        if (screen_x_start + copy_w > SCREEN_WIDTH) copy_w = SCREEN_WIDTH - screen_x_start;
        if (copy_w > 0) {
            uint16_t * dst = &framebuffer[(screen_y * SCREEN_WIDTH) + screen_x_start];
            memcpy(dst, src, copy_w * 2);
        }
    }
    return 1;
}

void show_status(uint16_t color) {
    for(int i=0; i<SCREEN_WIDTH*SCREEN_HEIGHT; i++) framebuffer[i] = color;
    Cache_WriteBack_Addr((uint32_t)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    board->getLCD()->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (const uint8_t*)framebuffer);
}

// ================= FONT LOGIC =================
static void font_inc_cb(lv_event_t * e) {
    if (current_font_idx < 2) { 
        current_font_idx++;
        lv_obj_set_style_text_font(ta_text_viewer, font_levels[current_font_idx], 0);
        load_text_at_offset(g_file_offset);
    }
}
static void font_dec_cb(lv_event_t * e) {
    if (current_font_idx > 0) { 
        current_font_idx--;
        lv_obj_set_style_text_font(ta_text_viewer, font_levels[current_font_idx], 0);
        load_text_at_offset(g_file_offset);
    }
}

// ================= VISUAL PAGINATION LOGIC =================

void load_text_at_offset(size_t offset) {
    File f = SD.open(g_txt_filename, FILE_READ);
    if (!f) return;

    if (offset >= g_total_file_size) offset = g_total_file_size - 1;
    f.seek(offset);

    size_t bytes_to_read = LOAD_BUFFER_SIZE; 
    if (offset + bytes_to_read > g_total_file_size) bytes_to_read = g_total_file_size - offset;

    char *text_buff = (char*)heap_caps_malloc(bytes_to_read + 1, MALLOC_CAP_SPIRAM);
    if (!text_buff) { f.close(); return; }

    f.read((uint8_t*)text_buff, bytes_to_read);
    text_buff[bytes_to_read] = 0; 
    f.close();

    lv_textarea_set_text(ta_text_viewer, text_buff);
    
    // Always start at top
    lv_obj_scroll_to_y(ta_text_viewer, 0, LV_ANIM_OFF);
    lv_obj_update_layout(ta_text_viewer);

    free(text_buff);

    lv_label_set_text_fmt(label_debug, "Offset: %d / %d", offset, g_total_file_size);
    g_file_offset = offset;
}

// NEXT BUTTON: Calculates visual cutoff & Updates History
static void btn_next_cb(lv_event_t * e) {
    // 1. Calculate where the current page ends
    lv_obj_t * label = lv_textarea_get_label(ta_text_viewer);
    lv_coord_t h = lv_obj_get_height(ta_text_viewer);
    lv_point_t p; p.x = 0; p.y = h - 20; // Check bottom-left
    
    uint32_t char_index = lv_label_get_letter_on(label, &p);
    size_t next_offset = 0;

    if (char_index > 0 && char_index < LOAD_BUFFER_SIZE) {
        next_offset = g_file_offset + char_index;
    } else {
        next_offset = g_file_offset + 500; // Fallback
    }

    // 2. Safety Check
    if (next_offset >= g_total_file_size) return; // End of book

    // 3. UPDATE HISTORY
    // If we were going back and forth, truncate the "future" history
    if (g_page_history.size() > (g_history_index + 1)) {
        g_page_history.resize(g_history_index + 1);
    }
    
    // Add new page to history
    g_page_history.push_back(next_offset);
    g_history_index++;

    // 4. Load
    load_text_at_offset(next_offset);
}

// PREV BUTTON: Uses History Stack
static void btn_prev_cb(lv_event_t * e) {
    // 1. Check if we have history
    if (g_history_index > 0) {
        // Go back one step in history
        g_history_index--;
        size_t prev_offset = g_page_history[g_history_index];
        load_text_at_offset(prev_offset);
    } else {
        // Already at start
        if (g_file_offset != 0) load_text_at_offset(0);
    }
}

void view_text_initial(String filename) {
    String full_path = current_path;
    if (!full_path.endsWith("/")) full_path += "/";
    full_path += filename;
    
    File f = SD.open(full_path, FILE_READ);
    if (!f) return;
    g_total_file_size = f.size();
    g_txt_filename = full_path;
    f.close();

    lv_scr_load(scr_text);
    
    current_font_idx = 2; // Default Size 28
    lv_obj_set_style_text_font(ta_text_viewer, font_levels[current_font_idx], 0);
    
    // Reset History
    g_page_history.clear();
    g_page_history.push_back(0); // Start at 0
    g_history_index = 0;

    load_text_at_offset(0); 
}

// ================= IMAGE VIEWER & BROWSER (UNCHANGED) =================

void view_image_smart(String filename) {
    String full_path = current_path;
    if (!full_path.endsWith("/")) full_path += "/";
    full_path += filename;
    show_status(0xFFE0); 

    File f = SD.open(full_path, FILE_READ);
    if (!f) { show_status(0xF800); delay(1000); return; }

    size_t fileSize = f.size();
    if (fileSize > 8 * 1024 * 1024) { f.close(); show_status(0xF800); delay(1000); return; }

    uint8_t *mem_buff = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!mem_buff) { f.close(); show_status(0xF800); delay(1000); return; }
    f.read(mem_buff, fileSize);
    f.close();
    Cache_WriteBack_Addr((uint32_t)mem_buff, fileSize);

    memset(framebuffer, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);

    if (jpeg.openRAM(mem_buff, fileSize, JPEGDraw)) {
        int w = jpeg.getWidth(); int h = jpeg.getHeight();
        int scale = 0; 
        if (w > 3000 || h > 3000)      { scale = JPEG_SCALE_EIGHTH; g_scaled_width = w/8; g_scaled_height = h/8; }
        else if (w > 1500 || h > 1500) { scale = JPEG_SCALE_QUARTER; g_scaled_width = w/4; g_scaled_height = h/4; }
        else if (w > 800 || h > 600)   { scale = JPEG_SCALE_HALF;    g_scaled_width = w/2; g_scaled_height = h/2; }
        else                           { scale = 0;                  g_scaled_width = w;   g_scaled_height = h;   }

        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        if (!jpeg.decode(0, 0, scale)) show_status(0x001F); 
        jpeg.close();
    } else { show_status(0xF800); }

    Cache_WriteBack_Addr((uint32_t)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    board->getLCD()->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (const uint8_t*)framebuffer);
    free(mem_buff);

    TouchPoint point;
    while(board->getTouch()->readPoints(&point, 1) > 0) delay(50);
    while(true) { if (board->getTouch()->readPoints(&point, 1) > 0) break; delay(50); }
    while(board->getTouch()->readPoints(&point, 1) > 0) delay(50);
    lv_obj_invalidate(lv_scr_act()); 
}

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
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) view_image_smart(selected_name);
    else if (lower.endsWith(".txt") || lower.endsWith(".log") || lower.endsWith(".ino")) view_text_initial(selected_name);
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
            if (file.isDirectory()) items.push_back({fname, true});
            else {
                String lo = fname; lo.toLowerCase();
                if (lo.endsWith(".jpg") || lo.endsWith(".jpeg") || lo.endsWith(".txt") || lo.endsWith(".log") || lo.endsWith(".ino")) items.push_back({fname, false});
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
            String lo = it.name; lo.toLowerCase();
            if(lo.endsWith(".txt") || lo.endsWith(".log")) {
                btn = lv_list_add_btn(list_filemanager, LV_SYMBOL_FILE, it.name.c_str());
                lv_obj_set_style_text_color(btn, lv_color_hex(0x00FF00), 0); 
            } else {
                btn = lv_list_add_btn(list_filemanager, LV_SYMBOL_IMAGE, it.name.c_str());
                lv_obj_set_style_text_color(btn, lv_color_white(), 0);
            }
        }
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_add_event_cb(btn, browser_event_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void text_back_cb(lv_event_t * e) {
    lv_scr_load(scr_browser);
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

    // --- 1. SHOW SPLASH SCREEN ---
    show_splash();

    // --- 2. INIT UI ---
    
    // Browser
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

    // Text Viewer
    scr_text = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_text, lv_color_hex(0x000000), 0);

    // Text Area
    ta_text_viewer = lv_textarea_create(scr_text);
    lv_obj_set_size(ta_text_viewer, 700, 400); 
    lv_obj_align(ta_text_viewer, LV_ALIGN_TOP_LEFT, 10, 60); 
    lv_obj_set_style_bg_color(ta_text_viewer, lv_color_hex(0x101010), 0);
    lv_obj_set_style_text_color(ta_text_viewer, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(ta_text_viewer, &lv_font_montserrat_28, 0); // Default 28
    
    // Disable Touch Dragging & Scrollbars
    lv_obj_clear_flag(ta_text_viewer, LV_OBJ_FLAG_CLICKABLE); 
    lv_obj_set_scrollbar_mode(ta_text_viewer, LV_SCROLLBAR_MODE_OFF); 

    // Back
    btn_text_back = lv_btn_create(scr_text);
    lv_obj_set_size(btn_text_back, 100, 40);
    lv_obj_align(btn_text_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(btn_text_back, lv_color_hex(0xFF0000), 0);
    lv_label_set_text(lv_label_create(btn_text_back), LV_SYMBOL_LEFT " BACK");
    lv_obj_add_event_cb(btn_text_back, text_back_cb, LV_EVENT_CLICKED, NULL);

    // Label
    label_text_filename = lv_label_create(scr_text);
    lv_obj_align(label_text_filename, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(label_text_filename, lv_color_hex(0xFFD700), 0);
    // [FIXED] Pass the text "filename.txt" NOT the object scr_text
    lv_label_set_text(label_text_filename, "filename.txt");
    
    // Debug Label
    label_debug = lv_label_create(scr_text);
    lv_obj_align(label_debug, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_set_style_text_color(label_debug, lv_color_hex(0x555555), 0);
    lv_label_set_text(label_debug, "Offset: 0");

    // --- BUTTONS ---

    // Font +
    btn_font_inc = lv_btn_create(scr_text);
    lv_obj_set_size(btn_font_inc, 70, 70);
    lv_obj_align(btn_font_inc, LV_ALIGN_TOP_RIGHT, -5, 50);
    lv_obj_set_style_bg_color(btn_font_inc, lv_color_hex(0x444444), 0);
    lv_label_set_text(lv_label_create(btn_font_inc), "A+");
    lv_obj_add_event_cb(btn_font_inc, font_inc_cb, LV_EVENT_CLICKED, NULL);

    // Font -
    btn_font_dec = lv_btn_create(scr_text);
    lv_obj_set_size(btn_font_dec, 70, 70);
    lv_obj_align(btn_font_dec, LV_ALIGN_TOP_RIGHT, -5, 130);
    lv_obj_set_style_bg_color(btn_font_dec, lv_color_hex(0x444444), 0);
    lv_label_set_text(lv_label_create(btn_font_dec), "A-");
    lv_obj_add_event_cb(btn_font_dec, font_dec_cb, LV_EVENT_CLICKED, NULL);

    // Prev Page
    btn_page_prev = lv_btn_create(scr_text);
    lv_obj_set_size(btn_page_prev, 70, 90);
    lv_obj_align(btn_page_prev, LV_ALIGN_BOTTOM_RIGHT, -5, -100);
    lv_obj_set_style_bg_color(btn_page_prev, lv_color_hex(0x660000), 0); 
    lv_label_set_text(lv_label_create(btn_page_prev), LV_SYMBOL_UP); 
    lv_obj_add_event_cb(btn_page_prev, btn_prev_cb, LV_EVENT_CLICKED, NULL);

    // Next Page
    btn_page_next = lv_btn_create(scr_text);
    lv_obj_set_size(btn_page_next, 70, 90);
    lv_obj_align(btn_page_next, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_set_style_bg_color(btn_page_next, lv_color_hex(0x006600), 0); 
    lv_label_set_text(lv_label_create(btn_page_next), LV_SYMBOL_DOWN); 
    lv_obj_add_event_cb(btn_page_next, btn_next_cb, LV_EVENT_CLICKED, NULL);

    // Go to Browser
    load_browser("/");
    lv_scr_load(scr_browser);
    lvgl_port_unlock();
}

void loop() { delay(1000); }