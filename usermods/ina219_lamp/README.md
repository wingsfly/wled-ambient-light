# INA219 lamp telemetry

This usermod reads INA219 raw shunt voltage (register 0x01, signed 10 uV/LSB)
and bus voltage (0x02, 4 mV/LSB after shifting by three).
It does not use the INA219 current/power registers or write calibration 0x05.
It is **telemetry only**, sampled once per second, not overcurrent protection.

| Hardware | Build environment | Shunt | Current LSB |
|---|---|---|---|
| Existing extension board; original A3 stage12 | `lamp` | 10 mOhm (default) | 1 mA |
| A3 2026-09-11 Bourns R8 revision | `lamp_a3_3mohm` | 3 mOhm | 3.333 mA |

The A3 environment only integrates the telemetry resistance; its inherited pin
and operating configuration is not a complete new-board qualification.
The A3 hardware is still NOT FOR FAB and the optional INA219 is DNP by default.
No hardware was flashed as part of this change.

At 4.6 A, the new shunt produces 13.8 mV (raw 1380). Using `lamp` would display
1.38 A. Conversely the 3 mOhm environment on a 10 mOhm board overstates current
by 3.333x. The Info page reports the compiled shunt value when INA219 is found.
This is nominal conversion, not per-board factory calibration: resistor tolerance,
temperature/lifetime drift, offset, gain error and PCB/solder error remain.

Reference: [TI INA219 datasheet](https://www.ti.com/lit/ds/symlink/ina219.pdf),
Shunt Voltage Register and Bus Voltage Register sections.
