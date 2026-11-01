# Autonomous controller

This example exercises event-driven operation with no user input. The
threshold is deliberately low (20 °C die temperature) so the safety rule
trips on the bench. Do not use it unchanged with a real load.

**Deterministic and offline**

- `test_cooling` switches `cooling_fan` (GPIO23) on as soon as the chip
  temperature exceeds 20 °C. It works with Wi-Fi off and no provider
  configured (checklist item 14).
- `/ask Turn off the cooling fan` is refused with `locked_by_safety_rule`.

**Event to agent**

- **BOOT button press:** a debounced `gpio_edge` starts an *autonomous* run.
  That run's ceiling is READ, so it cannot change the LED until you run
  `/policy auto-allow set_actuator` and `/policy save`.
- **`rule_on`:** the agent writes a short report, rate-limited to one per minute.
