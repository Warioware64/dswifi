# Local multiplayer demo

This is the local multiplayer demo from upstream DSWiFi, built against the copy
of the library in this repository instead of the one that comes with BlocksDS.

It exists as a **control experiment** for the DS Download Play work. Download
Play is a multiplayer host talking to a retail console, and when a transfer
stalls there are two very different explanations: the multiplayer layer of this
fork is broken for everyone, or it works and a retail client wants something
different from what it is being sent. Running this demo answers that, because
here both sides are DSWiFi.

## How to build it

Build the library first, from the root of the repository, then:

```
make -C examples/local_multiplayer DEBUG=1
```

## How to use it

Run it on two consoles. Pick host mode on one and client mode on the other, and
connect them. On the host screen watch:

```
CMD:<sent>+<retried>r done:<finished> nak:<failed>
Replies: <with data>  empty: <with nothing in them>
MP ok:<accepted> d<n>=<count>...
```

The number that matters is **`Replies`**, the ones carrying data. A client
always transmits something in its reply slot, so `empty` climbing on its own
only means the client is there and synchronised, not that anything is getting
through.

- **`Replies` climbing** means clients answer with real data. The multiplayer
  path works, and a Download Play transfer that stalls with only `empty`
  climbing is a difference specific to what retail consoles expect.
- **`Replies` at zero while `empty` climbs** means no client can answer this
  build with data at all, and Download Play is not the thing to debug. That is
  the same signature Download Play currently shows, so seeing it here would move
  the whole investigation into the multiplayer layer.
- **`nak` climbing** is the hardware reporting that the replies a CMD frame
  polled for never arrived. It is the same finding from the hardware's side.
- **`MP ok` far below `Replies received`** means replies arrive but are being
  discarded before the application sees them; the `d<n>` counters say which
  check rejected them, indexed by `Wifi_MultiplayerDropReason`.

The demo builds its own ARM7 core rather than using a prebuilt one, because this
fork changes the structures the two CPUs share.
