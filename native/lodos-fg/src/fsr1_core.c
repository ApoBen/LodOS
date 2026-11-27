#include "fsr1_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define A_CPU 1
#include "../third_party/amd-fsr1/ffx_a.h"
#include "../third_party/amd-fsr1/ffx_fsr1.h"
static VkShaderModule shader(VkDevice d,const char *path,VkResult *result){
 FILE *f=fopen(path,"rb"); if(!f){*result=VK_ERROR_INVALID_SHADER_NV;return VK_NULL_HANDLE;}
 fseek(f,0,SEEK_END); long n=ftell(f); rewind(f); uint32_t *p=malloc((size_t)n);
 if(!p || n<20 || fread(p,1,(size_t)n,f)!=(size_t)n){free(p);fclose(f);*result=VK_ERROR_INITIALIZATION_FAILED;return VK_NULL_HANDLE;} fclose(f);
 VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)n,.pCode=p}; VkShaderModule m; *result=vkCreateShaderModule(d,&ci,NULL,&m); free(p);return *result==VK_SUCCESS?m:VK_NULL_HANDLE;
}
static VkResult make_pipeline(LodosFsr1 *c,const char *path,VkPipeline *out){
 VkResult r; VkShaderModule m=shader(c->device,path,&r); if(r!=VK_SUCCESS)return r;
 VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
  .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=m,.pName="main"},
  .layout=c->pipeline_layout}; r=vkCreateComputePipelines(c->device,VK_NULL_HANDLE,1,&ci,NULL,out); vkDestroyShaderModule(c->device,m,NULL); return r;
}
VkResult lodos_fsr1_create(LodosFsr1 *c,VkDevice d,const char *easu,const char *rcas){
 memset(c,0,sizeof(*c)); c->device=d;
 VkDescriptorSetLayoutBinding b[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},{.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
 VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=b}; VkResult r=vkCreateDescriptorSetLayout(d,&si,NULL,&c->set_layout); if(r)return r;
 VkPushConstantRange pc={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.offset=0,.size=sizeof(uint32_t)*16}; VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&c->set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&pc};
 r=vkCreatePipelineLayout(d,&pi,NULL,&c->pipeline_layout); if(r)goto fail;
 VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}}; VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=2,.pPoolSizes=ps}; r=vkCreateDescriptorPool(d,&dpi,NULL,&c->pool); if(r)goto fail;
 VkDescriptorSetLayout layouts[2]={c->set_layout,c->set_layout}; VkDescriptorSet sets[2]; VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=c->pool,.descriptorSetCount=2,.pSetLayouts=layouts}; r=vkAllocateDescriptorSets(d,&ai,sets); if(r)goto fail; c->easu_set=sets[0];c->rcas_set=sets[1];
 VkSamplerCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0}; r=vkCreateSampler(d,&sm,NULL,&c->sampler); if(r)goto fail;
 r=make_pipeline(c,easu,&c->easu); if(r)goto fail; r=make_pipeline(c,rcas,&c->rcas); if(r)goto fail; return VK_SUCCESS;
fail:lodos_fsr1_destroy(c);return r;
}
void lodos_fsr1_destroy(LodosFsr1 *c){if(!c||!c->device)return; VkDevice d=c->device; if(c->easu)vkDestroyPipeline(d,c->easu,NULL);if(c->rcas)vkDestroyPipeline(d,c->rcas,NULL);if(c->sampler)vkDestroySampler(d,c->sampler,NULL);if(c->pool)vkDestroyDescriptorPool(d,c->pool,NULL);if(c->pipeline_layout)vkDestroyPipelineLayout(d,c->pipeline_layout,NULL);if(c->set_layout)vkDestroyDescriptorSetLayout(d,c->set_layout,NULL);memset(c,0,sizeof(*c));}
VkResult lodos_fsr1_record(LodosFsr1 *c,VkCommandBuffer cmd,VkImageView src,VkImageView mid,VkImageView dst,uint32_t sw,uint32_t sh,uint32_t ow,uint32_t oh,float sharp){
 if(!ow||!oh||!sw||!sh||ow>sw*4||oh>sh*4)return VK_ERROR_INITIALIZATION_FAILED;
 VkDescriptorImageInfo a={c->sampler,src,VK_IMAGE_LAYOUT_GENERAL},b={VK_NULL_HANDLE,mid,VK_IMAGE_LAYOUT_GENERAL},d={c->sampler,mid,VK_IMAGE_LAYOUT_GENERAL},e={VK_NULL_HANDLE,dst,VK_IMAGE_LAYOUT_GENERAL};
 VkWriteDescriptorSet w[4]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=c->easu_set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&a},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=c->easu_set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&b},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=c->rcas_set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&d},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=c->rcas_set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&e}}; vkUpdateDescriptorSets(c->device,4,w,0,NULL);
 uint32_t con[16]={0}; FsrEasuCon(&con[0],&con[4],&con[8],&con[12],(float)sw,(float)sh,(float)sw,(float)sh,(float)ow,(float)oh);
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c->easu); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c->pipeline_layout,0,1,&c->easu_set,0,NULL); vkCmdPushConstants(cmd,c->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(con),con);vkCmdDispatch(cmd,(ow+7)/8,(oh+7)/8,1);
 VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,NULL,0,NULL);
 uint32_t rc[4]; FsrRcasCon(rc,sharp);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c->rcas);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,c->pipeline_layout,0,1,&c->rcas_set,0,NULL);vkCmdPushConstants(cmd,c->pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(rc),rc);vkCmdDispatch(cmd,(ow+7)/8,(oh+7)/8,1);return VK_SUCCESS;
}
