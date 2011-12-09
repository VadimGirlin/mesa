TOP = ../../../..
include $(TOP)/configs/current

LIBNAME = r600

LIBRARY_INCLUDES = \
	-I$(TOP)/include \
	-I$(TOP)/src/gallium/drivers/radeon/


# get C_SOURCES
include Makefile.sources

ifeq ($(HAVE_LLVM),0x0209)

LIBRADEON = $(TOP)/src/gallium/drivers/radeon/libradeon.a

EXTRA_OBJECTS = \
	$(LIBRADEON)

endif

include ../../Makefile.template

$(LIBRADEON): make_radeon

.PHONY: make_radeon

make_radeon:
	$(MAKE) -C $(TOP)/src/gallium/drivers/radeon/ default

