/* Host-side API of the userspace virtio-vsock device. */
#ifndef SYSTEM_VSOCK_HOST_H
#define SYSTEM_VSOCK_HOST_H
#include <stdint.h>
/* Hand a connected host socket to the device; it becomes a vsock stream to guest_port.
 * Must be called with the BQL held. Returns 0 or -errno. */
int vsock_host_connect_fd(int fd, uint32_t guest_port);
#endif
