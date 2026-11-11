# Pin map change requests

From the firmware work, for the PCB designer. Nothing here blocked the firmware, and it builds against `shared/pinmap.json` as it stands with no overrides.

## 1. LED_DATA needs a level shifter

GPIO16 drives the first SK6812 at 3.3 V logic. The LEDs run from VLED, which follows VSYS and reaches 4.4 V on USB power. SK6812 datasheets give the input high threshold as about 0.7 x VDD, which is 3.08 V at 4.4 V. The ESP32-S3 guarantees an output high of only 0.8 x 3.3 V = 2.64 V under load, and about 3.2 V unloaded. The chain may work on battery and glitch or stay dark while charging.

Proposed change: put a single-gate buffer with TTL-level input, powered from VLED, between the 330 ohm resistor and the first LED. A 74AHCT1G125 fits. Check the threshold in the datasheet of the exact LED you buy first.

Firmware impact: none.

## 2. Put the shift register pull-ups on 3V3_PERIPH

The amended bumper wiring ties pad 1 of each micro switch to GND. If a switch's normally closed contact joins GND to one of its two inputs, that input's 10k pull-up conducts 0.33 mA whenever the bumper is released. Two bumpers make 0.66 mA, all the time.

If the 24 pull-ups hang from the always-on 3V3 rail, that current also flows in deep sleep and sets the standby time: about six months from 3000 mAh, before the regulator's own draw.

Proposed change: feed the 74HC165s and their pull-ups from 3V3_PERIPH, so they are off in sleep. The firmware already drives SR_LOAD and SR_CLK only while PERIPH_EN is high. BTN_HOME keeps its pull-up on 3V3 because it wakes the MCU.

Firmware impact: none.

## 3. Questions, no change requested

- USB cable detection. The firmware uses PGOOD_N from the shift register to decide when to attach to the bus, since the pad powers itself. That works as long as PGOOD_N is valid within a few milliseconds of plugging in. A dev kit without the charger needs the build flag `-DKS_VBUS_FROM_PGOOD=0`.
- Motor current. The DRV8833 sense resistor carries current only while the bridge drives, so the filtered ISEN voltage is the motor current times the PWM duty. The firmware divides by the duty. The 1k and 100n filter is fine for the 20 kHz PWM it uses.
- Trigger Hall sensors on 3V3_PERIPH. A DRV5055 on 3.3 V can swing its output up to about 3.1 V, which is the top of the ESP32-S3 ADC's usable range at 12 dB. Size and place the magnet so the output stays between about 0.2 V and 2.9 V over the whole trigger travel. The firmware calibrates the two end values, so either polarity works.
- Rumble on VLED. The pin map now feeds both rumble motors from the VLED switch. The firmware caps the LEDs at 250 mA but does not budget rumble current against them, so size that switch for the LEDs plus both motors.
- Trigger motor direction. A positive command must move the stop arm toward the released end of the trigger travel. If a motor is wired the other way, set `MOTOR_INVERT_L` or `MOTOR_INVERT_R` in `main/board.c`.
- Display orientation. `LCD_MIRROR_X` and `LCD_MIRROR_Y` in `main/board_periph.c` set which way up the picture is. The right values depend on how the module sits in the shell.
