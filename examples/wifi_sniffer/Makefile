# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2025

BLOCKSDS	?= /opt/blocksds/core

# User config

NAME		:= dswifi_wifi_sniffer

GAME_TITLE	:= Wireless sniffer
GAME_SUBTITLE	:= DSWiFi

# This example builds its own ARM7 core instead of using one of the prebuilt
# ones that come with BlocksDS. The ARM9 and the ARM7 share the structures that
# DSWiFi uses to talk to each other, so both of them have to be built against
# the same version of the library.

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile
