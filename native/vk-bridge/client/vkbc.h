/* libvkbridge_client: plain C API over the vk-bridge transport (not an ICD yet).
 * vkbc_<vkCmd>(c, ...) for each bridged cmd: see generated vkb_rpc.h. */
#ifndef VKBC_H
#define VKBC_H
#include "vkb_rpc.h"

/* connect to abstract AF_UNIX socket @name (NULL -> "lodos-vk"); NULL + errno on failure */
vkbc *vkbc_connect(const char *name);
void vkbc_disconnect(vkbc *c);
#endif
