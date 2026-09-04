# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023

# Tools
# -----

CP		:= cp
INSTALL		:= install
MAKE		:= make
RM		:= rm -rf

# Verbose flag
# ------------

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

# Targets
# -------

.PHONY: all arm7 arm7_cores arm9 clean docs install

all: arm9 arm7 arm7_cores

arm9:
	@+$(MAKE) -f Makefile.arm9 --no-print-directory
	@+$(MAKE) -f Makefile.arm9 --no-print-directory DEBUG=1
	@+$(MAKE) -f Makefile.arm9 --no-print-directory ENABLE_LWIP=1
	@+$(MAKE) -f Makefile.arm9 --no-print-directory ENABLE_LWIP=1 DEBUG=1

arm7:
	@+$(MAKE) -f Makefile.arm7 --no-print-directory
	@+$(MAKE) -f Makefile.arm7 --no-print-directory DEBUG=1

# The ready-made ARM7 cores, so that a program using this library doesn't have to
# build one. They link the archives above, so they come after them.
arm7_cores: arm7
	@+$(MAKE) -C sys/arm7 --no-print-directory

clean:
	@echo "  CLEAN"
	@$(RM) lib build
	@+$(MAKE) -C sys/arm7 --no-print-directory clean

docs:
	@echo "  DOXYGEN"
	doxygen Doxyfile

# This fork is installed next to the other third party libraries, as "dswifi_dl",
# instead of replacing the copy of DSWiFi that comes with BlocksDS. The two can
# be installed at the same time because the archives of this one have different
# names, so "-ldswifi_dl9" and "-ldswifi9" select one or the other.
#
# The headers keep the names they have upstream, so a program that used DSWiFi
# only needs to change the library that it links against to use this one. Don't
# add both libraries to LIBDIRS at the same time, or which set of headers gets
# used would depend on the order of the list.
BLOCKSDSEXT	?= /opt/blocksds/external

INSTALLDIR	?= $(BLOCKSDSEXT)/dswifi_dl
INSTALLDIR_ABS	:= $(abspath $(INSTALLDIR))

install: all
	@echo "  INSTALL $(INSTALLDIR_ABS)"
	@test $(INSTALLDIR_ABS)
	$(V)$(RM) $(INSTALLDIR_ABS)
	$(V)$(INSTALL) -d $(INSTALLDIR_ABS)
	$(V)$(CP) -r include lib COPYING* $(INSTALLDIR_ABS)
	$(V)$(INSTALL) -d $(INSTALLDIR_ABS)/sys/arm7
	$(V)$(CP) sys/arm7/arm7_*.elf $(INSTALLDIR_ABS)/sys/arm7
