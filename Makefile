SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-vastfmt.$(SHLIB_EXT)
debug: all

# hidapi comes from the system, and specifically from its HIDRAW backend on
# Linux. This plugin used to compile hidapi's LIBUSB backend into itself
# (src/hid.c, plus src/hid-mac.c on macOS) and link -lusb-1.0. That backend runs
# a read thread per open device, so its entry point lived inside this .so -
# which meant the library could never be safely dlclose()d and the plugin could
# not declare FPP_PLUGIN_SUPPORTS_UNLOAD. The hidraw backend is a thin wrapper
# over read/write/ioctl on /dev/hidraw*, starts no threads, and lives in
# libhidapi-hidraw.so, which is never unloaded - so nothing outside this plugin
# ends up pointing into it. fpp-kfmt drives its USB HID device the same way.
#
# The device is still opened by the path hid_enumerate() reports, so nothing in
# VASTFMT.cpp needed to change: the path format is the backend's own.
#
# libhidapi-dev ships on the FPP image (fpp-kfmt already builds against it with
# no declared dependency), so there is nothing extra to install.
ifeq '$(ARCH)' 'OSX'
# Self-provision as the old libusb rule did, so a macOS build still works from a
# clean checkout. Homebrew ships hidapi as a single libhidapi (IOHIDManager
# backend); there is no separate -hidraw variant on macOS.
HIDAPIHEADER=$(HOMEBREW)/include/hidapi/hidapi.h
$(HIDAPIHEADER):
	brew install hidapi
LIBS_fpp_vastfmt_so += -lhidapi
HIDAPI_DEP=$(HIDAPIHEADER)
else
# libhidapi-dev ships on the FPP image - fpp-kfmt already builds against it with
# no declared dependency - so there is nothing to install here.
LIBS_fpp_vastfmt_so += -lhidapi-hidraw
HIDAPI_DEP=
endif

CFLAGS+=-I.
OBJECTS_fpp_vastfmt_so += src/FPPVastFM.o  src/Si4713.o src/bitstream.o src/VASTFMT.o src/I2CSi4713.o
LIBS_fpp_vastfmt_so += -L$(SRCDIR) -lfpp -ljsoncpp
CXXFLAGS_src/FPPVastFM.o += -I$(SRCDIR)



%.o: %.cpp Makefile $(HIDAPI_DEP)
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

%.o: %.c Makefile $(HIDAPI_DEP)
	$(CCACHE) $(CCOMPILER) $(CFLAGS) $(CFLAGS_$@) -c $< -o $@

libfpp-vastfmt.$(SHLIB_EXT): $(OBJECTS_fpp_vastfmt_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_vastfmt_so) $(LIBS_fpp_vastfmt_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-vastfmt.$(SHLIB_EXT) $(OBJECTS_fpp_vastfmt_so)
