# Glasnost Jig — Firmware Architecture

Scope note: this repo covers **firmware + host tooling only**. Hardware
(schematic/PCB, connector strategy, exact GPIO-to-signal wiring) is being
designed separately. Everything here treats pin assignments as a single
swappable config point, not a hardcoded fact.

## Target platform

- **Chip: ESP32-S3.** Not C3/C6/H2 — those only have the fixed-function
  "USB Serial/JTAG" peripheral (one hardwired CDC+JTOAG interface, no custom
  composite descriptors). The S3 has real USB-OTG (dwc2) silicon, required
  for a composite device with two independent CDC-ACM interfaces.
- **ESP-IDF v5.5** (current stable release line). Build via podman using
  `docker.io/espressif/idf:release-v5.5` — no local IDF install needed.
- **USB stack: `espressif/esp_tinyusb` v2.2.1** (managed component, pulled
  via `idf_component.yml`, NOT the older in-tree `tinyusb`/`tusb_cdc_acm.h`
  API — that header is deprecated as of v2.0.0 and forwards to
  `deprecated/tusb_cdc_acm.h`). Use the current header `tinyusb_cdc_acm.h`.

## Confirmed esp_tinyusb v2.2.1 API (do not deviate/guess)

```c
// tinyusb_types.h
typedef enum {
    TINYUSB_CDC_ACM_0 = 0x0,
    TINYUSB_CDC_ACM_1,
    TINYUSB_CDC_ACM_MAX
} tinyusb_cdcacm_itf_t;

// tinyusb_cdc_acm.h
typedef struct {
    cdcacm_event_type_t type;
    union {
        cdcacm_event_rx_wanted_char_data_t rx_wanted_char_data;
        cdcacm_event_line_state_changed_data_t line_state_changed_data;
        cdcacm_event_line_coding_changed_data_t line_coding_changed_data;
    };
} cdcacm_event_t;

typedef enum {
    CDC_EVENT_RX,
    CDC_EVENT_RX_WANTED_CHAR,
    CDC_EVENT_LINE_STATE_CHANGED,
    CDC_EVENT_LINE_CODING_CHANGED
} cdcacm_event_type_t;

typedef struct { bool dtr; bool rts; } cdcacm_event_line_state_changed_data_t;
typedef struct { cdc_line_coding_t const *p_line_coding; } cdcacm_event_line_coding_changed_data_t;
// cdc_line_coding_t is TinyUSB's own struct (class/cdc/cdc.h):
//   uint32_t bit_rate; uint8_t stop_bits; uint8_t parity; uint8_t data_bits;

typedef void (*tusb_cdcacm_callback_t)(int itf, cdcacm_event_t *event);

typedef struct {
    tinyusb_cdcacm_itf_t cdc_port;
    tusb_cdcacm_callback_t callback_rx;
    tusb_cdcacm_callback_t callback_rx_wanted_char;
    tusb_cdcacm_callback_t callback_line_state_changed;
    tusb_cdcacm_callback_t callback_line_coding_changed;
} tinyusb_config_cdcacm_t;

esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *cfg);
esp_err_t tinyusb_cdcacm_deinit(int itf);
esp_err_t tinyusb_cdcacm_register_callback(tinyusb_cdcacm_itf_t itf, cdcacm_event_type_t event_type, tusb_cdcacm_callback_t callback);
esp_err_t tinyusb_cdcacm_unregister_callback(tinyusb_cdcacm_itf_t itf, cdcacm_event_type_t event_type);
size_t tinyusb_cdcacm_write_queue_char(tinyusb_cdcacm_itf_t itf, char ch);
size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf, const uint8_t *in_buf, size_t in_size);
esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf, uint32_t timeout_ticks);
esp_err_t tinyusb_cdcacm_read(tinyusb_cdcacm_itf_t itf, uint8_t *out_buf, size_t out_buf_sz, size_t *rx_data_size);
bool tinyusb_cdcacm_initialized(tinyusb_cdcacm_itf_t itf);
```

Driver bring-up (from the official `tusb_serial_device` example pattern):

```c
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
```

`TINYUSB_DEFAULT_CONFIG()` leaves descriptors NULL, so the composite
descriptor is auto-generated from Kconfig — this is why `sdkconfig.defaults`
must set `CONFIG_TINYUSB_CDC_ENABLED=y` and `CONFIG_TINYUSB_CDC_COUNT=2`.

Relevant Kconfig knobs (`espressif/esp_tinyusb` Kconfig):
`TINYUSB_CDC_ENABLED`, `TINYUSB_CDC_COUNT` (1-2), `TINYUSB_CDC_RX_BUFSIZE`,
`TINYUSB_CDC_TX_BUFSIZE`, `TINYUSB_DESC_USE_ESPRESSIF_VID`/`TINYUSB_DESC_CUSTOM_VID`,
`TINYUSB_DESC_USE_DEFAULT_PID`/`TINYUSB_DESC_CUSTOM_PID`, `TINYUSB_DESC_MANUFACTURER_STRING`,
`TINYUSB_DESC_PRODUCT_STRING`, `TINYUSB_DESC_SERIAL_STRING`. There is only
one `TINYUSB_DESC_CDC_STRING` (shared across both CDC interfaces) — this is
*why* [[PROTOCOL.md]] discovery is done by probing with `PING`, not by
interface string.

## Module layout (`firmware/main/`)

- `board_config.h` — **the only file with pin numbers.** Every signal is a
  named macro; unwired signals are `-1` and every consumer must check for
  that sentinel and return `NOT_WIRED` rather than assume a pin exists.
  Also holds default DUT UART baud, buffer sizes, task stack/priority
  constants (named, not magic numbers).
- `usb_bridge.c/h` — brings up `tinyusb_driver_install` + both
  `tinyusb_cdcacm_init` calls. Owns `CDC_ACM_0` (DUT UART passthrough):
  `callback_rx` forwards bytes to the DUT UART TX; `callback_line_coding_changed`
  calls `uart_set_baudrate()` on the DUT UART from `p_line_coding->bit_rate`.
  A dedicated FreeRTOS task blocks on `uart_read_bytes(..., timeout)` for the
  DUT UART RX and forwards to `CDC_ACM_0` via `tinyusb_cdcacm_write_queue` +
  `tinyusb_cdcacm_write_flush`.
- `cmd_channel.c/h` — owns `CDC_ACM_1` (control). `callback_rx` reads into a
  bounded static line buffer, dispatches complete lines to `cmd_parser`, and
  writes the single response line back out.
- `cmd_parser.c/h` — tokenizes a command line and dispatches through a
  `static const` command table (`{name, handler}`), so adding `SPI`/`I2C`
  verbs later is a one-line table entry, not a rewrite. Implements the v1
  verb set from [[PROTOCOL.md]] exactly (including `NOT_WIRED`/
  `NOT_IMPLEMENTED`/`UNKNOWN_COMMAND`/`BAD_ARG`/`LINE_TOO_LONG` error codes).
- `power_ctl.c/h` — GPIO abstraction for the DUT power switch and the
  optional reset line. Boots with **power OFF by default** (fail-safe: a
  jig that resets or crashes must not leave a DUT unexpectedly powered);
  tracks logical on/off state in RAM (no persistence needed in v1).
- `main.c` — `app_main()`: init board_config-derived GPIOs and DUT UART,
  bring up `usb_bridge`, bring up `cmd_channel`, done (everything else is
  event/task driven, no polling loop in `app_main` itself).

DUT UART must be a **separate hardware UART from IDF's own console/log
UART** (e.g. `UART_NUM_1` for the DUT, leave `UART_NUM_0`/default console
for `ESP_LOG*` output) so firmware log spam never leaks into the DUT's
console stream.

## Embedded best practices to follow

- Every ESP-IDF init call that represents a startup invariant (peripheral
  install, GPIO config, tinyusb driver install) is wrapped in
  `ESP_ERROR_CHECK`. Anything that can legitimately fail at *runtime* from
  untrusted input (a malformed command line, an unwired GPIO request) is
  handled as a normal error return + protocol `ERR ...`, never
  `ESP_ERROR_CHECK`'d or asserted.
- No dynamic heap allocation in steady-state hot paths — line buffers and
  UART staging buffers are fixed-size `static`/stack buffers sized from
  named constants in `board_config.h`.
- No busy-wait/spin polling. RX paths block on queues or
  `uart_read_bytes(..., pdMS_TO_TICKS(N))` with a sane timeout; no task
  should starve the idle task (needed for the IDF idle-task watchdog).
- Tagged `ESP_LOGI/W/E` per module (`"usb_bridge"`, `"cmd_channel"`,
  `"power_ctl"`, ...).
- Firmware version is a single macro (`GLASNOST_FW_VERSION "0.1.0"`) plus
  `esp_get_idf_version()` for the IDF part of `VERSION`'s response.
- `idf_component.yml` pins `espressif/esp_tinyusb: "^2.2.1"` explicitly —
  don't leave it as `"*"`.

## Hardware-boundary notes (for the separate hardware design)

Found during firmware review — these can't be fully closed in firmware alone:

- **Power-switch line needs an external pull to its safe-off polarity.**
  `power_ctl_init()` drives the power-switch GPIO to off as the first thing
  `app_main()` does, but the pin is undriven (high-Z) for the ROM/2nd-stage
  bootloader window before that runs. If the switch-enable/gate line has no
  external pull resistor biased to safe-off, that window is a glitch risk.
  Add a pulldown (for an active-high switch, as currently configured) or
  pull-up (if the hardware ends up active-low) on that net.
- **USB power descriptor is bus-powered/100mA, not currently overridable**
  (esp_tinyusb's default composite descriptor hardcodes this). If DUT power
  ever ends up sourced from the same USB VBUS rather than a separate supply,
  this under-declares real current draw to the host. Only matters once the
  power architecture is decided; if it's an issue, the app will need a
  custom descriptor instead of `TINYUSB_DEFAULT_CONFIG()`.

## Build/flash/monitor via podman

No local ESP-IDF install. Wrapper scripts under `firmware/`:

```sh
podman run --rm -it \
  -v "$PWD/firmware:/project:Z" -w /project \
  docker.io/espressif/idf:release-v5.5 \
  idf.py set-target esp32s3 build
```

Flashing/monitor need the serial device passed through
(`--device=/dev/ttyUSB0` or wherever the board's native download UART/JTAG
shows up — that's a hardware/bring-up detail for later, the script should
accept the device path as a parameter, not hardcode it).
