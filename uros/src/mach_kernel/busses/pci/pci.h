/*
 * busses/pci/pci.h — PCI bus type definitions and vendor/device IDs.
 *
 * Reconstructed from original ODE MkLinux DR3 sources.
 */

#ifndef _BUSSES_PCI_PCI_H_
#define _BUSSES_PCI_PCI_H_

/*
 * PCI type definitions
 */
typedef unsigned short	pci_vendor_id_t;
typedef unsigned short	pci_dev_id_t;
typedef unsigned char	pci_class_t;
typedef unsigned char	pci_subclass_t;

/*
 * Invalid vendor ID (all 1s means no device present)
 */
#define PCI_VID_INVALID		0xFFFF

/*
 * PCI bus/device scan ranges
 */
#define PCI_FIRST_BUS		0
#define PCI_LAST_BUS		255
#define PCI_FIRST_DEVICE	0
#define PCI_LAST_DEVICE		31

/*
 * Vendor IDs
 */
#define OLD_NCR_ID	0x1000
#define ATI_ID		0x1002
#define DEC_ID		0x1011
#define CIRRUS_ID	0x1013
#define IBM_ID		0x1014
#define NCR_ID		0x101A
#define AMD_ID		0x1022
#define MATROX_ID	0x102B
#define COMPAQ_ID	0x0E11
#define NEC_ID		0x1033
#define HP_ID		0x103C
#define KPC_ID		0x1040
#define OPTI_ID		0x1045
#define TI_ID		0x104C
#define SONY_ID		0x104D
#define MOT_ID		0x1057
#define MYLEX_ID	0x1069
#define APPLE_ID	0x106B
#define QLOGIC_ID	0x1077
#define BIT3_ID		0x108A
#define CABLETRON_ID	0x10B1
#define THREE_COM_ID	0x10B7
#define CERN_ID		0x10DC
#define ECP_ID		0x10F0
#define ECU_ID		0x10F4
#define PROTEON_ID	0x1108
#define S3_ID		0x5333
#define INTEL_ID	0x8086
#define ADP_ID		0x9004

/*
 * Device IDs
 */
#define DEC_PPB		0x0001
#define DEC_4250	0x0009
#define DEC_TGA		0x0004
#define DEC_NVRAM	0x0007
#define DEC_PZA		0x0008
#define DEC_21140	0x0009
#define DEC_7407	0x000D
#define DEC_FTA		0x000F
#define DEC_21041	0x0014

#define INTEL_PCEB	0x0482
#define INTEL_DRAM	0x0483
#define INTEL_SIO	0x0484
#define INTEL_PSC	0x0486
#define INTEL_CACHE	0x04A3

#define NCR_810		0x0001
#define NCR_825		0x0003

#define QLOGIC_ISP1020	0x1020
#define ADP_7870	0x7078

#define ATI_MACH32	0x4158
#define ATI_MACH64_CX	0x4358
#define ATI_MACH64_GX	0x4758

#define MYLEX_960P	0x0001

#define S3_VISION864	0x88C0
#define S3_VISION964	0x88D0

#define TCM_3C590	0x5900
#define TCM_3C595_0	0x5950
#define TCM_3C595_1	0x5951
#define TCM_3C595_2	0x5952

/*
 * PCI class codes (register 0x08, byte 3)
 */
#define BASE_BC		0x00
#define BASE_MASS	0x01
#define BASE_NETWORK	0x02
#define BASE_DISPLAY	0x03
#define BASE_MULTMEDIA	0x04
#define BASE_MEM	0x05
#define BASE_BRIDGE	0x06

/*
 * PCI subclass codes (register 0x08, byte 2)
 */
#define SUB_PREDEF	0x00
#define SUB_PRE_VGA	0x01

#define SUB_SCSI	0x00
#define SUB_IDE		0x01
#define SUB_FDI		0x02
#define SUB_IPI		0x03
#define SUB_MASS_OTHER	0x80

#define SUB_ETHERNET	0x00
#define SUB_TOKEN_RING	0x01
#define SUB_FDDI	0x02
#define SUB_NETWORK_OTHER 0x80

#define SUB_VGA		0x00
#define SUB_XGA		0x01
#define SUB_DISPLAY_OTHER 0x80

#define SUB_VIDEO	0x00
#define SUB_AUDIO	0x01
#define SUB_MULTMEDIA_OTHER 0x80

#define SUB_RAM		0x00
#define SUB_FLASH	0x01
#define SUB_MEM_OTHER	0x80

#define SUB_HOST	0x00
#define SUB_ISA		0x01
#define SUB_EISA	0x02
#define SUB_MC		0x03
#define SUB_PCI		0x04
#define SUB_PCMCIA	0x05
#define SUB_BRIDGE_OTHER 0x80

/*
 * PCI print helper (implemented in busses/pci/pci.c)
 */
extern void pci_print_id(pci_vendor_id_t, pci_dev_id_t,
			  pci_class_t, pci_subclass_t);

#endif /* _BUSSES_PCI_PCI_H_ */
