/* Minimal compatibility header: machid dpager types used by machid_vmstuff.c */
#ifndef _MACHID_DPAGER_H_
#define _MACHID_DPAGER_H_

#include <mach.h>
#include <mach/default_pager_types.h>
#include <servers/machid_types.h>

/* Page info used locally by machid_vmstuff */
typedef struct vm_page_info {
    vm_offset_t vpi_offset;
    int vpi_state;
} vm_page_info_t;

/* VPI state bits (minimal subset) */
#define VPI_STATE_INACTIVE 0x1
#define VPI_STATE_REFERENCE 0x2
#define VPI_STATE_PAGER     0x4

/* Object information (minimal subset) */
struct voi_info {
    mobject_name_t voi_object;
    int voi_state;
    vm_offset_t voi_paging_offset;
    vm_offset_t voi_shadow_offset;
    vm_size_t voi_pagesize;
    mobject_name_t voi_shadow;
};

/* VOI state bits (minimal subset) */
#define VOI_STATE_INTERNAL      0x1
#define VOI_STATE_PAGER_CREATED 0x2
#define VOI_STATE_PAGER_READY   0x4
#define VOI_STATE_PAGER_INITIALIZED 0x8

typedef struct object {
    struct object *o_link;
    struct voi_info o_info;
    struct object *o_shadow;
    mdefault_pager_t o_dpager;
    default_pager_object_t o_dpager_info;
    vm_page_info_t *o_pages;
    unsigned int o_num_pages;
    vm_page_info_t *o_hint;
} object_t;

#endif /* _MACHID_DPAGER_H_ */
