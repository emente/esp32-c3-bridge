# Connecting an SD card module

## Hardware

This firmware runs on the "D1 Mini ESP32" board

## Pin plan

| Signal            | GPIO | Board silkscreen label |
|-------------------|------|-------------------------|
| Sniffer UART RX   | 16   | `IO16` / `U2RX`         |
| Sniffer UART TX   | 17   | `IO17` / `U2TX`         |
| SD card SCK       | 18   | `CLK`                   |
| SD card MISO      | 19   | `MISO`                  |
| SD card MOSI      | 23   | `MOSI`                  |
| SD card CS/SS     | 5    | `SS`                    |

## Wiring an SPI SD card module

| SD module pin | Connects to      |
|----------------|-------------------|
| GND            | GND               |
| VCC            | 3V3 (see below)   |
| MISO           | GPIO19 (`MISO`)   |
| MOSI           | GPIO23 (`MOSI`)   |
| SCK            | GPIO18 (`CLK`)    |
| CS             | GPIO5 (`SS`)      |

Power: The ESP32's GPIOs are 3.3V logic. Most cheap SD breakout
modules have an onboard 5V-to-3.3V regulator for VCC *and* resistor
level-shifting on the logic lines, so they're safe to power from 5V --
but some bare/minimal modules are 3.3V-logic-only. Check your specific
module; when in doubt, power it from the board's `3V3` pin rather than
`5V`/`VCC`, which sidesteps the question entirely (every such module
accepts 3.3V on its VCC pin, since that's the chip's native voltage).

Card format: the Arduino `SD` library expects a FAT16/FAT32-formatted
card. Stick to SDHC cards (≤32 GB) formatted FAT32 -- exFAT (common on
larger/newer cards) isn't supported.

## What the firmware does with it

On boot, if a card is present the firmware starts a new log file at
`/logs/logNNNNN.its5` (`NNNNN` = one more than the highest-numbered
existing file, so a fresh file is created every power cycle and nothing
already on the card is ever overwritten or appended to). Every captured
packet is appended to that file in the same "ITS5" framing the sniffer
already uses on the wire, so a log file is a direct byte-for-byte replay
source.

New CLI commands (`pio device monitor`, or the same shell over USB):

- `sd` -- card status, free/used space, log file count, packets written
  since boot. Also reported in the periodic MQTT stats payload (`"sd":
  {"found": bool, "packets_written": N}`).
- `sdreplay [delete]` -- replays every log file's packets straight to MQTT
  (both configured brokers, same as live traffic), by feeding each file
  through the same ITS5 parser used for the live sniffer stream. Each
  publish is paced with a small delay, both so a big replay doesn't burst
  the broker with packets far faster than any live capture would, and
  because that pacing is what keeps the task watchdog fed during a replay
  long enough to matter. With `delete`, each file is removed once it's
  been fed to MQTT -- "fed" meaning attempted, the same delivery semantics
  a live packet already has (not a confirmed-received guarantee, which
  QoS 0 doesn't provide anyway); the file currently being logged to this
  session is never deleted even if asked.
- `sddelete yes` -- deletes every log file in `/logs` (the `yes` is
  required, to avoid an accidental one-word wipe) and starts a fresh one.
- `ls [dir]` / `dir [dir]` -- list an SD card directory (defaults to the
  current one).
- `cd [dir]` -- change the SD card's current directory (no argument goes
  to `/`).
- `rm <file>` -- delete one file from the SD card.
- `format yes` -- recursively deletes *everything* reachable on the card,
  not just `/logs`. Not a real low-level FAT format (the SD library used
  here doesn't expose one), just a thorough recursive delete.

(The internal LittleFS filesystem -- the web UI's static files, unrelated
to the SD card -- keeps its own listing under `lsfs`, the previous name
for what used to be plain `ls` before that name moved to the SD card
above.)
