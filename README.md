# RP2040 Kernel

A bare-metal dual-core kernel for the Raspberry Pi RP2040 with a pluggable CPU frequency governor system, interactive minishell, hardware benchmarking suite, and persistent configuration.

## Features

- **Dual-core architecture** — Core 0 runs the interactive REPL; Core 1 runs the non-blocking governor tick loop
- **Pluggable governor system** — `ondemand`, `schedutil`, `performance`, and the RP2040-optimized `rp2040_perf` governor
  - **rp2040_perf** — Aggressive metric-driven scaling with idle detection, hysteresis, and thermal awareness
  - **All governors** — Non-blocking single-step ramps, VREG pre-warming, intensity-based decision making
- **Safe frequency ramping** — Non-blocking `ramp_step()` with voltage-before-frequency sequencing, PLL-validity probing, and `multicore_lockout` guards
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

> **Note on 264 MHz:** The RP2040 PLL formula is `sys_clk = (12 MHz × fbdiv) / (postdiv1 × postdiv2)` with VCO constrained to [750, 1600] MHz. 265 MHz has no valid divisor combination; 264 MHz is the highest reachable frequency below 266 MHz and has five valid PLL configurations.

## Building

**Requirements**

- `pico-sdk` installed at `/opt/pico-sdk` (or set `PICO_SDK_PATH` in your environment)
- `cmake`, `make`, `arm-none-eabi-gcc`

```bash
git clone https://github.com/Night-Traders-Dev/RP2040_Kernel
cd RP2040_Kernel/src
./build.sh
```

The compiled `pico_gov.uf2` will be in `src/build/`. Hold BOOTSEL while plugging in the Pico, then copy the UF2 to the mass storage device.

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
bench <target> <ms>          Run a single benchmark for <ms> milliseconds
bench suite <ms> [csv]       Run full benchmark suite across all governors
clocks                       Dump PLL/clock divider frequencies
temp                         Read core temperature and vreg state
stats                        Toggle live clock/temp display
metrics                      Show aggregated app-submitted metrics
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
| `rp2040_perf` | RP2040-optimized governor. Pre-warms VREG, scales aggressively on app metrics, backs off on thermal excursion. Default. |
| `ondemand` | Ramps up on high intensity metrics or low temperature, backs off slowly above 65°C. |
| `schedutil` | Maps app-reported utilization linearly across the full frequency range. |
| `performance` | Always targets `MAX_KHZ`. No adaptation. |

### Tunable Parameters (`rp2040_perf`)

```
gov tune rp2040_perf set cooldown_ms        <ms>     Minimum time between target changes (default: 2000)
gov tune rp2040_perf set thr_high_intensity <0-100>  Intensity threshold for max frequency (default: 80)
gov tune rp2040_perf set thr_med_intensity  <0-100>  Intensity threshold for high step (default: 60)
gov tune rp2040_perf set thr_low_intensity  <0-100>  Intensity threshold for idle step (default: 20)
gov tune rp2040_perf set temp_backoff_C     <C>      Temperature to trigger thermal backoff (default: 72)
gov tune rp2040_perf set temp_restore_C     <C>      Temperature to restore max target (default: 65)
gov tune rp2040_perf set backoff_target_khz <khz>    Frequency to use during thermal backoff (default: 200000)
gov tune rp2040_perf set idle_target_khz    <khz>    Frequency to use during low-activity idle (default: 100000)
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

## Architecture

```
Core 0                          Core 1
──────────────────────────      ──────────────────────────────
stdio_init / USB enumeration    governors_init()
REPL loop (getchar_timeout)     governor->tick() loop
  └─ dispatch() → commands      │  └─ metrics_get_aggregate()
       ├─ freq / clock cmds     │  └─ ramp_to() if target != current
       ├─ governor cmds         │       └─ ramp_step() × N
       ├─ benchmark cmds        │            └─ multicore_lockout
       └─ dmesg / metrics       └─ metrics_publish_kernel()
Core 1 WDT monitor (5s)
```

**Frequency ramp safety:**
1. `find_achievable_khz()` probes candidates with `check_sys_clock_khz()` to skip non-PLL-achievable frequencies before touching hardware
2. Voltage is raised **before** a frequency increase and lowered **after** a frequency decrease
3. `multicore_lockout_start_blocking()` pauses Core 0 for the duration of each PLL reconfiguration step
4. If `set_sys_clock_khz()` fails despite a passing probe (silicon edge case), `target_khz` is clamped to `current_khz` and the ramp stops — `current_khz` is never updated on failure

## License

MIT — see [LICENSE](LICENSE).