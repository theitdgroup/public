// LVGL allocator that keeps all UI objects in PSRAM. Internal RAM is scarce and
// mbedTLS (news, weather, calendar) needs ~45 KB of it per HTTPS connection.
#include <lvgl.h>
#include <esp_heap_caps.h>

#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAPS_ANY   (MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void* lv_malloc_core(size_t size) {
  void* p = heap_caps_malloc(size, CAPS_PSRAM);
  return p ? p : heap_caps_malloc(size, CAPS_ANY);
}

void* lv_realloc_core(void* p, size_t new_size) {
  void* r = heap_caps_realloc(p, new_size, CAPS_PSRAM);
  return r ? r : heap_caps_realloc(p, new_size, CAPS_ANY);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
