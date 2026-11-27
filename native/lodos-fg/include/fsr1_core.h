#ifndef LODOS_FSR1_CORE_H
#define LODOS_FSR1_CORE_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Scale is input pixels per output pixel: quality 1.5x, balanced 1.7x, performance 2x. */
typedef enum { LODOS_FSR1_QUALITY, LODOS_FSR1_BALANCED, LODOS_FSR1_PERFORMANCE } LodosFsr1Preset;
static const float LODOS_FSR1_SCALE[] = {1.5f, 1.7f, 2.0f};
typedef struct {
 VkDevice device; VkDescriptorSetLayout set_layout; VkPipelineLayout pipeline_layout;
 VkPipeline easu, rcas; VkDescriptorPool pool; VkSampler sampler;
 VkDescriptorSet easu_set, rcas_set; uint32_t out_w,out_h;
} LodosFsr1;
VkResult lodos_fsr1_create(LodosFsr1*, VkDevice, const char *easu_spv, const char *rcas_spv);
void lodos_fsr1_destroy(LodosFsr1*);
/* Images must be RGBA8 UNORM and in GENERAL layout; intermediate is output-sized. */
VkResult lodos_fsr1_record(LodosFsr1*, VkCommandBuffer, VkImageView src, VkImageView intermediate,
                           VkImageView dst, uint32_t src_w,uint32_t src_h,uint32_t out_w,uint32_t out_h,float sharpness_stops);
#ifdef __cplusplus
}
#endif
#endif
