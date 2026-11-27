/* libvkbridge_client: plain C API over the vk-bridge transport (not an ICD yet).
 * vkbc_<vkCmd>(c, ...) for each bridged cmd: see generated vkb_rpc.h. */
#ifndef VKBC_H
#define VKBC_H
#include "vkb_rpc.h"

/* connect to abstract AF_UNIX socket @name (NULL -> "lodos-vk"); NULL + errno on failure */
vkbc *vkbc_connect(const char *name);
void vkbc_disconnect(vkbc *c);

#ifdef VKB_CMD_HAS_vkMapMemory
VkResult vkbc_vkMapMemory(vkbc *c, VkDevice device, VkDeviceMemory memory,
                          VkDeviceSize offset, VkDeviceSize size,
                          VkMemoryMapFlags flags, void **ppData);
#endif
int vkbc_send_fd(vkbc *c, char tag, int fd);
int vkbc_recv_fd(vkbc *c, char expected_tag);
#endif
