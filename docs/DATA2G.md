# Data2G as Ragtime meets it

What `data2g-host` does at the edges Ragtime touches, read from
Data2G's own docs and code at
[arodland/Data2G@13807c6](https://github.com/arodland/Data2G/tree/13807c6c4e51f57a29f034fd5bd38c8cccbfcb51)
(2026-10-25), so we stop guessing. It is a summary in our words, never
copied code (CLAUDE.md, "Data2G"). When Data2G moves on, read it again
and update this page, with the new commit. What Data2G PR #122 adds
(DISCONNECTED reasons, `TIMEOUT`, `TXPOWER`) was read at its head,
[843987e](https://github.com/arodland/Data2G/pull/122/commits/843987e),
on v0.3.0 (2026-10-26). The Hand ("Hands" below) was read where Data2G
PR #146 merged,
[e09e19d](https://github.com/arodland/Data2G/commit/e09e19d) (2026-11-11,
after v0.4.3; no release has it yet, 2026-11-12).

## Where to look in Data2G

- `README.md`, then `docs/ARCHITECTURE.md`: the map, Python beside C++.
- `docs/broadcast.md`: broadcast groups, the burst format and every
  `BCAST` command and status. The spec for our D2G traffic.
- `docs/arq.md`: connected sessions, the gear shifter, `CLEANTXBUFFER`.
- `data2g/host.py`'s module docstring: the command reference.
  `native/core/host/host.cpp` mirrors it.
- `data2g/kisslink.py`: how frames become bursts and come back out, and
  channel access (`data2g/arq/engine.py`, `_kiss_burst`).

Python (`data2g/`) is the normative reference. The programs operators
download are the C++/Qt port (`native/`): `data2g-host` (headless) and
`data2g-gui`, for Windows, macOS and Linux. Neither needs PyTorch. The
Python host needs only numpy and scipy; torch is for training.

## What the operator runs

- **Ports.** Commands on 8300 (CR-terminated lines, VARA style), the
  session's bytes on 8301, KISS on 8100. All are always on.
- **Rig.** The native host links Hamlib in. Its default rig is model 2,
  a client of rigctld at localhost:4532, so Ragtime's dial reads (`f`)
  share that rigctld. An operator who gives the host a serial rig
  directly has no rigctld for Ragtime to read. The host can poll the
  dial (`--rig-poll-interval`) but doesn't tell clients.
- **Transmit level** is `--output-volume` (dB) at start. Since Data2G PR
  #122, `TXPOWER dB` changes it from the next audio block (with
  `DATA2G ON`), and it stays as set when the client goes.
- **Broadcast width** is fixed when the host starts: `--kiss-bw 2400` (the
  default) or `500`. `MODES` lists only the modes inside it, and
  `BCAST MODE` refuses the rest.
- **No sound card needed for a bench.** `--audio-io pipe:IN,OUT` reads
  and writes raw 8 kHz audio on files or fifos, so two hosts
  cross-connect through two `mkfifo` pipes.

## Commands Ragtime uses

| Command | Reply | What it does |
| --- | --- | --- |
| `MYCALL call` | `OK`, then `REGISTERED call` | |
| `LISTEN ON` / `OFF` | `OK` | Take calls or not |
| `BW500`, `BW1200`, `BW2300`, `BW2750` | `OK` | The **session** width cap only: 500, 1200 or 2400 Hz (`BW2750` is 2400). Broadcast width is `--kiss-bw`. Under PR #146 `BW` also sets the Hand channels ("Hands"). There is no 200 Hz cap; the 200 Hz modes sit inside 500 Hz |
| `CHAT ON` | `OK` | Sessions favour latency over throughput (`docs/arq.md`, "CHAT ON") |
| `DATA2G ON` | `OK` | `BUFFER` counts plainly, `CLEANTXBUFFER` reports host byte offsets, bytes written before `CONNECTED` are dropped. Off again when the client goes |
| `CONNECT from to`, `DISCONNECT`, `ABORT` | `OK` / `WRONG` | `ABORT` drops a session without the handshake, and also stops the transmitter at once and clears its queue (`Transmitter::abort`, broadcast bursts too); it answers `DISCONNECTED` (`DISCONNECTED ABORT` with `DATA2G ON`, PR #122) even with no session (read at v0.3.0, 6f0e72f). Ragtime's SWR trip uses it |
| `TIMEOUT [s]` | `TIMEOUT 300.0` / `OK` / `WRONG` | PR #122, with `DATA2G ON` (else `WRONG`). Bare: the link idle timeout in seconds. With any `s` > 0: sets it, for the session under way and the next. Back to the default when the client goes |
| `TXPOWER [dB]` | `TXPOWER -1.2` / `OK` / `WRONG` | PR #122, with `DATA2G ON` (else `WRONG`). Bare: the output volume. With `dB`, -60 to 20: sets it (0 puts a burst's peak at full scale). Not reset when the client goes |
| `CLEANTXBUFFER [n]` | `CLEANTXBUFFER OK a b c` / `LATE` / `BUFFEREMPTY` / `FAILED` | Then `CLEANTXBUFFER SETTLED a b c [UNKNOWN]` (PROTOCOL.md, "Cuts") |
| `SN` | `SN snr` | The session partner's last burst, pilot S/N in 2500 Hz. `WRONG` before one |
| `VERSION` | `VERSION Data2G x.y.z` | |
| `BCAST OPEN D2G FROM call` | `BCAST PORT n` | A KISS port for the group, each burst naming the sender |
| `BCAST MODE n mode` | `OK` / `WRONG` | The port's transmit mode (below) |
| `MODES` | `MODE ...` lines, then `OK` | `MODE name width-Hz bytes-per-codeword max-codewords seconds-at-1 seconds-at-max`, narrowest first. Since PR #186, `CHANNELS awgn mpg mpp mpd` and one `THRESHOLD name dB...` per mode (same order, `-` for never reached) follow before `OK` |

Reports: `CONNECTED src dst bw`, `DISCONNECTED [reason [detail]]`
(below), `PTT ON|OFF`,
`BUSY ON|OFF`, `BUFFER n` (again every 30 s while not 0), `MODE submode`
(at a `PTT ON`, when the burst's mode differs from the last reported:
any burst the host keys, our session ACK bursts in `ack-*` modes
included, so a client takes the session's gear from the others;
`data2g/host.py` at 859724a),
`IAMALIVE` (every 60 s).

Broadcast statuses reach a client only once it has sent a `BCAST`
command:

- `BCAST n HEARD [call] snr`: a burst for our port, at its end. The call
  is there when the control decoded (we open with `FROM`). The S/N is
  always the last word. The burst's mode isn't given.
- `BCAST n LOST k`: frames of that burst lost. "At least": see Loss.
- `BCAST * MISSED submode n_cw [snr]`: a header with nothing else
  decoded. It may be another group's, and only outside a session.
- `BCAST n DROPPED k`: frames of ours thrown out unsent (port closed,
  or a frame too big for the mode). They get no ACKMODE ack.

When our command connection closes, the host closes our ports, puts
port 0 back to its defaults, ends a session gracefully and stops
listening.

## Modes

At 13807c6 `MODES` lists 50 modes at `--kiss-bw 2400` (22 at 500): 200 Hz
`n4-*`, 500 Hz `n10-*` and the FSK modes `fsk16r25-*` and `fsk8r50-*`,
1200 Hz OFDM and the polar modes, `fsk32r62-*` at 2300 Hz, and 2400 Hz
`w48-*`. The polar modes (`polar-k96-f4`, `polar-k96-f8`,
`polar-k192-f8`, 10 and 22 bytes a codeword) are data modes that
`BCAST MODE` takes. The `ack-*` modes carry 4 bytes a codeword and are
for session acknowledgements. Since PR #186 `MODES` carries the S/N
thresholds of `data2g/codes_data/mode_thresholds.json` (GEARBOX.md);
an older host doesn't, and the gearshift falls back on the file.

## A broadcast burst

- One queue for every port. The next burst takes the first queued
  frame's port and mode, then every later frame of that port that fits.
- **The mode is read when the burst is built.** `BCAST MODE` changes the
  mode of frames still queued in the host, not only later ones. HostLink
  is safe: it changes the mode only when the host holds none of our
  frames.
- **Size.** The host aims at 12 s or less, but a burst grows to carry its
  first frame. Receivers take a burst up to 16 s, or longer only as far
  as one 256-byte frame needs (`KISS_FRAME_BYTES`, AX.25's PACLEN), with
  `MODES`' max-codewords as the limit. So 256 bytes is the frame size
  every data mode carries. A longer frame goes out in some modes, but a
  receiver only decodes headers announcing as many codewords as it takes,
  so a longer burst is lost to it.
  Ragtime's 255-byte frames fit every mode.
- **Layout.** Control codewords (a 1-byte header, the group and our call
  in 18 bytes), then the data: `[length, 2 bytes][frame]` back to back,
  zero padded. The receiver checks the control, then the data, under the
  group's key.
- **Broadcast v3** (Data2G PR #121, open at 68dbaea, not merged).
  Header version 3, and a v2 burst is skipped as another kind: v2 and v3
  hosts don't hear each other's broadcasts. Three changes:
  - Each frame is COBS-encoded (no zero byte left in it) and ended by a
    zero byte, back to back, zero padded. It takes at most
    `framed_len(n) = n + 2 + n // 254` bytes: n + 2 up to 253, as the
    length did, 258 for our 255-byte frames. A frame that would start a
    codeword, the data's first included, gets a zero byte before it, one
    byte more. A frame that fills k codewords is k × bytes-per-codeword
    − 3 bytes long (up to 253). Ragtime counts both
    (`link::framedBytes`, `link::burstDataBytes`); on a v2 host that is a
    byte a frame over.
  - `BCAST MODE n [AUTO] DUP mode` sends each control codeword twice,
    RV 0 then RV 1, and a receiver combines the pair when the first copy
    fails alone (about 3 dB on the control). Off by default; Ragtime
    doesn't send it.
  - Each data codeword's mask carries the slot the data began in, so a
    burst whose control was lost is found whatever the control's length.

## Loss

- The codeword is what's lost. A frame is lost if any codeword it touches
  fails.
- **v2 (13807c6): a lost length ends the burst.** The receiver walks the
  lengths. If the codeword holding a frame's length fails, it can't find
  the next frame, so every later frame in that burst is lost too, and
  `LOST` counts them as one. Ragtime's file pieces fill whole codewords
  (`Airtime`, `pieceFrameBytes`), so each frame's length opens a
  codeword, and losing that codeword loses the rest of the burst.
- **v3 (PR #121): a lost codeword loses the frames it touches.** A zero
  byte in a good codeword always ends a frame, so the receiver picks up
  at the next zero after a bad codeword. `LOST k` counts each run of
  frames between good zeros as one, so at least k frames were lost; with
  frames filling whole codewords, as our pieces do, k is exact. A frame's
  start is known only from the zero before it. That is why a frame
  starting a codeword gets a zero of its own there: without it (as at
  d7d2b80), the zero would be the codeword before's last byte, and a
  frame all of whose bytes arrived would be lost with that codeword. The
  team asked for it in its review, and 68dbaea added it.
- **Control lost.** v2 tries the first data codeword under each open
  port's key. v3 looks only when slot 0 didn't decode at all (a slot 0
  that decoded but isn't a broadcast control is some other burst), then
  probes slots 1 to 8, and believes one slot alone only where v2 would
  have. Either way the burst arrives with no call on
  `HEARD`, and Ragtime then leaves its S/N out of the path's.
- No resends at this layer. `docs/broadcast.md` lists incremental
  redundancy across broadcast bursts as not done: our parity (PROTOCOL.md,
  "Parity") is the repair.

## Channel access

- A burst queued while the channel is free goes at once.
- One queued while a burst is being received waits. After that it takes
  each slot with probability (P + 1)/256: P is 63 by default (25%), the
  slot at least 1 s. A client may set P (KISS command 2) and lengthen the
  slot (KISS command 3, in 10 ms units). P is the host's, for every
  client. Ragtime 0.1 sets neither; PR #64 sends P = 191 (75% a slot).
- The hold follows a burst being received, from its preamble on. The
  `BUSY` the host reports is that, held only while the burst's pilots
  check out or energy is 3 dB over the noise floor.
- After 60 s of unbroken busy (`--kiss-busy-limit`) a burst goes anyway.
- Broadcast bursts go only while no session is under way.

## Sessions

- A station already in a session, or not listening, refuses a `CONNECT`
  at once with a NAK (busy, not listening, wrong version, refused), and
  the caller stops dialing.
- **Why a session ended** (PR #122, with `DATA2G ON`): `DISCONNECTED`
  then a reason word, and detail words for some. `CLIENT` (our
  `DISCONNECT`, or the client went), `IDLE` (the idle timeout), `PEER`
  (the far end's DISC: its operator, or its host's idle timeout),
  `ABORT`, `LINK_LOST` (nothing heard for 90 s), `LINK_FAILED why`
  (`no progress`, `link lost`, `protocol: ...`), `NO_ANSWER`,
  `PEER_RECONNECTED` (the partner called again), and the far end's
  refusals: `BUSY`, `VERSION`, `NOT_LISTENING`, `REFUSED` (an unknown
  NAK: `REFUSED reason n`). `CLIENT` and `IDLE` end in `unconfirmed`
  when the DISC was never acknowledged. Without `DATA2G ON`, or from a
  host before it, a bare `DISCONNECTED`, and Ragtime guesses as before
  (`SessionChat`).
- **One host, many clients.** The command port takes any number of
  clients, every one hears each `CONNECTED`, and any one's `DISCONNECT`
  ends the session (`CLIENT`). A VARA client left running on the same
  host, such as Pat listening for Winlink, answers a ragchew too, finds
  no Winlink in it and hangs up as soon as the Hellos have crossed
  (KC2G calling AG7EW, 2026-11-10). Ragtime can't see the other client,
  but a `CLIENT` it never asked for says so ("Another program on
  data2g-host ended the session", `SessionChat::Ending::ClosedHere`).
- A caller who dials again while its old session is still up replaces
  that session.
- The answering side takes the smaller of the two width caps.
- **An idle session is hung up.** With no data either way for 5 minutes
  (`IDLE_CLOSE_S`), or what `TIMEOUT` set, the host disconnects on its
  own (`IDLE` here, `PEER` at the far end). Each host runs its own timer,
  so a longer one keeps a quiet session up only when both ends set it.
  Ragtime sets a day (`HostLink::RAGCHEW_IDLE_TIMEOUT_S`), so a ragchew
  between two Ragtimes ends only on Leave; a far end on another client
  still hangs up after its own. A host before PR #122 can't be told:
  Ragtime then says so from the quiet ("data2g-host closed the quiet
  session", `SessionChat::HOST_IDLE_CLOSE_MS`) rather than "Link lost".
  While idle,
  the caller polls every 15 to 30 s (`KEEPALIVE_S`). With nothing heard
  from the partner for 90 s (`LINK_LOST_S`) the session is lost.

## Hands

Data2G PR #146 (merged at e09e19d, 2026-11-11) adds the Hand, the D2G
net's show of hands (docs/PLAN.md, "On a multiplex link: the Hand
burst"). Ragtime's check-in uses it to call the roll (PROTOCOL.md, "Roll
calls"). Its
design is in Data2G's `docs/hand-burst.md`, the commands in `docs/arq.md`
§9, the KISS side in `docs/broadcast.md` §5, the waveform in
`data2g/hand.py`.

- **What a Hand is.** 63 bits in one of 40 channels, 6.25 Hz 8-FSK with
  a constant envelope, 60 Hz apart round 1500 Hz, 10.56 s long. Channels
  are numbered from the centre out: 0 at 1470 Hz, 1 at 1530, 2 at 1410
  (`link::handChannelHz`). Only the channels inside the host's width
  are taken, set by the client's `BW` alone (`--kiss-bw` is for KISS
  clients and doesn't apply, since 0f277ab): 8 at 500 Hz, 20 at 1200,
  40 at 2400 (`link::handChannelsWithin`, and `HostLink::handChannels`:
  8 with the narrow setting's BW500, else 40). `BW` is the host's, not
  the client's: another command client sending one changes it. A CRC-16 seeded with an 8-bit tag and the channel
  rides under the polar code, so a Hand can't answer another net's call
  or land a channel over. Data2G measures 90% decoded at -19.2 dB in
  noise and -13.8 to -16.3 dB on fading paths.
- **References.** Every burst the host decodes becomes one: `REF id end
  offset snr` (id counting up from 1, the last 64 usable; end in host
  seconds on the sound card's clock; offset the measured frequency
  offset, Hz; snr the pilot SNR) on the command port, to a client that
  sent `DATA2G ON` or a `HAND` command. The command port can't say which
  KISS frames it belongs to, so SETHARDWARE `REFS ON` on the KISS port
  (`C0 06 "REFS ON" C0`, per connection) has the same line come there as
  a REF frame (SETHARDWARE, subcommand `0x11`, then the text) ahead of
  that burst's frames. `HostLink` sends `REFS ON` as the ports come up,
  and each `Heard` frame carries its burst's reference; a host from
  before the PR logs a warning and ignores it.
- **`HAND SEND ref tag d channel hex16`.** Answers burst `ref` with a
  Hand `d` seconds after its end (0 < d < 30), shifted by its offset, so
  it lands in its channel as net control hears it. WRONG for a gone
  ref, bad numbers, a channel past the width, a slot already past or a
  Hand already waiting. Then `HAND SENT c` as it keys, or `HAND DROPPED
  c` if the radio was busy more than 0.5 s past the slot.
- **`HAND LISTEN d tag ack channel...`.** Net control's window: `d`
  seconds (1 <= d < 30) after the end of the burst carrying the KISS
  frame sent with ACKMODE tag `ack` (4 hex digits), 10.56 s plus 0.6 s
  either side. Each Hand decoded comes as `HAND c hex16 snr dt` (dt its
  lateness, ms), then `HAND SHORT ms` if our own burst ran into the
  window's head, then `HAND END`; an armed window whose burst never goes
  gives up after 30 s. Send it before the burst's frames: `HostLink`
  holds the burst until the host's OK (`handListen`).
- **`HAND CANCEL`** stops a waiting Hand and any window, always OK. A
  command client that leaves cancels its own.
- **Holds.** While a Hand waits or a window is open the host starts no
  CQ, broadcast, ID or KISS burst of its own; session bursts still go.
  So broadcasts queue behind a round, for at most about 42 s.
- **Numbers** are strict: digits only, and the delay digits and a point
  (`link::handDelayText`), never an exponent or a comma.
- **Does this host do Hands?** No command says. A host from before the
  PR answers `HAND` with `WRONG`, but only after a roll call has gone, so
  Ragtime waits for a reference instead: a REF line or frame for any
  burst decoded shows it (`HostLink::handsSeen`), and until one comes a
  check-in goes as a plain Hello.

Not in it, from our review (2026-11-09): the 10-minute forced CW ID
still ignores the Hand's holds (`CW_FORCE_S` in `data2g/arq/engine.py`
at e09e19d), and `TXPOWER` still isn't reset
when the client goes. `docs/hand-burst.md` recommends a callsign in the
payload, which Jeff has decided for Ragtime (2026-11-08).

## Not in Data2G yet (asks)

- `TXPOWER` back to the station's own level when the client goes, as
  `TIMEOUT` is (asked in the PR #122 review), so a Ragtime that quits
  doesn't leave the next client a level it set.
- The mode on `BCAST HEARD` (the path log's mode column).
- A dial report from the host's own rig control.
- A 200 Hz session cap.
- The 10-minute forced CW ID made to wait for a Hand or an open window
  (PR #146 merged without it).
- A note when a v2 broadcast is heard by a v3 host, so an operator knows
  to update.
