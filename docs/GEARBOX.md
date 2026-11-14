# The gearshift

How Ragtime picks a Data2G mode. Jeff asked for a gearshift built on the
Data2G Submode Map (2026-10-22). This replaces Glissando's five tempo
speeds.

## What the operator sees

One popup, opened from the Gear button by Send (a speedometer with the
gear's short name under it) or Settings > Gearshift (Ctrl+G). Every
choice in it takes effect at once, so it closes with **Done** and a green
check, never "Close" or "Save" (Jeff, 2026-10-25: "X Close" read as
cancelling); Escape and the title bar's X do the same. It doesn't hold
up the main window: the chat stays in use beside it, and what changes
there (Auto following the S/N, the 500 Hz switch) shows in it at once.
It stacks as any other window, unless View > Keep tool windows on top
is ticked (off by default; Andrew, 2026-11-08). The popup *is* the
Submode Map:

- Speed across (bits per second, log scale from 20 to 10k), the S/N each
  mode needs down the side (harder further down), one dot per mode,
  coloured by modulation: FSK `#0f766e`, QPSK `#1f5fbf`, 16-QAM `#7c3aed`,
  64-QAM `#b8326b`, 256-QAM `#9a3412`, polar `#a16207`. Hovering a dot
  shows its short name and Data2G name, width, modulation, speed and
  threshold.
- **Width** chips, narrowest to widest: 200 Hz, 500 Hz, 1.2 kHz, 2.4 kHz
  (the map's four groups). A width is a cap: 2.4 kHz still uses the
  narrow modes. Modes wider than the cap are hollow grey circles. The
  width only picks which modes the gears come from: picking 500 Hz to
  get a narrow, easily heard mode leaves data2g-host free to use any
  width (Jeff, 2026-10-22).
- The **500 Hz switch** on the connection bar is what limits data2g-host:
  on, it uses only 500 Hz modes (`BW500`); off, any width (`BW2300`).
  While it is on the gears come only from 500 Hz modes too: the 1.2 kHz
  and 2.4 kHz chips grey out, and turning it off brings back the width
  chosen before.
- **Path** chips: Calm (no fading, AWGN), Good, Poor, Disturbed (the ITU
  F.1487 fading channels). The path picks the gearbox, because modes rank
  very differently once the path fades: polar modes rise on a good fading
  path and the QAM modes sink.
- The **gears** are knobs bearing their short names (below, "Mode
  names"), joined by a heavy line; every other
  best mode sits on a thin dashed line. The current gear is filled blue.
- **Auto**: the path's S/N is a dashed line. It is the weakest station
  heard in the last 15 minutes, by the mean of its last 5 bursts' SNRs
  as data2g-host reported them (Data2G PR #98), since a broadcast must
  reach every station; everything needing more than that, less a 3 dB margin, is
  shaded. Auto takes the fastest gear above the shading (ringed in green
  when Auto is off, as a suggestion).
- **QRN** (Jeff, 2026-10-26): with a rig whose S-meter Ragtime can read
  (Settings > Rig, rigctld or Hamlib), a blue dashed line at S/N 0 is
  the band's noise, labelled with the radio's own number, `QRN: -40`
  (Hamlib's `STRENGTH`, dB over S9). The noise is the quietest reading
  of the last minute, read every 5 s between bursts and not in the
  1.5 s after one (`radio::NoiseFloor`), so a station's burst doesn't
  lift it; a new band starts afresh. An axis on the right, "On your
  S-meter", says what a station at each gear's S/N reads there, S1 to
  S9 (S9 is 0 dB, 6 dB an S-unit) and then S9+10 and up. The map keeps
  its S/N scale: the noise moves the S-units, not the gears (Jeff chose
  this over the whole chart riding the noise).
- **Under the needle** (Jeff, 2026-10-27): Data2G's
  S/N is the signal over the noise in 2500 Hz, but the S-meter reads
  the signal and the noise together, as powers, across the whole
  receive filter. With the noise N on the S-meter and a filter B Hz
  wide, a station at S/N x reads

      N + 10 log10(1 + 10^(x/10) x 2500/B)  dB   (radio::meterRiseDb)

  so a strong station reads its S/N over the noise, moved by
  10 log10(2500/B), and a weak one hardly moves the needle: at S/N 0 in
  a 3000 Hz filter it reads 2.6 dB over the noise, at -12 dB 0.2 dB.
  The axis's ticks sit where that puts each S-unit (`snrForRise`).
  Gears for stations that lift the needle less than half an S-unit
  (3 dB, `NEEDLE_DB`) are shaded blue, "under the needle": they copy
  stations the S-meter can't show, and the axis there is a bar at the
  noise's own S-unit. A narrow filter lifts the needle sooner (S/N 0
  reads 7.8 dB over the noise in 500 Hz), so the shading shrinks. B is
  the rig's filter, read while the gearshift is open (`+m`, and PBT, as
  for the Ear), narrowed by its twin PBT with Calibrate's model where
  there is one; the axis's title gives it, "On your S-meter, 2400 Hz
  filter". Not read (no mode from the rig, or FM), it is taken as
  2500 Hz. Few S-meters are 6 dB an S-unit, so the axis is a guide;
  hovering the QRN label or "under the needle" says so.
- The gear list beside the map: the gear (its short name), Data2G mode,
  speed, S/N on this path, and how long 1 KB takes. Gears go by their
  names, not numbers (Jeff, 2026-10-22). Once the path's S/N is known,
  gears needing more than it less the 3 dB margin are red, in the list
  and on the map (Jeff, 2026-10-22).
- **Any mode** under the gear list picks a mode that is not a gear, from
  every mode drawn within the width, slowest first; clicking its dot on
  the map does the same. It is ringed in blue, turns Auto off, and holds
  while the mode is drawn within the width (meanwhile the gear falls back
  as for a width change). Picking a gear clears it.
- Leave out the `ack-` modes: they carry the host's own ARQ control.

The Gear button by Send is the one way in (Jeff, 2026-10-23), last in
the row after Send and the file button, as on the Session tab: a
speedometer with the gear's short name under it in small type (`E2C`),
the dial's foot 2 px over the name and the button no taller than Send,
its orange needle pointing at the gear's speed (Jeff, 2026-10-24) on a
log scale from 16 bit/s at the left end to 10 kbit/s at the right, the
five ticks at 16, 80, 400, 2000 and 10000 bit/s (`gearbox::dialPosition`),
and the Data2G name, speed, S/N and width in its tooltip. The connection
bar keeps only the `500 Hz` switch. FSK, PSK and the rest show only in the popup.

## How the gears are chosen

For each width cap and path:

1. Keep the modes no wider than the cap that have a threshold on this
   path (a missing threshold means the mode never reached 90% copy).
2. The best modes are those no other mode beats on both speed and S/N:
   sort by threshold, and keep each mode faster than every one before it.
   That leaves 2 to 26 modes, too many to shift through.
3. Gear 1 is the most robust best mode. Each next gear is the most robust
   best mode at least 1.8 times faster than the gear below, so every shift
   roughly doubles the speed. The fastest best mode is always the top
   gear (it replaces the last gear if it is less than 1.3 times faster).

`tools/gearbox/gearbox_ladder.py` does exactly this. On every path,
2.4 kHz gets 8 gears.

## Mode names

Outside the popup a mode goes by a three-character short name, Jeff's idea
(2026-10-22): a letter for how strong the signal must be, then how fast it
goes. `A32` scrapes through at −12.5 dB and moves 32 bit/s; `Z8k` needs
a quiet band and moves 8000. The Gear button, the status bar and the
message tooltips use them; the popup shows both.

**The letter** comes from the S/N the mode needs on a Calm path, whichever
path is picked, so a mode keeps its name when the path changes:

    letter = A + round((S/N + 12.5 dB) / 1.25 dB)

A is −12.5 dB, each letter is 1.25 dB more, K is 0 dB and Z is +18.75 dB.
Every four letters is 5 dB, so two names a few letters apart are a few
dB apart. Anything below A reads A and anything above Z reads Z. (Halves
round up.)

**The speed** is the payload rate, rounded like the speed column (halves
to even):

| Speed | Written | Examples |
| --- | --- | --- |
| below 100 bit/s | the bit/s | `29`, `72` |
| 100 to 949 | hundreds, then C | `1C` (99.5 to 149), `9C` |
| 950 and up | thousands, then k | `1k`, `8k` |

**Ties.** Two modes can come out the same: on the Submode Map,
`n4-qpsk-r2/3` and `n4-16qam-r1/3` are both `F2C`. Names are given
easiest first (lower Calm S/N, then faster, then by Data2G name), and a
mode whose name is taken moves up one letter until it finds a free one,
so the harder mode is the one that moves and its name never promises more
than it does. A mode that would move past Z, or has no Calm figure or no
speed yet, has no short name and shows its Data2G name. Names are worked
out over every mode Ragtime knows of, not just those the host offers, so
they don't change with the host's list.

On the Submode Map, 12 of the 45 modes move, most by one letter; the
farthest is `n10-qpsk-r1/3`, `F2C` to `I2C`. The 2.4 kHz gears read:

| Path | Gears 1 to 8 |
| --- | --- |
| Calm | `A32` `C72` `E2C` `G3C` `J6C` `M2k` `Q3k` `Z8k` |
| Good | `A32` `E69` `G1C` `G3C` `J6C` `M2k` `Q3k` `Z8k` |
| Poor | `A32` `C72` `F1C` `G3C` `J6C` `M2k` `Q3k` `Z8k` |
| Disturbed | `A32` `C72` `H2C` `G3C` `J6C` `M2k` `Q3k` `V7k` |

The letters always follow the Calm path, so on a fading path a gear can
read a lower letter than the one below it (`H2C` then `G3C`).

Speeds from the host's `MODES` win, so the digits follow the host
(the map's 32 bit/s may read `A29`), and a newer Data2G's thresholds can
move a letter by one. If Andrew's `MODES` ever reports these names,
Ragtime should take them from there.

## The figures

From the Data2G Submode Map (Jeff shared it 2026-10-22): the 10% failure
point of the smallest ARQ data burst (one control codeword and one data
codeword), 200 trials per point, S/N as average transmitted power over
noise in 2500 Hz. Longer bursts need a little more. Speed is the raw
payload rate without CRC or burst overhead.

Whether the polar modes can carry broadcast and session data, and be
picked with `BCAST MODE`, is an open ask: they are gears 2 and 3 on a good
fading path.

## Where Ragtime gets the figures

Nothing of Data2G's is built in (Jeff, 2026-10-22: "Nothing built in,
find Data2G"). Ragtime reads the figures at run time, each source adding
to or updating the one before:

1. **A Data2G checkout.** On launch Ragtime looks for data2g-host's own
   `data2g/codes_data/mode_thresholds.json` under `$DATA2G_HOME`, then
   under `Data2G` or `data2g` in the home folder or in its `src`, `git`,
   `code`, `projects`, `Projects`, `repos` or `dev` folder. Data2G is run
   from a clone (`uv sync`), so every operator running data2g-host has
   this file. Its form is `{"channels": ["awgn", "mpg", "mpp", "mpd"],
   "modes": {name: [dB, ...]}}`, 99 for a threshold never reached. It has
   no widths or speeds; those come from `MODES`.
2. **A thresholds file** picked with **Load thresholds...** in the popup
   (remembered as `/Gearshift/ThresholdsFile`), or else
   `submode_thresholds.json` in Ragtime's data folder
   (`~/.local/share/ragtime` on Linux, or `$XDG_DATA_HOME/ragtime`;
   `%APPDATA%\Ragtime` on Windows). Either form works: Data2G's
   own, or the Submode Map's JSON as `tools/gearbox/gearbox_ladder.py`
   reads it (`{name: {"hz", "mod", "rate", "bps", "th": {"awgn", ...}}}`,
   null for never), which carries widths and speeds, so it gives gears
   before the host connects.
3. **The host's `MODES` list.** Gears use only the modes the host offers.
   Its width and speed win over the file's: the speed is the payload rate
   from its airtimes, each codeword past the first adding
   (time at most codewords − time at one) / (most codewords − 1) seconds
   for its bytes. Thresholds in `MODES` (Data2G PR #186: `CHANNELS`,
   then a `THRESHOLD` line per mode) win too, a `-` there taking away a
   file's threshold. With them the popup names data2g-host as the source
   and hides **Load thresholds...**, so no file is needed.

With no thresholds found, the popup says where Ragtime looked. With
Data2G's file but no host yet, it says the gears appear once data2g-host
is connected.

The figures move as Data2G improves. Data2G's own file at `674c268`
(2026-10-22) differs from the Submode Map by up to a dB or so, enough to
change some ladders (with the map's speeds): on a Calm path at 500 Hz and
wider, `n4-qpsk-r1/3` and `n4-qpsk-r2/3` become gears 2 and 3, and on a
Good path at 1.2 kHz and wider the ladder starts at `polar-k96-f8`, with
`polar-k192-f8` in place of `polar-k96-f4`, one gear fewer.
Reading the operator's own file keeps the gears in step with the host they
run. The tables below are the Submode Map's.

## In the code

- `core/include/ragtime/gearbox/Submodes.h`: `SubmodeTable` (both file
  forms, `setFacts` and `setThreshold` for `MODES`),
  `payloadBitsPerSecond`, paths, modulations and their colours.
- `core/include/ragtime/gearbox/Gearbox.h`: `bestModes`, `gearLadder`
  (the same ladders as `gearbox_ladder.py`, checked on all 16 gearboxes),
  `autoGearIndex` with the 3 dB margin, the short names (`snrLetter`,
  `speedShorthand`, `shortNames`), `PathSnr` (the mean of the last
  5 bursts), the widths, the labels, and
  `Gearshift`, the popup's state (`setNarrowOnly` is the 500 Hz switch;
  `effectiveWidth()` is the width the ladder uses). Changing the width or path keeps the
  chosen mode if it is still a gear, or else takes the fastest gear no
  faster than it, so the gearbox never shifts up on its own. Auto with no
  S/N reports yet holds the chosen gear. `selectMode` chooses any mode
  (`chosenMode()`, a `Gear` numbered 0 when off the ladder).
- `app/src/GearshiftDialog.*`: the popup, opened by the Gear button
  (`GearshiftIcon.*` draws its speedometer and name as one picture) and Settings > Gearshift
  (Ctrl+G). `ToggleSwitch.*` is the 500 Hz switch. `app/src/Thresholds.*`:
  finding and reading the file. `Gearshift::shortName` names a mode for
  the status bar and message tooltips.
- `tests/GearboxTest.cpp` uses made-up placeholder figures, never
  Data2G's.

## Every gearbox

**200 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/5` | 51 | −10.1 dB |
| 2 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 3 | `n4-qpsk-r2/3` | 176 | −6.8 dB |

**500 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 4 | `n10-qpsk-r1/2` | 333 | −3.5 dB |
| 5 | `n10-16qam-r1/2` | 672 | +1.2 dB |
| 6 | `n10-64l-r3/4` | 1544 | +8.6 dB |
| 7 | `n10-256l-r3/4` | 2067 | +13.4 dB |

**1200 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 4 | `qpsk-r1/5` | 319 | −4.9 dB |
| 5 | `qpsk-r1/2` | 806 | +0.1 dB |
| 6 | `n10-256l-r3/4` | 2067 | +13.4 dB |

**2400 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `fsk32r62-r1/2` | 151 | −7.9 dB |
| 4 | `qpsk-r1/5` | 319 | −4.9 dB |
| 5 | `w48-qpsk-r1/5` | 639 | −1.6 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +2.4 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +7.6 dB |
| 8 | `w48-256l-r5/8` | 8222 | +18.5 dB |

**200 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/5` | 51 | −1.3 dB |
| 2 | `n4-qpsk-r1/2` | 134 | +0.3 dB |
| 3 | `n4-16qam-r1/3` | 176 | +2.1 dB |

**500 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `n4-qpsk-r1/3` | 88 | +0.1 dB |
| 3 | `n4-16qam-r1/3` | 176 | +2.1 dB |
| 4 | `n10-qpsk-r1/2` | 333 | +5.7 dB |
| 5 | `n10-16qam-r1/2` | 672 | +10.9 dB |
| 6 | `n10-64l-r3/4` | 1544 | +18.9 dB |
| 7 | `n10-256l-r3/4` | 2067 | +25.8 dB |

**1200 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `polar-k96-f8` | 69 | −1.9 dB |
| 3 | `polar-k96-f4` | 139 | −1.2 dB |
| 4 | `qpsk-r1/5` | 319 | +1.1 dB |
| 5 | `qpsk-r1/2` | 806 | +6.1 dB |
| 6 | `n10-256l-r3/4` | 2067 | +25.8 dB |

**2400 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `polar-k96-f8` | 69 | −1.9 dB |
| 3 | `polar-k96-f4` | 139 | −1.2 dB |
| 4 | `qpsk-r1/5` | 319 | +1.1 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +4.1 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +8.4 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +13.8 dB |
| 8 | `w48-256l-r5/8` | 8222 | +25.8 dB |

**200 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 2 | `n4-16qam-r1/3` | 176 | −2.1 dB |

**500 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `n10-qpsk-r1/2` | 333 | +1.6 dB |
| 5 | `n10-16qam-r1/2` | 672 | +11.6 dB |

**1200 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `qpsk-r1/5` | 319 | −1.0 dB |
| 5 | `qpsk-r1/2` | 806 | +4.1 dB |
| 6 | `16qam-r1/2` | 1639 | +9.9 dB |

**2400 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `qpsk-r1/5` | 319 | −1.0 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +2.6 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +7.2 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +12.4 dB |
| 8 | `w48-256l-r5/8` | 8222 | +26.2 dB |

**200 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/3` | 88 | +1.8 dB |
| 2 | `n4-16qam-r1/3` | 176 | +4.1 dB |

**500 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `n10-qpsk-r1/3` | 222 | +0.9 dB |
| 4 | `n10-16qam-r1/3` | 439 | +4.6 dB |
| 5 | `n10-16qam-r1/2` | 672 | +8.9 dB |

**1200 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `polar-k192-f8` | 153 | −2.2 dB |
| 4 | `qpsk-r1/5` | 319 | −0.2 dB |
| 5 | `qpsk-r1/2` | 806 | +4.2 dB |
| 6 | `16qam-r1/2` | 1639 | +10.3 dB |

**2400 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `polar-k192-f8` | 153 | −2.2 dB |
| 4 | `qpsk-r1/5` | 319 | −0.2 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +3.4 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +7.0 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +11.9 dB |
| 8 | `w48-64l-r2/3` | 6556 | +23.1 dB |
