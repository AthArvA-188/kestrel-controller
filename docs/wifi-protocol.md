# Kestrel WiFi protocol

Version 1. These are the UDP packets a Kestrel controller in WiFi mode exchanges with the receiver on the gaming PC.

Two implementations exist. The controller's is `firmware/components/kestrel_core/src/ks_wifi_proto.c`. The receiver's is `host/kestrel_receiver.py`. The firmware's native test writes one packet of each type to `firmware/sim/out/wifi_vectors.txt`, and the receiver's test decodes those bytes and re-encodes them. Nobody has run the protocol between a real controller and a PC.

## Transport

- UDP over IPv4. The receiver listens on port 47810.
- The controller uses one socket. Replies go to the address and port its packets came from.
- Multi-byte fields are little-endian.
- Each packet type has a fixed length. Both sides ignore bytes after the fields they know, so a later version can append fields.

## Header

Every packet starts with these 8 bytes.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | magic | ASCII `KS` |
| 2 | 1 | version | 1 |
| 3 | 1 | type | See the next table |
| 4 | 2 | seq | Sender's packet counter, wraps at 65535 |
| 6 | 2 | session | 0 in DISCOVER, otherwise the number from OFFER |

| Type | Name | Direction | Total length |
| --- | --- | --- | --- |
| 1 | DISCOVER | Controller to broadcast | 34 |
| 2 | OFFER | Receiver to controller | 28 |
| 3 | INPUT | Controller to receiver | 28 |
| 4 | FEEDBACK | Receiver to controller | 22 |
| 5 | BYE | Either way | 8 |

A packet with a different magic, version, or type, or one shorter than its type's length, is dropped.

## DISCOVER

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 8 | 6 | device_id | The controller's WiFi MAC address |
| 14 | 2 | fw_version | High byte major, low byte minor |
| 16 | 1 | battery_pct | 0 to 100 |
| 17 | 1 | flags | Bit 0 charging, bit 1 menu open |
| 18 | 16 | name | UTF-8, padded with zero bytes |

## OFFER

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 8 | 16 | name | The receiver's name, UTF-8, padded with zero bytes |
| 24 | 2 | rate_hz | INPUT packets per second the receiver wants |
| 26 | 1 | slot | Player number, from 1 |
| 27 | 1 | reserved | 0 |

The controller clamps `rate_hz` to the range 50 to 1000. The receiver's default is 250.

## INPUT

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 8 | 4 | buttons | One bit per output, see below |
| 12 | 2 | lx | Left stick, -32768 to 32767, positive is right |
| 14 | 2 | ly | Left stick, positive is up |
| 16 | 2 | rx | Right stick |
| 18 | 2 | ry | Right stick |
| 20 | 1 | lt | Left trigger, 0 to 255 |
| 21 | 1 | rt | Right trigger |
| 22 | 1 | battery_pct | 0 to 100 |
| 23 | 1 | flags | Bit 0 charging, bit 1 menu open |
| 24 | 4 | time_ms | Controller uptime when the packet was built |

The state is the one after the controller's remapping, dead zones, and response curves. While the on-screen menu is open the controller sends a neutral state and sets the menu flag.

Button bits:

| Bit | Output | Bit | Output |
| --- | --- | --- | --- |
| 0 | A | 10 | R3 |
| 1 | B | 11 | D-pad up |
| 2 | X | 12 | D-pad down |
| 3 | Y | 13 | D-pad left |
| 4 | LB | 14 | D-pad right |
| 5 | RB | 15 | LT pressed by a mapped button |
| 6 | View | 16 | RT pressed by a mapped button |
| 7 | Menu | 17 | Extra 1 |
| 8 | Home | 18 | Extra 2 |
| 9 | L3 | 19 | Extra 3 |

Bits 15 and 16 are set when a button mapped to a trigger is held. The `lt` and `rt` fields already read 255 in that case. An Xbox 360 pad has no place for bits 15 to 19, so the receiver drops them.

## FEEDBACK

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 8 | 1 | rumble_left | Heavy motor, 0 to 255 |
| 9 | 1 | rumble_right | Light motor, 0 to 255 |
| 10 | 1 | fx_valid | Bit 0: the left effect applies. Bit 1: the right effect applies |
| 11 | 1 | reserved | 0 |
| 12 | 4 | fx_left | Trigger effect, see below |
| 16 | 4 | fx_right | Trigger effect |
| 20 | 2 | ack_seq | `seq` of the last INPUT packet the receiver applied |

A trigger effect is four bytes: `effect`, `start`, `strength`, `param`.

| Effect | Name | start | strength | param |
| --- | --- | --- | --- | --- |
| 0 | Off | unused | unused | unused |
| 1 | Resistance | Trigger position where the arm meets the trigger | 255 does not give way | unused |
| 2 | Hair trigger | Position of the hard stop | unused | unused |
| 3 | Click | Position where resistance begins | Stiffness before the click | Travel to the click, 0 means 24 |
| 4 | Pulse | Position where pulsing begins | Sets the kick size | Pulses per second, 0 means 12 |

Positions run from 0 (released) to 255 (fully pulled). An effect whose `fx_valid` bit is set replaces the active profile's effect for that trigger until the session ends. With the bit clear the controller keeps its profile's effect. The controller drops a FEEDBACK packet whose effect number is above 4.

## BYE

Header only. It ends the session named in the header.

## Session flow

1. The controller sends DISCOVER every 500 ms to the subnet's broadcast address and to 255.255.255.255.
2. The receiver answers each DISCOVER with an OFFER to the packet's source. The first one for a controller creates a virtual pad and a random non-zero session number. Later ones repeat the same session, in case an OFFER was lost.
3. The controller takes the first OFFER it receives and sends INPUT to that address at the offered rate.
4. The receiver sends FEEDBACK whenever rumble or a trigger effect changes, and at least every 250 ms.
5. The controller accepts FEEDBACK and BYE only from the offer's address and port, with the offer's session number.
6. If the controller hears nothing valid for 2 seconds it stops both rumble motors, drops the receiver's trigger effects, and returns to step 1.
7. If the receiver gets no INPUT for 2 seconds it releases every control on the virtual pad, removes the pad, and sends BYE.

A controller that restarts sends DISCOVER from a new port. The receiver matches its `device_id`, closes the old session, and starts a new one in the same slot. The receiver serves up to four controllers, which is the XInput limit.

## Sequence numbers

`seq` in INPUT goes up by one for each packet. The receiver applies a packet only when it is newer than the last one applied, meaning the difference modulo 65536 is between 1 and 32767. That drops duplicates and packets that arrive out of order, and the gaps give a loss count.

`ack_seq` with `time_ms` would let a controller estimate the round trip. The current firmware does not use it.

## Timing

The controller samples its inputs every millisecond. At the default 250 Hz a change waits up to 4 ms for the next packet. WiFi adds its own delay on top, and nobody has measured it for this design.

## Security

Version 1 has no authentication and no encryption. On the network the PC and controller share, anyone can:

- read the inputs
- send DISCOVER and get a virtual pad on the PC, then send it inputs
- answer a controller's DISCOVER before the real receiver does, take its inputs, and drive its rumble and trigger motors within the firmware's current limit

Use WiFi mode on a network you trust. The receiver's `--allow` option limits it to listed controller IDs, and `--bind` limits it to one network interface. Neither stops an attacker who copies an ID from a DISCOVER packet. The session number guards against stray packets and is not a secret. A later version should pair the controller with the receiver once and authenticate every packet with a shared key.
