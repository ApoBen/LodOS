/* T018 e2e: client process -> vkb_server (other process) -> real driver, 6 bridged cmds.
 * prints: E2E device=<name> api=<maj.min.patch> qfam=<n>, latency, negative tests, then E2E ok. */
#define _GNU_SOURCE
#include "vkbc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIE(...) do { fprintf(stderr, "E2E FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ < 0) DIE("%s -> %d", #x, (int)r_); } while (0)
#define LAT_N 1000

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : NULL;
    vkbc *c = NULL;
    for (int i = 0; i < 50 && !c; i++) { /* server may still be starting */
        c = vkbc_connect(name);
        if (!c) { struct timespec ts = { 0, 100 * 1000000 }; nanosleep(&ts, NULL); }
    }
    if (!c) DIE("connect: %s", strerror(errno));

    uint32_t iver = 0;
    VK(vkbc_vkEnumerateInstanceVersion(c, &iver));
    printf("E2E instance_version=%u.%u.%u\n", VK_API_VERSION_MAJOR(iver), VK_API_VERSION_MINOR(iver), VK_API_VERSION_PATCH(iver));

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "lodos-e2e",
                              .applicationVersion = 1, .pEngineName = "lodos", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };

    /* negative: pNext chains rejected by server */
    VkValidationFeaturesEXT bogus = { .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
    ci.pNext = &bogus;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkbc_vkCreateInstance(c, &ci, NULL, &inst);
    if (r != VK_ERROR_FEATURE_NOT_PRESENT) DIE("pNext not rejected (res=%d)", (int)r);
    printf("E2E neg pNext -> VK_ERROR_FEATURE_NOT_PRESENT ok\n");
    ci.pNext = NULL;

    VK(vkbc_vkCreateInstance(c, &ci, NULL, &inst));
    if (!inst) DIE("null instance id");

    uint32_t n = 0;
    VK(vkbc_vkEnumeratePhysicalDevices(c, inst, &n, NULL));
    if (!n) DIE("no physical devices");
    VkPhysicalDevice pds[8] = { 0 };
    if (n > 8) n = 8;
    VK(vkbc_vkEnumeratePhysicalDevices(c, inst, &n, pds));
    printf("E2E physdevs=%u ids:", n);
    for (uint32_t i = 0; i < n; i++) printf(" %llu", (unsigned long long)(uintptr_t)pds[i]);
    printf("\n");

    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof props);
    VK(vkbc_vkGetPhysicalDeviceProperties(c, pds[0], &props));

    uint32_t nq = 0;
    VK(vkbc_vkGetPhysicalDeviceQueueFamilyProperties(c, pds[0], &nq, NULL));
    VkQueueFamilyProperties qf[16];
    uint32_t nq2 = nq > 16 ? 16 : nq;
    VK(vkbc_vkGetPhysicalDeviceQueueFamilyProperties(c, pds[0], &nq2, qf));
    for (uint32_t i = 0; i < nq2; i++)
        printf("E2E qfam[%u] flags=0x%x count=%u\n", i, qf[i].queueFlags, qf[i].queueCount);

    printf("E2E device=%s api=%u.%u.%u qfam=%u vendor=0x%x driver=0x%x\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion), nq, props.vendorID, props.driverVersion);

    /* negative: unknown handle id rejected server-side */
    VkPhysicalDeviceProperties junk;
    r = vkbc_vkGetPhysicalDeviceProperties(c, (VkPhysicalDevice)(uintptr_t)0xdead, &junk);
    if (r != VK_ERROR_UNKNOWN) DIE("bad handle not rejected (res=%d)", (int)r);
    printf("E2E neg bad-handle -> VK_ERROR_UNKNOWN ok\n");

    /* latency: per-call roundtrip of vkGetPhysicalDeviceProperties */
    static double lat[LAT_N];
    memset(&junk, 0, sizeof junk); /* padding untouched by decode */
    for (int i = 0; i < 100; i++) VK(vkbc_vkGetPhysicalDeviceProperties(c, pds[0], &junk));
    double t0 = now_us();
    for (int i = 0; i < LAT_N; i++) {
        double a = now_us();
        VK(vkbc_vkGetPhysicalDeviceProperties(c, pds[0], &junk));
        lat[i] = now_us() - a;
    }
    double tot = now_us() - t0;
    if (memcmp(&junk, &props, sizeof props)) DIE("props differ between calls");
    qsort(lat, LAT_N, sizeof lat[0], cmpd);
    printf("E2E lat vkGetPhysicalDeviceProperties n=%d avg=%.2fus p50=%.2fus p99=%.2fus max=%.2fus\n",
           LAT_N, tot / LAT_N, lat[LAT_N / 2], lat[LAT_N * 99 / 100], lat[LAT_N - 1]);

    VK(vkbc_vkDestroyInstance(c, inst, NULL));
    /* destroyed id must be gone from the server table */
    n = 0;
    r = vkbc_vkEnumeratePhysicalDevices(c, inst, &n, NULL);
    if (r != VK_ERROR_UNKNOWN) DIE("destroyed instance still mapped (res=%d)", (int)r);
    vkbc_disconnect(c);
    printf("E2E ok\n");
    return 0;
}
