# DSWifi DL

A fork of [DSWifi](https://blocksds.skylyrac.net/dswifi/index.html) that adds
support for acting as a **DS Download Play host**: announcing a program in the
Download Play menu of other consoles and sending it to them.

Everything that upstream DSWifi does is still here and unchanged. See
`documentation/` for the original guides, and `examples/` for the new ones.

## Installing

This fork installs itself as `dswifi_dl`, next to the other third party
libraries, instead of replacing the copy of DSWifi that comes with BlocksDS:

```
make
make install
```

Both libraries can be installed at the same time. The archives of this one are
called `libdswifi_dl7.a`, `libdswifi_dl9.a` and so on, so `-ldswifi_dl9` and
`-ldswifi9` select one or the other.

The headers keep the names they have upstream (`dswifi9.h`, `dswifi7.h`, ...),
so a program that used DSWifi only needs to change the library that it links
against. For the same reason, don't add both libraries to `LIBDIRS` at the same
time, or which set of headers gets used would depend on the order of the list.

## Using it

```
LIBS    := -ldswifi_dl9 -lnds9
LIBDIRS := $(BLOCKSDSEXT)/dswifi_dl $(BLOCKSDS)/libs/libnds
```

The DS Download Play API is in `dswifi_dlplay.h`.

### The ARM7 core

The two CPUs share the structures the library uses to talk to each other, and
this fork changes them, so the ARM7 cores that come with BlocksDS can't be used:
they link the copy of DSWiFi that comes with it.

Ready-made cores built against *this* library come with it instead. Pick one:

```
ARM7ELF := $(BLOCKSDSEXT)/dswifi_dl/sys/arm7/arm7_dswifi_dl.elf
```

with the same combinations BlocksDS offers, and a debug build of each:

| | |
| --- | --- |
| `arm7_dswifi_dl.elf` | wireless only |
| `arm7_dswifi_dl_maxmod.elf` | and maxmod |
| `arm7_dswifi_dl_libxm7.elf` | and LibXM7 |

Add `_debug` to the name for the build that carries the ARM7 log, which is what
`consoleArm7Setup()` displays. `examples/local_multiplayer` uses one; a program
that needs something the core doesn't do can still build its own, which is what
`examples/dlplay_probe` does.

**The core and the ARM9 must come from the same build of the library.** Mixing
them links without complaint, and the two would then read different fields of the
same structure. The library checks for it: `Wifi_InitDefault()` fails and
`Wifi_GetInitFailStage()` returns `253`.

The easiest way to end up with a mismatched pair is to let `make` produce one.
`rom_arm9/Makefile` builds the ROM out of the ARM9 alone:

```make
$(ROM): $(ELF)
```

so rebuilding the library, and then the program, keeps whichever core was packed
last time — `ndstool` is never re-run. Say so after the `include`:

```make
$(ROM): $(ARM7ELF)
```

An older core paired this way predates the check itself and so can't report it:
it writes its progress where the ARM9 no longer looks, and
`Wifi_GetInitFailStage()` reads back `0`, as though the ARM7 had never started.
