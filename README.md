# RP2040 Kernel

A bare-metal dual-core kernel for the Raspberry Pi RP2040 with a pluggable CPU frequency governor system, interactive minishell, hardware benchmarking suite, PIO-based idle measurement, and persistent configuration.

## Features

- **Dual-core architecture** — Core 0 runs the interactive REPL; Core 1 runs the non-blocking governor tick loop
- **PIO idle & jitter subsystem** — Two autonomous PIO state machines (PIO0, SM0+SM1) provide hardware-accurate measurements with zero CPU overhead:
  - **SM0 `idle_measure`** — measures real CPU idle time by timing how long Core 0 spends in its `getchar` spin-wait; result is an EMA-smoothed idle fraction
  - **SM1 `period_measure`** — measures the period between Core 0 heartbeat pulses; detects PLL transition jitter by comparing consecutive readings with a rolling CV window
  - **Governor safety gate** — `pio_idle_safe_to_scale()` blocks frequency steps until heartbeat period CV is below 1.5% for 4+ consecutive readings; prevents mid-ramp scaling anomalies
  - **Settle window** — after each PLL step, jitter assessment is suppressed for 8 poll cycles (~8 ms) while the PLL locks; stale pre-transition readings are discarded
- **Pluggable governor system** — `ondemand`, `schedutil`, `performance`, and the RP2040-optimized `rp2040_perf` governor
  - **rp2040_perf** — Aggressive metric-driven scaling with idle detection, PIO-gated frequency steps, hysteresis, and thermal awareness
  - **All governors** — Non-blocking single-step ramps, VREG pre-warming, intensity-based decision making
- **Safe frequency ramping** — Non-blocking `ramp_step()` with voltage-before-frequency sequencing, PLL-validity probing, `multicore_lockout` guards, and automatic PIO baseline reset on every successful step
  - Responsive: single 5 MHz step per governor tick (~40 ms) allows concurrent app execution
  - Thermal-aware: global throttle cap applies when core temp > 70°C; restores at < 65°C
- **Runtime governor tuning** — Adjust governor parameters at runtime via CLI; changes persist across reboots
- **Metrics subsystem** — Apps submit workload/intensity samples (cleared each tick); governors consume aggregated stats for frequency decisions
- **Comprehensive benchmarking suite**
  - Workloads: `cpu`, `memcpy`, `memset`, `mem_stream`, `rand_access`, `mem_stream_dma`
  - Dynamic intensity: measures real throughput and submits realistic workload intensity every ~100 ms
  - Live telemetry: frequency (MHz) and temperature (°C) logged throughout execution
  - CSV output: runnable across all governors with structured results
  - Non-blocking: live stats update every 500 ms during benchmark run (synchronized with main loop)
- **`dmesg` ring buffer** — 64-entry timestamped kernel log with optional UART drain; reduced noise via state-change logging
- **Core 1 watchdog** — Core 0 monitors Core 1's heartbeat counter every 5 seconds and reboots on stall
- **MMIO peek/poke** — Safe address-validated 32-bit register read/write from the shell
- **Persistent storage** — Governor selection and tunable parameters survive reboot via flash

## Hardware

| Item | Value |
|---|---|
| Target board | Raspberry Pi Pico (RP2040) |
| Clock range | 125 – 264 MHz |
| Max tested | 264 MHz @ 1.30V |
| SDK | [pico-sdk](https://github.com/raspberrypi/pico-sdk) |
| PIO idle pin | GPIO 20 (Core 0 drives HIGH during `getchar` spin) |
| PIO heartbeat pin | GPIO 21 (Core 0 emits brief pulse each main-loop tick) |

> **Note on 264 MHz:** The RP2040 PLL formula is `sys_clk = (12 MHz × fbdiv) / (postdiv1 × postdiv2)` with VCO constrained to [750, 1600] MHz. 265 MHz has no valid divisor combination; 264 MHz is the highest reachable frequency below 266 MHz and has five valid PLL configurations.

> **Note on GPIO 20/21:** These pins are consumed by the PIO subsystem at boot. If your hardware uses them, redefine `PIO_IDLE_PIN` and `PIO_HB_PIN` in `pio_idle.h` before building.

## Building

**Requirements**

- `pico-sdk` installed at `/opt/pico-sdk` (or set `PICO_SDK_PATH` in your environment)
- `cmake`, `make`, `arm-none-eabi-gcc`

```bash
git clone https://github.com/Night-Traders-Dev/RP2040_Kernel
cd RP2040_Kernel/src
./build.sh
```

The compiled `pico_minishell.uf2` will be in `src/build/`. Hold BOOTSEL while plugging in the Pico, then copy the UF2 to the mass storage device.

## Shell Commands

Connect via USB serial at 115200 baud (e.g. `sudo microcom -p /dev/ttyACM0`).

```
set <mhz>                    Set target frequency (125–264 MHz)
gov list                     List all governors
gov set <name>               Switch active governor
gov status                   Show current governor
gov tune rp2040_perf show    Print all tunable parameters
gov tune rp2040_perf set <param> <value>
gov tune rp2040_perf get <param>
gov tune rp2040_perf list    List available tunable parameters
bench <target> <ms>          Run a single benchmark for <ms> milliseconds
bench suite <ms> [csv]       Run full benchmark suite across all governors
pio                          Show PIO idle fraction, heartbeat jitter, and scaling readiness
clocks                       Dump PLL/clock divider frequencies
temp                         Read core temperature and vreg state
stats                        Toggle live clock/temp display
metrics                      Show aggregated app-submitted metrics
persist                      Show persisted governor and rp_params status
peek <hex_addr>              Read 32-bit MMIO register
poke <hex_addr> <hex_val>    Write 32-bit value to MMIO register
flash                        Show flash size and firmware usage
uptime                       Show system uptime
dmesg                        Print kernel log
dmesg uart <on|off>          Enable/disable UART log drain
reboot                       Restart system
bootsel                      Reboot into UF2 flash mode
clear                        Clear the screen
help                         Show command list
```

## Governors

| Governor | Description |
|---|---|
| `rp2040_perf` | RP2040-optimized governor. Pre-warms VREG, scales aggressively on app metrics, gates frequency steps on PIO stability, backs off on thermal excursion. Default. |
| `ondemand` | Ramps up on high intensity metrics or low temperature, backs off slowly above 65°C. |
| `schedutil` | Maps app-reported utilization linearly across the full frequency range. |
| `performance` | Always targets `MAX_KHZ`. No adaptation. |

### Tunable Parameters (`rp2040_perf`)

```
gov tune rp2040_perf set cooldown_ms          <ms>     Minimum time between target changes (default: 2000)
gov tune rp2040_perf set ramp_up_cooldown_ms  <ms>     Faster cooldown for scale-up transitions (default: 500)
gov tune rp2040_perf set thr_high_intensity   <0-100>  Intensity threshold for max frequency (default: 80)
gov tune rp2040_perf set thr_med_intensity    <0-100>  Intensity threshold for high step (default: 60)
gov tune rp2040_perf set thr_low_intensity    <0-100>  Intensity threshold for idle step (default: 20)
gov tune rp2040_perf set dur_high_ms          <ms>     Duration required at high intensity (default: 500)
gov tune rp2040_perf set dur_med_ms           <ms>     Duration required at medium intensity (default: 250)
gov tune rp2040_perf set dur_short_ms         <ms>     Duration threshold for idle classification (default: 200)
gov tune rp2040_perf set temp_backoff_C       <C>      Temperature to trigger thermal backoff (default: 72)
gov tune rp2040_perf set temp_restore_C       <C>      Temperature to restore max target (default: 65)
gov tune rp2040_perf set backoff_target_khz   <khz>    Frequency to use during thermal backoff (default: 200000)
gov tune rp2040_perf set idle_target_khz      <khz>    Frequency to use during low-activity idle (default: 100000)
gov tune rp2040_perf set idle_timeout_ms      <ms>     Sustained inactivity before entering idle (default: 5000)
```

All changes persist across reboots.

## Metrics API

Application code can feed workload data to the governor by calling:

```c
#include "metrics.h"

// workload: arbitrary units (e.g. queue depth, bytes processed)
// intensity: 0–100, percentage-style utilization estimate
// duration_ms: how long the work took
metrics_submit(workload, intensity, duration_ms);
```

Governors receive a rolling aggregate of these samples on every tick and use them to make frequency scaling decisions.

## PIO Subsystem

The PIO subsystem runs entirely in hardware on PIO0 and requires no CPU cycles for timing. It provides two independently useful signals to the governor layer:

**Idle fraction (SM0):** Core 0 drives `PIO_IDLE_PIN` HIGH during its `getchar_timeout_us(0)` spin and LOW as soon as a character arrives or a timeout occurs. SM0 counts sys-clock cycles for each HIGH window and pushes a 32-bit tick count to its RX FIFO on the falling edge. `pio_idle_poll()` drains the FIFO each main-loop iteration and maintains an EMA of `idle_us / loop_period_us`.

**Heartbeat period / jitter (SM1):** Core 0 emits a brief (≥8 NOP) HIGH pulse on `PIO_HB_PIN` once per main-loop iteration. SM1 measures the LOW phase between consecutive pulses — effectively the full loop period — and pushes it to its RX FIFO. `pio_idle_poll()` computes the signed delta between consecutive readings (`hb_jitter_pct`) and maintains a rolling 8-sample coefficient-of-variation window to declare the clock "stable".

**Scaling safety gate:** `rp2040_perf` calls `pio_idle_safe_to_scale(0.03, 3.0, 4)` before applying any new frequency target. A frequency step is deferred (with a rate-limited dmesg log) until the heartbeat CV drops below 1.5% for at least 4 consecutive readings and the most recent jitter is within 3%. After each successful `ramp_step()`, `pio_idle_notify_freq_change()` resets the window and starts an 8-poll settle period.

```
pio                          # inspect live PIO stats at the shell
```

```
PIO Idle Monitor:
  IDLE_PIN          : GPIO 20
  HB_PIN            : GPIO 21
  idle_ticks        : 74821
  idle_fraction     : 92.3 %
  hb_period_ticks   : 79104  (0.60 us @ 264 MHz)
  hb_jitter_ticks   : +12
  hb_jitter_pct     : 0.02 %
  stable_count      : 47
  safe_to_scale     : YES
```

## Architecture

```
Core 0                          Core 1
──────────────────────────      ──────────────────────────────
stdio_init / USB enumeration    governors_init()
pio_idle_init()                 governor->tick() loop
REPL loop                         └─ metrics_get_aggregate()
  pio_idle_heartbeat()              └─ pio_idle_safe_to_scale()
  pio_idle_poll()                   └─ ramp_step() if target != current
  pio_idle_enter()                       └─ multicore_lockout
  getchar_timeout_us(0)                  └─ set_sys_clock_khz()
  pio_idle_exit()                        └─ pio_idle_notify_freq_change()
  dispatch() → commands           └─ metrics_publish_kernel()
Core 1 WDT monitor (5s)

PIO0 (hardware, no CPU)
  SM0  idle_measure   ← GPIO 20 (IDLE_PIN driven by Core 0)
  SM1  period_measure ← GPIO 21 (HB_PIN driven by Core 0)
  Both push 32-bit tick counts to RX FIFOs → drained by pio_idle_poll()
```

**Frequency ramp safety:**
1. `pio_idle_safe_to_scale()` checks that the heartbeat period has been stable (CV < 1.5%) for at least 4 consecutive readings before the governor is permitted to change `target_khz`
2. `find_achievable_khz()` probes candidates with `check_sys_clock_khz()` to skip non-PLL-achievable frequencies before touching hardware
3. Voltage is raised **before** a frequency increase and lowered **after** a frequency decrease
4. `multicore_lockout_start_blocking()` pauses Core 0 for the duration of each PLL reconfiguration step
5. If `set_sys_clock_khz()` fails despite a passing probe (silicon edge case), `target_khz` is clamped to `current_khz` and the ramp stops — `current_khz` is never updated on failure
6. `pio_idle_notify_freq_change()` is called on every successful step, clearing the jitter window and starting a new settle period

## License

MIT — see [LICENSE](LICENSE).