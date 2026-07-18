/*
 * mp_v1_1.h - MultiProcessor Specification 1.x configuration flag.
 *
 * The name dates back to Intel MPS 1.1 (1994); MPS 1.4 (1997) is wire
 * compatible.  Kept as an ODE-style stub for now so the historical
 * ``#if MP_V1_1`` ifdefs keep working.  #300 (Fase A SMP) wires
 * -DMP_V1_1=1 into KERNEL_DEFINES when UROS_NCPUS>1; the guard below
 * makes that value win over the default-zero defined here.
 *
 * The file will be retired by #306 (ODE stub cleanup) and the symbol
 * itself renamed during the post-SMP rework.
 */
#ifndef _MP_V1_1_H_
#define _MP_V1_1_H_

#ifndef MP_V1_1
#define MP_V1_1 0
#endif

#endif /* _MP_V1_1_H_ */
