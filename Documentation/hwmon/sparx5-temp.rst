.. SPDX-License-Identifier: GPL-2.0-only

Microchip SparX-5 SoC
=====================

Supported chips:

  * VSC7546, VSC7549, VSC755, VSC7556, and VSC7558 (Sparx5 series)

    Prefix: 'sparx5-temp'

    Addresses scanned: -

    Datasheet: Provided by Microchip upon request and under NDA

Author: Lars Povlsen <lars.povlsen@microchip.com>

The driver also supports the Microchip LAN969x temperature sensor and fan
tachometer through the microchip,lan9691-hwmon compatible.

Description
-----------

The Sparx5 SoC contains a temperature sensor based on the MR74060
Moortec IP.

The sensor has a range of -40°C to +125°C and an accuracy of +/-5°C.

Sysfs entries
-------------

The following attributes are supported.

======================= ========================================================
temp1_input		Die temperature (in millidegree Celsius.)
fan1_input              Fan speed in RPM (LAN969x only).
pwm1                    PWM duty cycle, from 0 to 255 (LAN969x only).
pwm1_freq               PWM frequency in Hz (LAN969x only).
======================= ========================================================

The tachometer counts pulses over one-second intervals, independently of the
PWM output. The fan child's pulses-per-revolution property specifies how many
pulses represent one revolution; it defaults to two.

On LAN969x, the fan child's pwms property selects the initial PWM period and
polarity. The output is dedicated to this fan and is not available to external
PWM consumers. The pwm1 value describes the logical duty cycle independently
of the output polarity. Frequency requests are rounded and clamped to values
representable by the hardware divider; pwm1_freq reports the resulting
frequency.

Thermal cooling
---------------

With device-tree thermal support enabled, the LAN969x fan can also act as a
thermal cooling device. The fan child's cooling-levels property maps cooling
states to PWM values from 0 to 255, in ascending order. When cooling levels
are provided, the driver initially selects the highest level.

Manual pwm1 writes remain available and update the reported cooling state
to the highest state whose PWM value does not exceed the requested value,
or state zero if the value is below the first level. Subsequent thermal
cooling requests can override manual PWM settings. Each cooling request
programs the exact PWM value associated with the requested state.
