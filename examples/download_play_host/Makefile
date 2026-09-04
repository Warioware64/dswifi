# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2025

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

# User config

NAME		:= dswifi_download_play_host

GAME_TITLE	:= Download Play host
GAME_SUBTITLE	:= DSWiFi

SOURCEDIRS	:= arm9/source

# Build against the copy of the library in this repository. Run "make" in the
# root of the repository first to build it. Set DSWIFI_DL to
# "$(BLOCKSDSEXT)/dswifi_dl" to use an installed copy instead.
DSWIFI_DL	?= $(abspath $(CURDIR)/../..)

ifeq ($(DEBUG),1)
    DEFINES	:= -DDSWIFI_LOGS
    LIBS	:= -ldswifi_dl9d_noip -lnds9d
else
    LIBS	:= -ldswifi_dl9_noip -lnds9
endif

LIBDIRS		:= $(DSWIFI_DL)

# The ARM7 core that comes with this library, rather than one built here.
#
# It has to be the one from the same build as the ARM9: both halves share the
# structures DSWiFi uses to talk between the CPUs, and a core built against a
# different version of the library reads the wrong fields. Wifi_Init() checks
# for that and refuses, so a mismatch says so rather than misbehaving.
ifeq ($(DEBUG),1)
    ARM7ELF	:= $(DSWIFI_DL)/sys/arm7/arm7_dswifi_dl_debug.elf
else
    ARM7ELF	:= $(DSWIFI_DL)/sys/arm7/arm7_dswifi_dl.elf
endif

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

# Repackage the ROM when that core changes, which the template doesn't do: it
# only makes the ROM depend on the ARM9. Rebuilding the library and then this
# leaves the previous core in the ROM, which is the mismatch above.
$(ROM): $(ARM7ELF)
