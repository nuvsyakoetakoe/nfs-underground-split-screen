# Split-screen controls

[← README](../README.md) · [Русская инструкция](README.ru.md#управление)

## Assign players

Connect your devices before starting a race. Open **Options → Split Screen Options**, select a separate **Device** for each player, then choose **Save Settings**. Back cancels unsaved changes. Names and assignments are saved to `SplitScreenOptions.ini` beside `Speed.exe`.

For a first race, use **player one: keyboard / player two: XInput gamepad**. The mod also implements two XInput gamepads and selection of individual physical keyboards. These input combinations have not all been independently verified; the confirmed gameplay check is described in the [compatibility section](../README.md#compatibility).

With default settings, **Auto** uses a keyboard for player one and a gamepad for player two when fewer than two XInput gamepads are connected; with two connected, it assigns gamepads. Explicit assignments make it easier to keep track of each player's device. Two players need separate input devices; there is no documented layout for sharing one keyboard.

## Driving

The mod defines these split-screen bindings in `src/local_input.c`. Gamepad labels use the Xbox layout. This describes the implemented mapping; not every action has been tested in every race mode.

| Action | Keyboard | XInput gamepad |
| --- | --- | --- |
| Steer | A / D or Left / Right arrows | Left stick |
| Accelerate | W or Up arrow | RT |
| Brake | S or Down arrow | LT |
| Handbrake | Space | A |
| Shift up | Left Shift | RB |
| Shift down | Left Ctrl | LB |
| Nitrous | Left Alt or N | B |
| Reset car | R or Home | X |
| Change camera | C or Enter | Y |
| Look back | B or Right Ctrl | Right stick click |
| Horn | H | Left stick click |

The gamepad's right stick is also passed to the camera look handler. Gear changes depend on the transmission selected for the car.

An XInput-compatible controller is required for the mapping above. DirectInput-only controllers without conversion have not been verified. If a device is missing or controls the wrong player, see [troubleshooting](TROUBLESHOOTING.md#input-devices).
