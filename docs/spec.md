# Kestrel controller specification

Kestrel is a game controller for PC, Android, iPhone, and iPad. This document records the design decisions. Coordinates live in `shared/layout.json` and pin assignments in `shared/pinmap.json`. Those two files win if this document disagrees with them.

Status: first design pass. Nobody has built or tested this hardware.

## Requirements

| Requirement | Design answer |
| --- | --- |
| USB-C and Bluetooth | ESP32-S3 module with native USB and Bluetooth LE 5 |
| Remote play over WiFi | WiFi mode sends inputs straight to a receiver on the gaming PC |
| Xbox button layout | Offset sticks, ABXY, D-pad, View, Menu, Home, LB, RB, LT, RT, L3, R3 |
| TMR sticks | Two TMR modules in the ALPS RKJXV footprint |
| Tension ring under each stick | Rotating ring that preloads an auxiliary centring spring |
| Adaptive triggers | Motor, worm, and stop arm per trigger, as in the DualSense |
| Tactile LB and RB | D2FC-type micro switches |
| Four back paddles | Two per grip on micro switches, remappable |
| Backlit buttons and stick rings | 24 addressable RGB LEDs under translucent parts |
| Screen | 1.9 inch 320 x 170 IPS module for status and on-device remapping |
| PC, Android, iOS, iPadOS | XInput and DInput over USB, HID over Bluetooth LE |
| Wider grip | 172 mm overall width with fuller grips |

## Controls

Kestrel has 20 digital inputs, two sticks, and two analogue triggers.

- Face: A, B, X, Y, D-pad up, down, left, and right, View, Menu, Home, Fn
- Shoulders: LB, RB, LT, RT
- Sticks: L3, R3
- Back: paddles P1 to P4

Fn opens the on-screen menu. While the menu is open the controller sends a neutral report to the host, the D-pad moves the cursor, A selects, and B goes back.

## Electronics

### Block summary

| Block | Part | Notes |
| --- | --- | --- |
| MCU and radio | ESP32-S3-WROOM-1-N16R2 | USB full speed, Bluetooth LE 5, WiFi 4 at 2.4 GHz |
| Sticks | TMR module, ALPS RKJXV footprint | Analogue outputs to ADC1 |
| Trigger position | DRV5055A4 linear Hall sensor | One per trigger on the PCB bottom, on the switched 3.3 V rail |
| Trigger motors | DRV8833 dual H-bridge | 0.2 ohm sense resistors, current readback on ADC1 |
| Buttons | 3 x 74HC165 on SPI3 | 24 inputs read in one transfer |
| LEDs | 24 x SK6812MINI | One data line through an SN74LVC1T45 level shifter |
| Display | Waveshare 1.9inch LCD Module (ST7789) on SPI2 | 8-pin cable, backlight on PWM |
| Rumble | 2 x ERM motor | Low-side N-FET with flyback diode, supplied from the LED rail |
| Charger | BQ24074 | Power path, so the pad runs while charging. Input limit fixed at 500 mA |
| Battery protection | AO3401A P-FET | Blocks a battery plugged in backwards |
| Fuel gauge | MAX17048 | Battery percentage over I2C |
| 3.3 V rail | TPS63001 buck-boost | Holds 3.3 V across the full battery range |
| Analogue rail | TLV70030 3.0 V LDO | Clean supply for the sticks |
| Battery | 1S Li-Po, 3000 mAh | JST-PH lead |

### Why these choices

The ESP32-S3 is the only common pre-certified module with USB device, Bluetooth LE, and WiFi in one part. It has no Bluetooth Classic, so every Bluetooth mode uses HID over GATT.

The internal ADC reads the sticks. The sticks run from a 3.0 V rail so their output stays inside the ADC's linear range. Firmware oversamples and calibrates each axis.

Shift registers read the buttons because the module has too few free pins for 20 direct inputs. One 24-bit SPI transfer takes a few microseconds, so scanning at 1 kHz adds no measurable latency.

Home connects directly to GPIO15 because that pin can wake the MCU from deep sleep.

The display is a Waveshare module on a cable, not a bare panel. Bare 1.9 inch panels ship with several different flex pinouts, and a wrong guess would mean a new PCB. The module's 8-pin connector is documented.

Each bumper micro switch has all three pads wired: one to ground and two to inputs. The common terminal of D2FC-type switches was not confirmed during design, and this wiring works for every arrangement. Firmware learns the released state at calibration.

TMR stick modules need a supply on a specific outer pin of each axis, and vendors differ. Solder jumpers JP1 to JP4 set which pin gets 3.0 V and which gets ground.

### PCB

The main board is 134 x 82 mm with four copper layers. It is autorouted and passes KiCad's rule checks with no errors and nothing unconnected. The autorouter ran signal tracks through both inner layers, so neither is a solid plane. `hardware/README.md` describes how the rails are joined and what a layout review should change.

### Power

The BQ24074 feeds VSYS from USB when present and from the battery otherwise. Firmware puts the MCU into deep sleep and drives PERIPH_EN low to switch off the display, LEDs, sticks, and Hall sensors. Home wakes it.

Estimated draw in Bluetooth mode with dim lighting is about 170 mA, which gives about 17 hours from 3000 mAh. WiFi mode draws more. These figures are estimates from datasheet typicals and need measuring.

The charger takes at most 500 mA from USB, which any port can supply. A full charge from empty should take about seven hours with the pad asleep. Raising the limit needs USB-C current detection and is left for a later revision.

## Host compatibility

| Host | USB-C | Bluetooth LE | Status |
| --- | --- | --- | --- |
| Windows PC | XInput or DInput | Xbox-identity HID or generic HID | Expected to work, untested |
| Android phone or tablet | DInput | Xbox-identity HID or generic HID | Expected to work, untested |
| iPhone or iPad | DInput | Xbox-identity HID | Unverified, see below |
| Linux and Steam Deck | DInput | Generic HID | Expected to work, untested |

### Modes

- **USB XInput.** Xbox 360 wired protocol. A Microsoft OS descriptor with the `XUSB10` compatible ID makes Windows load its XInput driver.
- **USB DInput.** Standard HID gamepad with 16 buttons, a hat switch, and six axes.
- **Bluetooth Xbox identity.** Presents the same HID report layout as an Xbox Wireless Controller. Windows exposes it through XInput.
- **Bluetooth generic.** Standard HID gamepad over GATT.
- **WiFi.** UDP link to the Kestrel receiver on a PC.

The user picks the mode on the screen. Holding Home plus a face button at power-on also selects it.

### iOS and iPadOS risk

Apple's Game Controller framework accepts a list of known controllers and MFi-licensed devices. Developers on Apple's forums report that generic Bluetooth HID gamepads pair but never reach games. The plan is to present the Xbox controller's Bluetooth identity, which iOS already supports. Nobody has confirmed that this works from an ESP32, so treat iPhone and iPad support as unproven until a prototype is tested. A commercial product would join Apple's MFi programme.

### Identity and licensing

The Xbox-identity Bluetooth mode uses Microsoft's vendor ID and report format. That is fine for a personal prototype and is not cleared for a product on sale. The USB modes use the pid.codes test ID `1209:0001` as a placeholder. A product needs its own vendor ID.

## WiFi remote play

Remote play apps run the game on a PC and stream video to another screen. In WiFi mode Kestrel joins the home network and sends its inputs directly to the gaming PC. A small receiver program on that PC presents a virtual Xbox 360 pad through ViGEmBus, so every game and streaming host sees a normal controller. The input path skips the phone or tablet that shows the video.

- Setup: the controller starts a WPA2 access point, shows its password on the screen, and serves a page where the user enters the WiFi name and password.
- Discovery: the controller broadcasts on UDP and the receiver answers.
- Link: input packets at 250 Hz or more, with rumble and trigger effects returned on the same socket.

For phone-hosted remote play apps, pair Kestrel to the phone over Bluetooth as usual.

## Adaptive triggers

Each trigger has a Hall sensor for position and a motor-driven stop arm. The worm gear is self-locking, so the arm resists the finger without holding current. Firmware moves the arm in response to trigger position.

| Effect | Behaviour |
| --- | --- |
| Off | Arm parked clear of the trigger |
| Resistance | Arm meets the trigger at a set point and holds |
| Hair trigger | Arm stops the trigger after a short pull |
| Click | Arm holds until the pull passes a threshold, then drops away |
| Pulse | Arm oscillates against the trigger at a set rate |

XInput has no trigger effect command, so effects come from the active profile, from a vendor HID output report, or from the WiFi receiver.

## Stick tension

A POM disc on the stick stem sits under a spring-loaded plunger. Tilting the stick lifts the plunger with the edge of the disc. Turning the ring 300 degrees moves the spring seat 2 mm and changes the preload. The plan is a three-start ramp with detents. The CAD does not model the ramp yet.

- Added force at the cap: about 10 to 46 gf, calculated from the geometry
- Module baseline: about 55 gf
- Total: about 65 to 100 gf

The spring also presses the stem downward, which lowers the force needed to click L3 or R3. The spring force is 1.2 N at rest on the tightest setting and rises to about 3.3 N at full tilt. A translucent ring around the turret base is lit by six LEDs.

## Screen

The home screen shows the connected host, the mode, the battery percentage, and the active profile. The menu covers connection, remapping, triggers, sticks, lighting, and system settings.

Remapping takes three steps on the pad. Choose Remap, press the physical control, then press or pick the output. Profiles store mappings, dead zones, trigger effects, and lighting in flash.

## Open items

1. Confirm the footprint and height of the chosen TMR module against its drawing. The PCB uses the ALPS RKJXV1224005 footprint.
2. Find which outer pin of each stick axis is the supply on the chosen TMR module and set JP1 to JP4 before first power-up. The default puts 3.0 V on pads 3 and 4.
3. Check the display pocket and cable route against the Waveshare module drawing.
4. Run bumper calibration at bring-up so firmware learns the micro switch polarity.
5. Test iPhone and iPad recognition of the Xbox-identity Bluetooth mode.
6. Tune the trigger actuator gear ratio and arm geometry on a prototype.
7. Measure stick tension range and L3/R3 click force at maximum preload.
8. Measure battery life and charge time.
9. Regulatory testing (FCC, CE) applies to any product on sale. The module is pre-certified, but the finished product still needs checks.
