# Kestrel receiver

`kestrel_receiver.py` is the PC side of Kestrel's WiFi mode. It listens for controllers on UDP port 47810 and gives each one a virtual Xbox 360 pad through ViGEmBus, so games and streaming hosts see an ordinary controller. Rumble from a game goes back to the controller, along with any trigger effects you set on the command line.

Nobody has run it with a real Kestrel controller or with ViGEmBus. The tests below cover the packet handling against a simulated controller.

## Requirements

- Windows 10 or 11
- Python 3.10 or later. The tests ran on 3.12.6.
- For the virtual pad: the [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver and the `vgamepad` package. Installing the driver needs administrator rights. Check the driver's project page for its current status before you rely on it.

The script imports `vgamepad` only when it creates a virtual pad. Everything else, including the tests and the `log` sink, runs on the standard library.

## Run

```powershell
py -3.12 -m pip install vgamepad
py -3.12 kestrel_receiver.py
```

Put the controller in WiFi mode. Its screen shows the PC's name once the two have found each other. If Windows Firewall asks, allow Python on private networks. Discovery uses broadcast, so the PC and the controller must be on the same subnet.

To check the link without ViGEmBus, print the inputs instead:

```powershell
py -3.12 kestrel_receiver.py --sink log
```

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--sink vigem` or `--sink log` | `vigem` | Virtual Xbox 360 pad, or print each state change |
| `--rate HZ` | 250 | Input packets per second to ask for, 50 to 1000 |
| `--bind ADDRESS` | all interfaces | Listen on one address only |
| `--port N` | 47810 | UDP port. The firmware broadcasts to 47810 |
| `--allow DEVICE_ID` | any controller | Accept only this controller. Repeat for more |
| `--left-trigger EFFECT`, `--right-trigger EFFECT` | none | Trigger effect to send to every controller |
| `-v` | off | Debug logging |

A controller's ID is its WiFi MAC address as 12 hex digits. The receiver logs it when the controller connects.

An effect is `name[:start[:strength[:param]]]` with values from 0 to 255. The names are `off`, `resistance`, `hair`, `click`, and `pulse`. `start` defaults to 60 and `strength` to 160. For example:

```powershell
py -3.12 kestrel_receiver.py --right-trigger hair:70 --left-trigger resistance:40:200
```

Without these options each trigger keeps the effect from the controller's active profile. `docs/wifi-protocol.md` explains what the values mean for each effect.

## Tests

```powershell
cd host
py -3.12 -m unittest -v test_receiver
```

The tests start the receiver on a loopback port, drive it with a simulated controller, and replace the virtual pad with a fake that records what it receives. They cover discovery, input delivery, dropped and reordered packets, rumble and trigger effects on the return path, timeouts, reconnection, the four-pad limit, and the allow list. One test decodes the packets in `firmware/sim/out/wifi_vectors.txt`, which the firmware's C encoder writes, and is skipped until you run the firmware's native tests.

Last run: 23 tests, 23 passed, with `vgamepad` not installed.

## Limits

- The ViGEmBus path (`ViGEmSink`) has never run. It follows the `vgamepad` documentation.
- The protocol has no authentication or encryption. Use it on a network you trust. See the security section of `docs/wifi-protocol.md`.
- An Xbox 360 pad has no paddles or extra buttons. Map the paddles to standard buttons on the controller.
