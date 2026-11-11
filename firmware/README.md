# Kestrel firmware

Firmware for the Kestrel controller: an ESP32-S3 pad with USB, Bluetooth LE, and WiFi modes, a 320 x 170 screen, on-device remapping, adaptive triggers, and 24 addressable LEDs. `docs/spec.md` has the product decisions and `shared/pinmap.json` has the pins.

## Status: untested on hardware

No Kestrel hardware exists yet, so nobody has flashed or run this firmware. What has run:

- The firmware compiles and links for the ESP32-S3.
- The hardware-independent code passes its unit tests on a PC.
- The screen UI renders to PNG files on a PC, and I looked at each one.
- The PC receiver passes its tests against a simulated controller.

Everything that touches a pin, a radio, or a host is unverified: the drivers, USB enumeration on any operating system, Bluetooth pairing, the WiFi link, the trigger actuators, and all timing. The Xbox-identity Bluetooth mode and iPhone and iPad support are unverified by design. Expect bring-up work in every driver.

## Layout

| Path | Contents |
| --- | --- |
| `components/kestrel_core/` | Hardware-independent C with no ESP-IDF includes |
| `main/` | ESP-IDF code: board layer, USB, Bluetooth, WiFi, application |
| `sim/` | Native build of the core: unit tests and screenshot renderer |
| `sim/out/` | Rendered screens and the WiFi packet vectors |
| `tools/` | Generators for the pin headers and the font |
| `pinmap_change_requests.md` | Requests and questions for the PCB designer |

Core modules, all in `components/kestrel_core/src/`:

| File | Does |
| --- | --- |
| `ks_input.c` | Decodes the shift register bytes and bumper pairs, debounces, calibrates sticks and triggers |
| `ks_profile.c` | Four profiles, the input-to-output mapper, stored formats with CRC, the storage interface |
| `ks_reports.c` | XInput, HID gamepad, and Xbox Bluetooth report builders, descriptors, and output report parsers |
| `ks_trigger.c` | Trigger effect state machine with current limiting |
| `ks_wifi_proto.c` | UDP packet encode and decode |
| `ks_periph.c` | LED zones and the 250 mA cap, fuel gauge conversions, charger state, idle timer |
| `ks_fb.c`, `ks_font_data.c` | RGB565 drawing and the generated font |
| `ks_ui.c` | Home screen, menu, remap flow, calibration screens |

Firmware modules in `main/`:

| File | Does |
| --- | --- |
| `board.c` | PERIPH_EN and MOTOR_NSLEEP sequencing, 74HC165 chain on SPI3, ADC1 continuous sampling, DRV8833 and rumble PWM, deep sleep |
| `board_periph.c` | ST7789 through `esp_lcd`, SK6812 chain through RMT, MAX17048 over I2C, NVS storage |
| `usb_dev.c` | TinyUSB descriptors, the XInput class driver, HID callbacks |
| `ble_hid.c` | NimBLE HID over GATT, device information, and battery services |
| `wifi_link.c` | Station mode, the setup access point and page, the UDP link |
| `app_main.c` | The 1 kHz input task, the UI task, and the glue |

`main/pins_gen.h` and `components/kestrel_core/include/ks_pinmap_gen.h` come from `shared/pinmap.json`. Run `tools/gen_pins.py` again whenever the pin map changes. The current headers include the amendment that put `BTN_LB_ALT` and `BTN_RB_ALT` on the two spare shift register inputs.

## Build

The build uses PlatformIO with the `espidf` framework: platform `espressif32` 6.10.0, which carries ESP-IDF 5.4.0, and the `espressif/tinyusb` component 0.21.0. These steps keep the toolchain and build tree off drive C. PowerShell:

```powershell
cd firmware
py -3.12 -m venv .venv
.venv\Scripts\python -m pip install platformio pillow
$env:PLATFORMIO_CORE_DIR = "D:\Tools\platformio"
$env:PLATFORMIO_BUILD_DIR = "D:\Tools\kestrel-build"
.venv\Scripts\python tools\gen_pins.py
.venv\Scripts\pio run
```

The first run installs 5.2 GB of tools under `PLATFORMIO_CORE_DIR`, including GCC 14.2.0 for Xtensa, and took 12 minutes here. Later builds take seconds. The binary is `D:\Tools\kestrel-build\kestrel\firmware.bin`.

`tools\gen_font.py` regenerates `ks_font_data.c` from Aileron Regular, the CC0 face that Pillow embeds. The generated file is in the tree, so you need the script only to change sizes.

The top-level `CMakeLists.txt` is a standard ESP-IDF project file, so `idf.py build` should also work from an ESP-IDF 5.4 shell. I did not try that.

## Flash

The board has no automatic reset circuit. To enter the ROM bootloader, hold BOOT, tap EN on the debug header, then release BOOT. The ESP32-S3 then appears as a USB serial device on the USB-C port.

```powershell
.venv\Scripts\pio run -t upload --upload-port COM7
```

Tap EN again to start the firmware. A USB-to-serial adapter on the debug header's UART0 pins also works, and that header carries the log output at 115200 baud.

On a dev kit with no charger chip, build with `-DKS_VBUS_FROM_PGOOD=0` or the USB modes never attach. With no shift registers fitted, the data line floats and buttons read at random.

## Native tests and screenshots

The core builds with any C11 compiler. With Visual Studio 2022 and CMake:

```powershell
cmake -S sim -B D:\Tools\kestrel-sim-build -G "Visual Studio 17 2022" -A x64
cmake --build D:\Tools\kestrel-sim-build --config Release
D:\Tools\kestrel-sim-build\Release\kestrel_tests.exe sim\out
D:\Tools\kestrel-sim-build\Release\kestrel_render.exe sim\out
```

`kestrel_tests` runs the unit tests and writes `sim/out/wifi_vectors.txt`. `kestrel_render` drives the real UI state machine with simulated button presses and writes 22 PNG files.

## Test results

Run on 2026-11-11 on Windows 11.

| Check | Result |
| --- | --- |
| Firmware build for `esp32s3` | Links. `firmware.bin` is 1,093,360 bytes, 34.7% of the 3 MB app partition. Static RAM 47,300 bytes. No compiler warnings in the 15 project sources |
| Native unit tests, MSVC with `/W4` | 49 tests, 49 passed, 2565 checks, no compiler warnings |
| UI screenshots | 22 rendered and inspected |
| Receiver tests (`host/`) | 23 tests, 23 passed |

The unit tests cover shift register decoding and bit order, the three possible bumper wirings in both contact orders, debounce, stick and trigger calibration, the calibration capture, mapping of all 20 inputs to all 21 outputs, profile storage and corruption handling, the Fn gesture, every report format, HID descriptor sizes against the report builders, each trigger effect against a model of the actuator, current limiting, WiFi packets, the LED current cap, fuel gauge conversions, the idle timer, and the UI flows.

Two test failures on the first run found real problems. The default profiles drew about 323 mA of LED current, so the cap was dimming them, and I lowered the default brightness. A pulse effect design chased its own motion, and I replaced it before it reached a test.

## Modes

The mode is chosen in Menu, Connection. The pad saves it and restarts. Holding Home with a button at power-on also sets it: A for USB XInput, B for USB DInput, X for Bluetooth Xbox, Y for Bluetooth HID, Menu for WiFi.

| Mode | What the host sees | Rumble | Trigger effects from the host |
| --- | --- | --- | --- |
| USB XInput | Xbox 360 wired protocol on a vendor interface, class FF/5D/01. A Microsoft OS 1.0 descriptor gives the compatible ID `XUSB10` | Yes | No. The profile sets them |
| USB DInput | HID gamepad: 16 buttons, hat, X Y Z Rz at 16 bits, Rx Ry at 8 bits | Output report 2: left, right | Output report 3 |
| Bluetooth Xbox | Xbox Wireless Controller (model 1914) report map and PnP ID 045E:0B13 | Xbox rumble report | No |
| Bluetooth HID | The same HID gamepad as USB DInput, over GATT | Output report 2 | Output report 3 |
| WiFi | UDP packets to `host/kestrel_receiver.py`. See `docs/wifi-protocol.md` | Yes | Yes |

Output report 3 is five bytes: target (1 left, 2 right, 3 both), effect (0 off, 1 resistance, 2 hair trigger, 3 click, 4 pulse), start, strength, param. It sits in its own vendor collection, usage page FF00.

Limits to know about:

- USB VID 1209:0001 is the pid.codes test ID and a placeholder. Both USB modes use it. They differ in `bcdDevice` and serial number so that Windows binds drivers separately, which is untested. A product needs its own IDs.
- The Xbox Bluetooth report map is reproduced from published dumps and has not been compared with a capture from a real controller. Whether Windows, Android, or iOS accept it from this device is unknown. The identity uses Microsoft's vendor ID, which is acceptable for a personal prototype only.
- iPhone and iPad support depends on that mode and is unproven. Generic HID gamepads pair with iOS but games do not see them.
- Bluetooth shows the host's address, since a peripheral does not learn the host's name.
- WiFi has no authentication or encryption. The setup access point is the exception: it uses WPA2 with a random password that the pad shows on its screen.

## Controls on the pad

Fn opens the menu. The D-pad moves, A selects, B goes back, and Fn closes it. While the menu is open the host receives a neutral report.

Remapping takes three steps: choose Remap, press the control to change, then press the button whose function it should take or pick an output from the list with the D-pad and A. Any of the 20 inputs can be the source, including Fn and the paddles. If Fn is given an output, it sends that output and opens the menu after a one second hold instead. Step 2 returns to the menu after 8 seconds with no press.

Each of the four profiles holds the mapping, stick dead zones and curves, trigger dead zone, one effect per trigger, and a colour and brightness for each of five LED zones. Changes save to NVS 1.5 seconds after the last adjustment.

## Calibration

Menu, Sticks, Calibrate records the stick centres, stick ranges, trigger rest and full-pull values, and the released state of both bumper pairs. Until it has run, the firmware uses placeholder analogue ranges and takes the bumper levels at power-on as released. Run it once on every new board.

Each bumper drives two inputs because the switch's common terminal is unknown. The firmware treats a bumper as pressed when its pair of levels differs from the released pair.

## Trigger effects

Nothing senses the stop arm. The controller estimates its position from the commanded duty and finds the park end stop by watching the motor current, at start-up and after any fault. `ks_trig_params_default` holds the travel time and current thresholds, which are guesses for an N20 motor and need tuning on the real mechanism.

- Off parks the arm clear of the trigger.
- Hair trigger holds the arm at the start point. The host sees a full pull when the trigger reaches it.
- Resistance lets the arm give way while the trigger presses on it, slower for a higher strength. At 255 it does not give way.
- Click resists until the trigger passes the click point, then drops the arm away until the trigger is released.
- Pulse kicks the trigger back and retreats, at the set rate, while the trigger is past the start point.

Motor duty folds back above 600 mA. The motor stops for half a second after 40 ms above 900 mA or on a DRV8833 fault, then homes again.

## Power

The pad sleeps after the idle timeout in Menu, System (10 minutes by default) unless a USB host is attached. Sleep stops the motors, drives MOTOR_NSLEEP and PERIPH_EN low and holds them, and enters deep sleep. Home wakes it through ext1. GPIO45 and GPIO46 are strapping pins, so the firmware drives them as plain outputs and never enables a pull-up on them.

LED output is scaled so the estimated chain current stays at or under 250 mA. The estimate uses 12 mA per colour channel and 1 mA idle per LED.

## Not done

- Nothing is tuned or verified on hardware.
- No way to flip a stick axis from the menu. The `invert` flags exist in the calibration data and default to off.
- Trigger position uses a two-point calibration. The Hall sensor's response to a swinging magnet is not linear, so the reading will need a correction curve.
- The arm estimate drifts between homing runs. Periodic homing while an effect is off would fix that.
- Lighting is static colour per zone. No animations and no low-battery signal.
- The MAX17048 alert pin is read by nothing. There is no low-battery shutdown beyond the cell's protection board.
- Bluetooth keeps one host at a time and ignores the duration fields of Xbox rumble commands.
- No OTA updates, motion sensors, or audio. Those were out of scope.
