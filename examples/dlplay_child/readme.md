# Download Play child

A program meant to be **sent** to a console rather than started from a card. Once
it is running it connects back to the host that sent it, and the two exchange
messages.

Send it with `examples/download_play_host`. Started from a card it says so and
stops, because there is no host to connect back to.

## What it shows

Two things a program delivered by Download Play gets for free, without asking the
host for either:

- **Where it came from.** The loader leaves the host's MAC address, channel and
  game ID at `0x027FFC40`, along with an indicator saying the program arrived
  this way. GBATEK documents it and says outright that "multiplayer games can use
  that info for communicating with each other after the upload". There is no need
  to scan for the host: the address and channel are enough to associate.
- **Whatever the host chose to send with it.** The 32 bytes at `0x027FFBE0` are
  Nintendo's user parameter, set here through `Wifi_DlPlayInfo::user_param`. They
  travel in the part of the boot information that nothing signs, so a host can
  use them freely.

Talking back is `Wifi_DlPlayAppReply()` on this side and
`Wifi_DlPlaySetAppHandler()` on the host's. A host tells a returning console
apart from one asking to be sent a program by what it says, the same way it tells
a Download Station client apart.

## Note

This could not work at all until recently. The library reached the memory it
shares with the ARM7 through an uncached mirror, and on a console started by
Download Play the address it was given could be one that isn't memory, so
initialization never finished and a delivered program had no wifi. This example
is the test for that.
