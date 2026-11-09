"""Raw /dev/i2s receive data for a given IR signal, as a Harmony Hub's receiver delivers it.

Measured on a Hub (and matching stock hal): the IR sensor's raw output with the carrier, one bit per
1.5 us, MSB first, inverted (light = 0 bit), with the two bytes of every 16-bit sample swapped.
Used by test_co_simulation.py (one-shot capture) and test_ws_events.py / test_mqtt_v2.py (capture mode).
"""

IR_RX_US_PER_BIT = 1.5


def ir_rx_bitstream(timings_us, idle_before_bytes=2048, idle_after_us=30000, carrier_hz=38000, duty=1.0 / 3):
    """Raw /dev/i2s receive data for alternating mark/space durations (starting with a mark)"""
    period = 1e6 / carrier_hz
    light = [0] * (idle_before_bytes * 8)
    for i, d in enumerate(list(timings_us) + [idle_after_us]):
        n = round(d / IR_RX_US_PER_BIT)
        pulses = round(d / period)  # a remote sends whole carrier pulses: the last one is not cut short
        for k in range(n):
            tt = k * IR_RX_US_PER_BIT
            light.append(1 if i % 2 == 0 and tt < pulses * period and (tt % period) < period * duty else 0)
    light += [0] * (-len(light) % 16)
    data = bytearray()
    for j in range(0, len(light), 8):
        byte = 0
        for b in light[j:j + 8]:
            byte = (byte << 1) | b
        data.append(~byte & 0xFF)
    for j in range(0, len(data), 2):
        data[j], data[j + 1] = data[j + 1], data[j]
    return bytes(data)


def nec_timings(code32):
    """NEC frame: 9 ms / 4.5 ms header, 32 bits MSB first as received (560 us marks, 560 / 1690 us spaces), end mark"""
    t = [9000, 4500]
    for i in range(31, -1, -1):
        t += [560, 1690 if (code32 >> i) & 1 else 560]
    return t + [560]


def sirc12_timings(bits):
    """Sony SIRC 12-bit frame: 2.4 ms header, 1.2 ms (1) or 0.6 ms (0) marks, 0.6 ms spaces"""
    t = [2400, 600]
    for i, b in enumerate(bits):
        t.append(1200 if b else 600)
        if i < len(bits) - 1:
            t.append(600)
    return t
