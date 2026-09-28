# RESEARCH.md — raw GPU temperature sensor research notes

Research log for reading NVIDIA GPU temperatures directly from PCI BAR0
(no NVML, no proprietary userspace libs), targeting the hwmon channels of
this module. All findings verified on a Palit RTX 3090 (GA102, PCI ID
0x2204) at `0000:81:00.0`, water-cooled, kernel 7.0.0-34-generic with the
proprietary driver `nvidia` 595.91.07 (GSP-RM active). BAR0 phys:
`0xb2000000`.

Test tools:
- `gddr6` (userspace, sudo, `/dev/mem`) — the test bed for any new sensor
  before it lands in this module.
- `dump_regs` (throwaway root tool, see `--watch` mode) — raw BAR0 dumper:
  one-shot dump of the NV_THERM page `0x20400..0x204FF` plus the VRAM
  sensor registers, and a watch mode printing nvidia-smi's
  `temperature.gpu` next to decoded sensor values every 2 seconds.

## ⚠ DANGER: wide BAR0 scans

A one-shot scan that swept the PMA region `0x20000..0x21000` and the FB
region `0xE000..0xF000` (`dump_regs --scan`) **corrupted the GPU state**:
`nvidia-smi` stopped returning any data and the running CUDA workload
(llama.cpp) crashed with "illegal access". The state recovered on its own
within minutes, but the cause (read-on-reset registers, or firmware-
internal PMU/FB state) is unknown. **Wide register scans are on hold.**
Only read the known, individually verified offsets (the ones `--watch`
uses). If scanning resumes: narrow ranges, right after a reboot, with no
workloads attached.

## Confirmed registers

| Offset   | Sensor                        | Decode                        | Status      |
|----------|-------------------------------|-------------------------------|-------------|
| 0x0000E2A8 | GDDR6/GDDR6X memory junction (GA102/AD*; GA104 uses 0x0000EE50) | `(raw & 0xFFF) >> 5`, 1/32 °C resolution | confirmed |
| 0x00020460 | GPU core (NVML_TEMPERATURE_GPU equivalent) | fixed-point, bits [15:8] = integer °C; valid if bit 29 or bit 30 set | confirmed |
| 0x0002046C | GPU hotspot / die junction   | same as core                  | confirmed   |

### GPU core (0x00020460)

- Tracks `nvidia-smi temperature.gpu` with an offset measured at
  **K = raw_full − nvsmi ≈ +2.0 °C** over the 27..42 °C range (load and
  cooldown in sync). See "Core sensor calibration" below; the offset is
  pending cross-card verification (per-architecture constant vs per-die).
- The open-source nouveau driver (`nvkm/subdev/therm/gp100.c`,
  `gp100_temp_get()`) reads the same offset with the same mask
  (`raw & 0x1FFF8) >> 8`), checks bit 29 as "primary valid" and only traces
  bit 30 ("SHADOWed sensor"). It refuses to work at all when GSP-RM is
  active (`nvkm_gsp_rm()` → `-ENODEV`). On a GSP-RM system the firmware
  populates the shadow sensors, so **bit 29 stays clear and bit 30 is set**
  — this is why a bit-29-only check reports N/A under the proprietary
  driver. Correct availability test: `bit 29 OR bit 30`, plus raw value not
  `0xFFFFFFFF` / `0`.
- `0x00020474` is a register-by-register **mirror** of `0x00020460` (identical
  in every sample so far); `0x00020478` mirrors `0x0002046C`.

### Memory (VRAM) sensors

- **Junction register**: `0x0000E2A8` on GA102/AD* chips; `0x0000EE50` on
  GA104 (RTX 3070) cards. On GA102, `0xEE50` contains garbage
  (raw `0xc1b477db`) — do not read it on non-GA104 cards.
- Decode: 12-bit value in bits [11:0], 1/32 °C per LSB.
- **2 °C firmware quantization confirmed**: every reading is an even
  integer degree (raw always a multiple of 64); the value moves in exact
  2 °C steps (52, 54, 56, 58, 60 / 60, 58, 56, 52, 50, 48, 46).
- On Windows, OCCT shows 24 per-chip GDDR6X temperatures, and this
  register always matches the **hottest** of the 24 chips exactly.
- A reliable source states the 3090 has **exactly 24 per-chip memory
  sensors** (one per GDDR6X chip). The NV_THERM page holds at most ~12
  live slots, so the per-chip sensors are not (all) there — they most
  likely live in the FB register region around `0xE2A8` (or in further
  NV_THERM instances). **Location unknown; scanning that region is on
  hold** because of the corruption incident (see DANGER note above).
- The unidentified NV_THERM sensors (`0x20464`, `0x20470`, `0x2047C`,
  `0x204B8`, ...) stay below the junction in every measurement — they may
  be die-side sensors (power stage / memory-controller area) rather than
  DRAM chips.

## Fixed-point fraction (CONFIRMED)

The low byte [7:0] of the NV_THERM sensor registers is a **fraction × 256**
(1/256 °C per LSB): e.g. `0xc0001cc0` → 28.75 °C.

**Confirmed by watch data**: while cooling, r20460 walked smoothly and
monotonically through 34.06 → 34.00 → 33.87 → 33.81 → 33.68 → 31.62 →
31.28 → 31.09 → 30.84 → ... → 29.90 (±0.1-0.2 °C sensor noise between
2 s samples), and r2046C the same way (45.53 → 45.28 → 45.09 → ... →
37.96). A service or random low byte would not track the temperature
like that. This also explains why OCCT (Windows) reports GPU and hotspot
temperatures to hundredths of a degree.

Millidegrees for hwmon: `((raw & 0xFFFF) >> 8) * 1000 + (raw & 0xFF) * 1000 / 256`.

## Core sensor calibration (raw vs NVML)

With the fraction available, the offset K = raw_full − nvsmi was measured
at several NVML levels (average per level, water-cooled card):

| nvsmi | raw_full (avg) | K = raw − nvsmi |
|-------|----------------|-----------------|
| 28    | 30.18          | 2.18            |
| 29    | 30.92          | 1.96            |
| 30    | 31.62          | 1.62            |
| 32    | 33.84          | 1.84            |
| 39    | 41.22          | 2.22            |
| 40    | 42.00          | 2.00            |

The per-level spread of K (≈0.5-0.6 °C) is exactly what nvidia-smi's
integer rounding produces against a continuous raw value. The level
centers average ≈ 1.98 — a **constant +2.0 °C offset** fits the data
without a gain term (a linear fit gives slope ≈ 0.985-1.00,
indistinguishable from 1.0 at this precision).

Status: measured on one die. gddr6 currently subtracts 2.0 °C
(`CORE_CALIB_OFFSET_C`; set to 0 to display raw). Whether the offset is
a per-architecture constant or per-die must be checked on the air-cooled
card (wide range) before it is applied in the kernel module.

## NV_THERM window map (0x20400..0x204FF), two idle snapshots

```
                 snap1 (nvsmi=32)   snap2 (nvsmi≈28)
0x20460  core          34             28
0x20464  ?             42             36
0x20468  ?              0 (dead)      0 (dead)
0x2046C  hotspot       45             36
0x20470  ?             38             29
0x20474  core mirror   34             28
0x20478  hspt mirror   45             36
0x2047C  ?             38             33   <-- NOT a mirror of 0x20470
0x204B8  ?             n/a            29   == 0x20470
0x204BC  ?             n/a            36   == 0x20464
0x204C0  ?             n/a            29   == 0x20470
0x204C4  ?             n/a            36   == 0x20464
```

Observations:

- `0x20474 == 0x20460` and `0x20478 == 0x2046C` in every sample (mirrors).
- `0x2047C` tracked `0x20470` closely in snap1 but diverged in snap2
  (33 vs 29) — an **independent** sensor, not a shadow copy.
- `0x20464` equals `0x2046C` in snap2 but was 3 °C lower in snap1 —
  independent as well.
- `0x204B8/0x204BC/0x204C0/0x204C4` duplicate `0x20470`/`0x20464` values —
  likely additional mirror copies of the same physical sensors.
- `0x20468` is permanently 0 (with availability flags set) — unused slot.
- Poison pattern `0xBAFD5040` marks many unused slots in the page.
- Config-like values: `0x204a0`=0x0800, `0x204a4` (b30 set, value 0),
  `0x204ac`/`0x204b0`=0x60, `0x204e0`/`0x204e4`=`0x0007FFFF` (≈127.99 °C in
  the fixed-point encoding — plausible max-temperature limit),
  `0x204c8`≈114.25, `0x204fc`≈112.00 — plausible alarm thresholds.
- Bit 31 is **dynamic** (observed flipping on `0x2046C` between snapshots),
  not a static sensor-class flag.

## Load + cooldown watch results (llama-server load, 24 + 27 samples)

Typical values per phase:

| sensor            | under load (nvsmi 36-38) | after load (nvsmi 33-28) |
|-------------------|--------------------------|--------------------------|
| vram (0xE2A8)     | 52 -> 60, rising         | 60 -> 46, falling        |
| core (r20460/74)  | 38-39 (nvsmi+2)          | 34 -> 29 (nvsmi+1..2)    |
| hotspot (r2046C/78)| 49-50 (highest)         | 46 -> 37                 |
| r204BC/r204C4     | 47-49                    | 44 -> 37                 |
| r20464            | 46-47                    | 42 -> 37                 |
| r20470            | 44                       | 39 -> 30                 |
| r2047C            | 43-44                    | 39 -> 34                 |
| r204B8/r204C0     | 41-42                    | 37 -> 30                 |
| r20468            | 0 (dead)                 | 0 (dead)                 |

Findings:

- **VRAM 2 °C quantization confirmed**: every single reading is even, and
  the value moves in exact 2 °C steps (52, 54, 56, 58, 60 / 60, 58, 56,
  52, 50, 48, 46); the low-5-bits marker never fired.
- **Strict mirrors** (identical in every row): r20474=r20460 (core),
  r20478=r2046C (hotspot), r204C0=r204B8.
- **r2047C is independent of r20470**: on cooldown r20470 fell 39->30 while
  r2047C only fell 39->34 (+4 lag). A mirror cannot do that; it is a
  separate sensor with a different thermal mass (or, less likely, a
  deliberately delayed shadow).
- **r204BC/r204C4 track r20464** with a small offset (+1..+2 while
  heating, converging at steady state) — shadow copy or adjacent sensor,
  undecided.
- **Memory-bank hypothesis gains support**: under load there are 5 (or 4,
  if r204BC/C4 are shadows of r20464) distinct values below the junction,
  and on cooldown they **diverge** (38 / 34 / 30) — independent physical
  sensors with different thermal masses. The junction register (0xE2A8)
  stays the maximum in both phases, consistent with "junction = max over
  banks" and with OCCT's 24 chip values being extrapolated from ~4 bank
  sensors (24 chips / 6 per bank).
- **Core offset stays constant**: +2 °C under load, +1..+2 °C on cooldown,
  matching NVML throughout.
- VRAM heating is decoupled from the die (junction rose 52->60 while the
  core stayed flat at 38-39 under sustained compute+memory load).

## Per-chip memory sensors (location unknown)

The 3090 has 24 GDDR6X chips. A reliable source states there are
**exactly 24 per-chip sensors**, and OCCT (Windows) shows all 24 values;
the junction register (0xE2A8) always equals the hottest of them.

This supersedes the earlier "4 bank sensors × 6 chips" guess: with 24 real
sensors, the unidentified NV_THERM slots (`0x20464`, `0x20470`, `0x2047C`,
`0x204B8`, ...) — of which there are far fewer than 24 — are most likely
**not** the DRAM chip sensors (die-side power/memctrl sensors instead),
and the per-chip sensors must be found in a different register region
(most plausibly the FB area around `0xE2A8`).

Finding them is **on hold**: the wide scan that would reveal them
corrupted the GPU state (see DANGER note). Next attempt: narrow ranges,
after a reboot, no workloads.

## Open kernel module findings (tag 595.91.07)

`NVIDIA/open-gpu-kernel-modules` contains, for GA102:

- the host-side kernel module and RM kernel code (API plumbing only);
- the **GSP firmware embedded as a binary**
  (`src/nvidia/generated/g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c`) —
  no GSP source in the tree;
- **no published register headers for the Ampere NV_THERM window**
  (unpublished swref; only Hopper/Blackwell `dev_therm.h` exist, and they
  contain boot-completion scratch registers, not temperature sensors);
- **no GPU thermal subdevice code** for Ampere — the actual sensor read and
  the physical handler of the `NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2` API
  live in the closed GSP/PMU firmware.

Conclusion: the raw register decode + per-die calibration that NVML applies
cannot be recovered from open source. The BAR0 raw read (identical to
nouveau) is the limit of the open path, and the +1..+2 °C offset vs
nvidia-smi is expected and documented in the README.

## Validity bits (NV_THERM sensor registers)

| Bit  | Meaning                                                        |
|------|----------------------------------------------------------------|
| 29   | primary sensor reading valid (cleared under GSP-RM)            |
| 30   | reading from a SHADOWed sensor (set under GSP-RM)              |
| 31   | dynamic status flag (not a sensor-class marker)                |

Invalid readings: raw `0x00000000`, raw `0xFFFFFFFF`, or neither bit 29 nor
30 set.

## Status and TODO

Done:
- gddr6: "GPU Core" column implemented (valid = bit 29 or 30), verified
  against nvidia-smi.
- nv-sensors-extra 1.1: `temp3` (GPU core) added, DKMS-installed and
  verified; temp1/temp2 unchanged.
- Fixed-point fraction **confirmed** (smooth monotonic tracking in watch
  data); gddr6 now prints VRAM/hotspot/core to hundredths.
- Core calibration offset measured: K ≈ +2.0 °C constant (per-level table
  above); gddr6 applies it behind a toggle define (`CORE_CALIB_OFFSET_C`).
- Wide BAR0 scan shown to be **dangerous** (GPU state corruption, nvidia-
  smi blackout, CUDA illegal access); scan mode on hold.
- nv-sensors-extra 1.2: millidegree resolution in all three channels
  (VRAM 1/32 °C, hotspot/core 1/256 °C); hotspot now has the same
  availability check (bit 29/30) as core; channel order changed to
  **temp1 = GPU core, temp2 = GPU hotspot, temp3 = memory junction**.
  **No calibration in the module** by design — the ~+2 °C offset is
  documented in the README, to be applied through the lm-sensors offset
  and measured per card.

TODO:
1. Cross-card confirmation on the air-cooled 3090 (60–80 °C): (a) the core
   sensor tracks NVML there too (0x20460 is the right register); (b) the
   +2.0 °C offset stays constant over the wide range (and no gain term
   appears). The measured offset goes into the user's lm-sensors config,
   not into the module. (Main open item.)
2. Find the 24 per-chip memory sensors (FB region / further NV_THERM
   instances) — narrow ranges only, after a reboot, no workloads.
3. Decide the status of r204BC/r204C4 (shadow of r20464 or separate
   sensor) and of r2047C (independent sensor vs delayed shadow).
4. Optional: add the per-chip memory sensors as temp4+ — experimental.
