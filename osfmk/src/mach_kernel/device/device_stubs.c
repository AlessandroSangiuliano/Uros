/*
 * Stub implementations for device functions not yet compiled
 * These are temporary stubs to get the kernel linking.
 */

#include <mach/kern_return.h>
#include <device/device_types.h>
#include <device/net_status.h>
#include <ipc/ipc_port.h>

/* SCSI info stubs (scsi_info device not compiled) */
io_return_t scsi_info_open(dev_t dev, dev_mode_t flag, io_req_t ior) { return D_NO_SUCH_DEVICE; }
void scsi_info_close(dev_t dev) {}
io_return_t scsi_info_read(dev_t dev, io_req_t ior) { return D_NO_SUCH_DEVICE; }
io_return_t scsi_info_write(dev_t dev, io_req_t ior) { return D_NO_SUCH_DEVICE; }
io_return_t scsi_info_getstatus(dev_t dev, dev_flavor_t flavor, dev_status_t data, mach_msg_type_number_t *count) { return D_NO_SUCH_DEVICE; }
io_return_t scsi_info_setstatus(dev_t dev, dev_flavor_t flavor, dev_status_t data, mach_msg_type_number_t count) { return D_NO_SUCH_DEVICE; }

/* Thread/task sample stubs */
kern_return_t thread_sample(void *thread, void *reply_port) { return KERN_FAILURE; }
kern_return_t task_sample(void *task, void *reply_port) { return KERN_FAILURE; }

/* ds_device reply stubs are now MIG-generated from device_reply.defs */

/* Profile stub (when MACH_PROF is disabled) */
void profile(void) {}
