# Pololu init/read path + ST empty-FOV crosstalk (required for cover-glass modules)
import time
from vl6180x import VL6180X, make_i2c


def main():
    print("VL6180X — Pololu init, SHDN=GP7")
    sensor = VL6180X(make_i2c(8, 9), shdn_pin=7, timeout_ms=500)
    print(
        "model=0x%02X scaler=%d ptp=%d rce=0x%02X"
        % (
            sensor.readReg(0x000),
            sensor.readReg16Bit(0x096),
            sensor.ptp_offset,
            sensor.readReg(0x02D),
        )
    )

    print("XTALK: aim at EMPTY SPACE (no target) for ~2s...")
    time.sleep_ms(1500)
    xtalk = sensor.calibrate_crosstalk_empty(20)
    # ECE + residual glass often forces ECEFAIL/RAWUFLOW on these modules
    sensor.set_ece(False)
    print("xtalk_reg=%d  ECE=off  rce=0x%02X" % (xtalk, sensor.readReg(0x02D)))

    print("Live — white card at 100mm, then 50mm. Paste lines.")
    while True:
        mm = sensor.readRangeSingleMillimeters()
        st = sensor.readRangeStatus()
        raw = sensor.readRawRange()
        rate = sensor.readReturnRate()
        line = "%3d mm  raw=%3d  rate=%5d  [%s]" % (
            mm,
            raw,
            rate,
            sensor.status_name(st),
        )
        if sensor.timeoutOccurred():
            line += " TIMEOUT"
        print(line)
        time.sleep_ms(80)


main()
