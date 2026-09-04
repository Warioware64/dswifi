# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023

# The ARM7 cores that come with this fork.
#
# A program using this library needs an ARM7 core built against it. The ones in
# $(BLOCKSDS)/sys/arm7/main_core link the copy of DSWiFi that comes with
# BlocksDS, and this fork changes both the MAC RAM layout and Wifi_MainStruct,
# which the two CPUs have to agree on -- so pairing one of those with an ARM9
# built against this library links cleanly and then misbehaves.
#
# These are the same cores, built against this library instead, so a program can
# use one rather than building its own:
#
#     ARM7ELF := $(BLOCKSDSEXT)/dswifi_dl/sys/arm7/arm7_dswifi_dl.elf
#
# The combinations mirror the ones BlocksDS ships for the stock library. There is
# no "minimal" variant, because a core without the wireless library is BlocksDS's
# own and this fork has nothing to add to it.

MAKE		:= make
RM		:= rm -rf

.PHONY: all clean plain maxmod libxm7

all: plain maxmod libxm7

clean:
	@echo "  CLEAN"
	@$(RM) build
	@$(RM) arm7_*.elf

plain:
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl_debug \
		  DEBUG_LIBS=1

maxmod:
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl_maxmod \
		  USE_MAXMOD=1
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl_maxmod_debug \
		  USE_MAXMOD=1 \
		  DEBUG_LIBS=1

libxm7:
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl_libxm7 \
		  USE_LIBXM7=1
	@+$(MAKE) -f Makefile.generic --no-print-directory \
		  NAME=dswifi_dl_libxm7_debug \
		  USE_LIBXM7=1 \
		  DEBUG_LIBS=1
