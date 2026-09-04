# DSWiFi examples

This folder contains examples of how to use the features that this fork of
DSWiFi adds on top of the upstream library.

- `download_play_host`: Sends a NDS ROM to another console with DS Download Play.
- `dlplay_child`: Meant to be sent by `download_play_host` rather than started
  from a card. Once it is running it connects back to the host that sent it and
  the two exchange messages, using what the loader leaves at 0x027FFC40 to find
  it — no scanning — and reading the 32 bytes the host chose to send along with
  the program.
- `dlplay_probe`: Connects to a console hosting DS Download Play, records the
  frames it sends, and answers them the way a console does. Pointed at a retail
  host it shows what a working host's side of the transfer looks like; pointed
  at `download_play_host` it drives the whole transfer, so the host can be
  tested end to end with two flashcarts instead of a retail console. SELECT
  switches it to listening only: it joins without answering and writes down what
  the *other* console in the session is telling the host, which is the only way
  to see that — a console that is not taking part never receives multiplayer
  frames at all, whatever its receive filter says.
- `wifi_sniffer`: Records the frames of nearby consoles to a file, so that what
  a real DS Download Play host puts on the air can be compared with what this
  library sends. It starts by hopping through the channels and keeping only the
  beacons that announce a game; A locks it to one channel and records everything
  said there, including the authentication and association exchange and the
  replies a client sends, which is what watching two other consoles talk to each
  other needs.
- `local_multiplayer`: The upstream local multiplayer demo, built against this
  fork. It is the control experiment for the Download Play work: both sides are
  DSWiFi, so it says whether the multiplayer layer works at all before blaming
  anything on what a retail console expects.
