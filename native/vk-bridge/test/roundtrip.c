/* vk-bridge v0 host roundtrip test: fill -> encode -> decode -> deep-compare, per cmd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkb_marshal.h"

static void vkb_fill_bytes(void *p, size_t n, uint32_t *seed) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((*seed)++ * 37u + 11u);
}

static const char *vkb_fill_str(vkb_arena *a, uint32_t *seed) {
    char *s = vkb_arena_alloc(a, 32);
    snprintf(s, 32, "lodos-%u", (unsigned)(*seed)++);
    return s;
}

static int vkb_streq(const char *x, const char *y) {
    if (!x || !y) return x == y;
    return strcmp(x, y) == 0;
}

#include "vkb_test_gen.c"

static uint8_t fill_mem[1 << 20], dec_mem[1 << 20], buf[1 << 20];

/* hand-written checks independent of generated eq_* helpers */
static int manual_create_instance(void) {
    vkb_arena da = { dec_mem, sizeof dec_mem, 0 };
    const char *layers[] = { "VK_LAYER_lodos" };
    const char *exts[] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "LodOS", 7, "wine", 9, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 1, layers, 2, exts };
    VkInstance inst = (VkInstance)(uintptr_t)0x1122334455667788ull;
    size_t n = vkb_enc_vkCreateInstance(buf, &ci, NULL, &inst);
    const VkInstanceCreateInfo *dci; const VkAllocationCallbacks *dal; VkInstance *dinst;
    if (vkb_dec_vkCreateInstance(buf, &da, &dci, &dal, &dinst) != n) return 0;
    return dal == NULL && dci->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO && dci->pNext == NULL &&
           strcmp(dci->pApplicationInfo->pApplicationName, "LodOS") == 0 &&
           strcmp(dci->pApplicationInfo->pEngineName, "wine") == 0 &&
           dci->pApplicationInfo->apiVersion == VK_API_VERSION_1_3 &&
           dci->enabledLayerCount == 1 && strcmp(dci->ppEnabledLayerNames[0], "VK_LAYER_lodos") == 0 &&
           dci->enabledExtensionCount == 2 && strcmp(dci->ppEnabledExtensionNames[1], "VK_KHR_xlib_surface") == 0 &&
           *dinst == inst;
}

/* NULL output array path (count query) */
static int manual_enum_null(void) {
    vkb_arena da = { dec_mem, sizeof dec_mem, 0 };
    VkInstance inst = (VkInstance)(uintptr_t)0xabcull;
    uint32_t cnt = 5;
    size_t n = vkb_enc_vkEnumeratePhysicalDevices(buf, inst, &cnt, NULL);
    VkInstance di; uint32_t *dc; VkPhysicalDevice *dp;
    if (vkb_dec_vkEnumeratePhysicalDevices(buf, &da, &di, &dc, &dp) != n) return 0;
    return di == inst && dc && *dc == 5 && dp == NULL;
}

int main(void) {
    int fails = 0, oks = 0;
    for (size_t i = 0; i < VKB_RT_COUNT; i++) {
        vkb_arena fa = { fill_mem, sizeof fill_mem, 0 }, da = { dec_mem, sizeof dec_mem, 0 };
        uint32_t seed = 1 + (uint32_t)i * 1000;
        if (vkb_rt_table[i].fn(&fa, &da, buf, sizeof buf, &seed)) {
            printf("ROUNDTRIP %s ok\n", vkb_rt_table[i].name);
            oks++;
        } else {
            printf("ROUNDTRIP %s FAIL\n", vkb_rt_table[i].name);
            fails++;
        }
    }
    if (!manual_create_instance()) { printf("MANUAL vkCreateInstance FAIL\n"); fails++; }
    else printf("MANUAL vkCreateInstance ok\n");
    if (!manual_enum_null()) { printf("MANUAL vkEnumeratePhysicalDevices(NULL) FAIL\n"); fails++; }
    else printf("MANUAL vkEnumeratePhysicalDevices(NULL) ok\n");
    if (fails) { printf("ROUNDTRIP_ALL FAIL fails=%d\n", fails); return 1; }
    printf("ROUNDTRIP_ALL ok n=%d\n", oks);
    return 0;
}
