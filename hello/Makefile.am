#bin_PROGRAMS = hello_server
#hello_server_SOURCES = hello.c
AUTOMAKE_OPTIONS = foreign

SRCDIR	= ${HOME}/Uros/hello
OBJDIR = ${HOME}/Uros/hello
#GLBINCDIR = /home/slex/xmach-installation/include

GSRCDIR	= ${HOME}/Uros/hello/../kernel/generic
MSRCDIR = ../kernel


#prefix = ${HOME}/xmach-installation
exec_prefix = ${prefix}

clean-local:
	@rm obj/*


include $(SRCDIR)/conf/Makeconf

include $(MSRCDIR)/Makerules
include $(GSRCDIR)/Makerules
include $(SRCDIR)/Makerules