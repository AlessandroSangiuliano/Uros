#ifndef _MACH_I386_VM_TYPES_H_
#define _MACH_I386_VM_TYPES_H_

#include <stdint.h>

typedef unsigned int natural_t;
typedef int integer_t;
typedef int int32;
typedef unsigned int uint32;
typedef natural_t vm_offset_t;
typedef natural_t vm_size_t;

#define MACH_MSG_TYPE_INTEGER_T MACH_MSG_TYPE_INTEGER_32

#endif /* _MACH_I386_VM_TYPES_H_ */
