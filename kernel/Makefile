# Generated automatically from Makefile.in by configure.
#
# Copyright (c) 1995,1994 The University of Utah and
# the Computer Systems Laboratory (CSL).  All rights reserved.
#
# Permission to use, copy, modify and distribute this software and its
# documentation is hereby granted, provided that both the copyright
# notice and this permission notice appear in all copies of the
# software, derivative works or modified versions, and any portions
# thereof, and that both notices appear in supporting documentation.
#
# THE UNIVERSITY OF UTAH AND CSL ALLOW FREE USE OF THIS SOFTWARE IN ITS "AS
# IS" CONDITION.  THE UNIVERSITY OF UTAH AND CSL DISCLAIM ANY LIABILITY OF
# ANY KIND FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
#
# CSL requests users of this software to return to csl-dist@cs.utah.edu any
# improvements that they make and grant CSL redistribution rights.
#
#      Author: Bryan Ford, University of Utah CSL
#
#### Start of configuration section ####

GSRCDIR	= /home/slex/Scaricati/xMach-main/kernel//home/slex/Scaricati/xMach-main/kernel/generic
MSRCDIR = /home/slex/Scaricati/xMach-main/kernel
OBJDIR	= /home/slex/Scaricati/xMach-main/kernel

prefix = /usr/local
exec_prefix = ${prefix}

include $(OBJDIR)/Makeconf

SUBDIRS =  boot/bsd bootstrap include kernel kernel/imps libmach libmach/c libmach/exec libmach/standalone libmach/unixoid libthreads mig

##### End of configuration section #####

# Some subdirectories must be built in a certain order,
# either before or after all other subdirectories.
FIRST_SUBDIRS = mig libmach libmach/standalone libmach/exec libmach/c \
		libthreads
LAST_SUBDIRS = boot/bsd boot/linux

SRCTREES = $(MSRCDIR)

BMODS = $(OBJDIR)/kernel/kernel $(OBJDIR)/bootstrap/bootstrap

#image: bsdimage linuximage
image: bsdimage

bsdimage: all
	MACHBOOTDIR=`cd $(OBJDIR)/boot/bsd; pwd` \
	$(OBJDIR)/boot/bsd/mkbsdimage -o $(OBJDIR)/Mach $(BMODS)

linuximage: all
	MACHBOOTDIR=`cd $(OBJDIR)/boot/linux; pwd` \
	$(OBJDIR)/boot/linux/mklinuximage -o $(OBJDIR)/zMach $(BMODS)

include $(GSRCDIR)/Makerules.top

