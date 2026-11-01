# Temperature agent

This is the scenario from the brief: "Check the temperature and switch on the
cooling fan if it is above 28 degrees."

**Wiring**

- **NTC thermistor (10 kΩ, β 3950)** from GPIO4 (ADC1_CH3) to GND, with a
  10 kΩ resistor from 3V3 to GPIO4. This is the `ntc` model with the NTC on
  the low side.
- **Optional SHT3x** on I2C: SDA GPIO9, SCL GPIO10, address 0x44 (68). Use
  4.7 kΩ pull-ups.
- **Fan** through a logic-level MOSFET on GPIO23 (see docs/HARDWARE.md). Do
  not drive the fan from the pin directly.

The `overheat` safety rule switches the fan on above 32 °C and off below
30 °C, entirely locally. While it holds the fan, the agent cannot turn it
off. If the NTC stops giving valid readings for 20 s (open or short
circuit), the fan is forced on.

Try: `/ask Check the temperature and switch on the cooling fan if it is above 28 degrees.`
