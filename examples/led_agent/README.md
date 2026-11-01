# LED agent

The simplest profile. It exposes the on-board RGB LED (GPIO27, brightness
capped at 25 %) and one external LED on GPIO23. Wire the external LED with a
330 Ω series resistor to GND.

Load it: `/devices profile load`, then paste `profile.json` (one line).

Try:

- `/ask Make the status LED orange at half brightness`
- `/ask Blink? No — just turn the desk LED on`

Expected tool calls:

- `set_actuator {"device":"status_led","value":50,"color":"#ff8000"}`
- `gpio_write {"device":"desk_led","level":1}`, verified by readback
