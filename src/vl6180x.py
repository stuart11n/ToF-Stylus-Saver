# MicroPython port of Pololu VL6180X Arduino library
# https://github.com/pololu/vl6180x-arduino
#
# I2C uses MicroPython mem ops with addrsize=16 (repeated start). Split
# writeto+readfrom breaks 16-bit register access on ESP32-S2.

from machine import I2C, Pin
import time

ADDRESS_DEFAULT = 0x29

# RANGE_SCALER values for 1x, 2x, 3x — STSW-IMG003 ScalerLookUP[]
_SCALER_VALUES = (0, 253, 127, 84)

ERROR_NONE = 0
ERROR_SYSERR_1 = 1
ERROR_SYSERR_5 = 5
ERROR_ECEFAIL = 6
ERROR_NOCONVERGE = 7
ERROR_RANGEIGNORE = 8
ERROR_SNR = 11
ERROR_RAWUFLOW = 12
ERROR_RAWOFLOW = 13
ERROR_RANGEUFLOW = 14
ERROR_RANGEOFLOW = 15

_STATUS_NAME = {
    0: "NONE",
    1: "SYSERR_1",
    5: "SYSERR_5",
    6: "ECEFAIL",
    7: "NOCONVERGE",
    8: "RANGEIGNORE",
    11: "SNR",
    12: "RAWUFLOW",
    13: "RAWOFLOW",
    14: "RANGEUFLOW",
    15: "RANGEOFLOW",
}

# Register map (subset used by Pololu)
IDENTIFICATION__MODEL_ID = 0x000
SYSTEM__MODE_GPIO1 = 0x011
SYSTEM__INTERRUPT_CONFIG_GPIO = 0x014
SYSTEM__INTERRUPT_CLEAR = 0x015
SYSTEM__FRESH_OUT_OF_RESET = 0x016
SYSRANGE__START = 0x018
SYSRANGE__INTERMEASUREMENT_PERIOD = 0x01B
SYSRANGE__MAX_CONVERGENCE_TIME = 0x01C
SYSRANGE__CROSSTALK_VALID_HEIGHT = 0x021
SYSRANGE__PART_TO_PART_RANGE_OFFSET = 0x024
SYSRANGE__RANGE_CHECK_ENABLES = 0x02D
SYSRANGE__VHV_RECALIBRATE = 0x02E
SYSRANGE__VHV_REPEAT_RATE = 0x031
SYSALS__START = 0x038
SYSALS__INTERMEASUREMENT_PERIOD = 0x03E
SYSALS__ANALOGUE_GAIN = 0x03F
SYSALS__INTEGRATION_PERIOD = 0x040
RESULT__RANGE_STATUS = 0x04D
RESULT__INTERRUPT_STATUS_GPIO = 0x04F
RESULT__ALS_VAL = 0x050
RESULT__RANGE_VAL = 0x062
RESULT__RANGE_RAW = 0x064
RESULT__RANGE_RETURN_RATE = 0x066
RESULT__RANGE_REFERENCE_RATE = 0x068
SYSRANGE__CROSSTALK_COMPENSATION_RATE = 0x01E
RANGE_SCALER = 0x096
READOUT__AVERAGING_SAMPLE_PERIOD = 0x10A
INTERLEAVED_MODE__ENABLE = 0x2A3
I2C_SLAVE__DEVICE_ADDRESS = 0x212


def make_i2c(sda=8, scl=9, freq=400000):
    return I2C(0, sda=Pin(sda), scl=Pin(scl), freq=freq)


class VL6180X:
    def __init__(self, i2c, address=ADDRESS_DEFAULT, shdn_pin=None, timeout_ms=500):
        self.i2c = i2c
        self.address = address
        self.scaling = 0
        self.ptp_offset = 0
        self.io_timeout = timeout_ms
        self.did_timeout = False
        self.last_i2c_status = 0  # Pololu last_status (I2C); keep name clear
        self._shdn = None

        if shdn_pin is not None:
            self._shdn = Pin(shdn_pin, Pin.OUT, value=0)
            self._hardware_reset()

        self.init()
        self.configureDefault()

    def _hardware_reset(self):
        """
        SHDN (GPIO0 / XSHUT): hold low to power down, release high to boot.
        ST/Pololu need a fresh-out-of-reset cycle for private-register init.
        Wait >= 5 ms after release before any I2C (user/board requirement).
        """
        if self._shdn is None:
            return
        self._shdn.value(0)
        time.sleep_ms(10)  # ensure full power-down
        self._shdn.value(1)
        time.sleep_ms(10)  # >= 5 ms before first I2C

    def writeReg(self, reg, value):
        self.i2c.writeto_mem(self.address, reg, bytes([value & 0xFF]), addrsize=16)
        self.last_i2c_status = 0

    def writeReg16Bit(self, reg, value):
        self.i2c.writeto_mem(
            self.address,
            reg,
            bytes([(value >> 8) & 0xFF, value & 0xFF]),
            addrsize=16,
        )
        self.last_i2c_status = 0

    def writeReg32Bit(self, reg, value):
        self.i2c.writeto_mem(
            self.address,
            reg,
            bytes(
                [
                    (value >> 24) & 0xFF,
                    (value >> 16) & 0xFF,
                    (value >> 8) & 0xFF,
                    value & 0xFF,
                ]
            ),
            addrsize=16,
        )
        self.last_i2c_status = 0

    def readReg(self, reg):
        return self.i2c.readfrom_mem(self.address, reg, 1, addrsize=16)[0]

    def readReg16Bit(self, reg):
        b = self.i2c.readfrom_mem(self.address, reg, 2, addrsize=16)
        return (b[0] << 8) | b[1]

    def readReg32Bit(self, reg):
        b = self.i2c.readfrom_mem(self.address, reg, 4, addrsize=16)
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]

    def setAddress(self, new_addr):
        self.writeReg(I2C_SLAVE__DEVICE_ADDRESS, new_addr & 0x7F)
        self.address = new_addr

    def init(self):
        """AN4545 'SR03 settings' — Mandatory private registers (Pololu)."""
        # Ensure we see the chip after SHDN release
        t0 = time.ticks_ms()
        while True:
            try:
                model = self.readReg(IDENTIFICATION__MODEL_ID)
                if model == 0xB4:
                    break
            except OSError:
                pass
            if time.ticks_diff(time.ticks_ms(), t0) > 500:
                raise OSError("VL6180X not found (model id)")
            time.sleep_ms(5)

        self.ptp_offset = self.readReg(SYSRANGE__PART_TO_PART_RANGE_OFFSET)

        if self.readReg(SYSTEM__FRESH_OUT_OF_RESET) == 1:
            self.scaling = 1
            self.writeReg(0x207, 0x01)
            self.writeReg(0x208, 0x01)
            self.writeReg(0x096, 0x00)
            self.writeReg(0x097, 0xFD)  # RANGE_SCALER = 253
            self.writeReg(0x0E3, 0x01)
            self.writeReg(0x0E4, 0x03)
            self.writeReg(0x0E5, 0x02)
            self.writeReg(0x0E6, 0x01)
            self.writeReg(0x0E7, 0x03)
            self.writeReg(0x0F5, 0x02)
            self.writeReg(0x0D9, 0x05)
            self.writeReg(0x0DB, 0xCE)
            self.writeReg(0x0DC, 0x03)
            self.writeReg(0x0DD, 0xF8)
            self.writeReg(0x09F, 0x00)
            self.writeReg(0x0A3, 0x3C)
            self.writeReg(0x0B7, 0x00)
            self.writeReg(0x0BB, 0x3C)
            self.writeReg(0x0B2, 0x09)
            self.writeReg(0x0CA, 0x09)
            self.writeReg(0x198, 0x01)
            self.writeReg(0x1B0, 0x17)
            self.writeReg(0x1AD, 0x00)
            self.writeReg(0x0FF, 0x05)
            self.writeReg(0x100, 0x05)
            self.writeReg(0x199, 0x05)
            self.writeReg(0x1A6, 0x1B)
            self.writeReg(0x1AC, 0x3E)
            self.writeReg(0x1A7, 0x1F)
            self.writeReg(0x030, 0x00)
            self.writeReg(SYSTEM__FRESH_OUT_OF_RESET, 0)
        else:
            s = self.readReg16Bit(RANGE_SCALER)
            if s == _SCALER_VALUES[3]:
                self.scaling = 3
            elif s == _SCALER_VALUES[2]:
                self.scaling = 2
            else:
                self.scaling = 1
            self.ptp_offset *= self.scaling

    def configureDefault(self):
        """AN4545 recommended + optional public registers (Pololu)."""
        self.writeReg(READOUT__AVERAGING_SAMPLE_PERIOD, 0x30)
        self.writeReg(SYSALS__ANALOGUE_GAIN, 0x46)
        self.writeReg(SYSRANGE__VHV_REPEAT_RATE, 0xFF)
        self.writeReg16Bit(SYSALS__INTEGRATION_PERIOD, 0x0063)
        self.writeReg(SYSRANGE__VHV_RECALIBRATE, 0x01)

        self.writeReg(SYSRANGE__INTERMEASUREMENT_PERIOD, 0x09)
        self.writeReg(SYSALS__INTERMEASUREMENT_PERIOD, 0x31)
        self.writeReg(SYSTEM__INTERRUPT_CONFIG_GPIO, 0x24)

        self.writeReg(SYSRANGE__MAX_CONVERGENCE_TIME, 0x31)
        self.writeReg(INTERLEAVED_MODE__ENABLE, 0)
        self.setScaling(1)

    def setScaling(self, new_scaling):
        default_crosstalk_valid_height = 20
        if new_scaling < 1 or new_scaling > 3:
            return
        self.scaling = new_scaling
        self.writeReg16Bit(RANGE_SCALER, _SCALER_VALUES[self.scaling])
        self.writeReg(
            SYSRANGE__PART_TO_PART_RANGE_OFFSET, self.ptp_offset // self.scaling
        )
        self.writeReg(
            SYSRANGE__CROSSTALK_VALID_HEIGHT,
            default_crosstalk_valid_height // self.scaling,
        )
        rce = self.readReg(SYSRANGE__RANGE_CHECK_ENABLES)
        self.writeReg(
            SYSRANGE__RANGE_CHECK_ENABLES, (rce & 0xFE) | (1 if self.scaling == 1 else 0)
        )

    def getScaling(self):
        return self.scaling

    def setTimeout(self, timeout_ms):
        self.io_timeout = timeout_ms

    def timeoutOccurred(self):
        tmp = self.did_timeout
        self.did_timeout = False
        return tmp

    def readRangeStatus(self):
        return self.readReg(RESULT__RANGE_STATUS) >> 4

    def status_name(self, code=None):
        if code is None:
            code = self.readRangeStatus()
        return _STATUS_NAME.get(code, "ERR_%d" % code)

    def readRangeContinuous(self):
        t0 = time.ticks_ms()
        while (self.readReg(RESULT__INTERRUPT_STATUS_GPIO) & 0x07) != 0x04:
            if self.io_timeout and time.ticks_diff(time.ticks_ms(), t0) > self.io_timeout:
                self.did_timeout = True
                return 255
            time.sleep_ms(1)
        range_mm = self.readReg(RESULT__RANGE_VAL)
        self.writeReg(SYSTEM__INTERRUPT_CLEAR, 0x01)
        return range_mm

    def readRangeSingle(self):
        self.writeReg(SYSRANGE__START, 0x01)
        return self.readRangeContinuous()

    def readRangeSingleMillimeters(self):
        return self.scaling * self.readRangeSingle()

    def readAmbientContinuous(self):
        t0 = time.ticks_ms()
        while (self.readReg(RESULT__INTERRUPT_STATUS_GPIO) & 0x38) != 0x20:
            if self.io_timeout and time.ticks_diff(time.ticks_ms(), t0) > self.io_timeout:
                self.did_timeout = True
                return 0
            time.sleep_ms(1)
        ambient = self.readReg16Bit(RESULT__ALS_VAL)
        self.writeReg(SYSTEM__INTERRUPT_CLEAR, 0x02)
        return ambient

    def readAmbientSingle(self):
        self.writeReg(SYSALS__START, 0x01)
        return self.readAmbientContinuous()

    def startRangeContinuous(self, period=100):
        period_reg = (period // 10) - 1
        if period_reg < 0:
            period_reg = 0
        if period_reg > 254:
            period_reg = 254
        self.writeReg(SYSRANGE__INTERMEASUREMENT_PERIOD, period_reg)
        self.writeReg(SYSRANGE__START, 0x03)

    def stopContinuous(self):
        self.writeReg(SYSRANGE__START, 0x01)
        self.writeReg(SYSALS__START, 0x01)
        self.writeReg(INTERLEAVED_MODE__ENABLE, 0)

    def readReturnRate(self):
        return self.readReg16Bit(RESULT__RANGE_RETURN_RATE)

    def readRawRange(self):
        return self.readReg(RESULT__RANGE_RAW)

    def calibrate_crosstalk_empty(self, samples=20):
        """
        ST AN4545 empty-FOV crosstalk calibration (not in Pololu RangeSingleShot).
        Aim at open space (no target within ~200 mm). Writes average return rate
        to SYSRANGE__CROSSTALK_COMPENSATION_RATE.
        """
        rates = []
        for _ in range(samples):
            self.readRangeSingle()
            rates.append(self.readReturnRate())
            time.sleep_ms(50)
        # Ignore saturated ECEFAIL garbage (0xFFFF)
        good = [r for r in rates if 0 < r < 65535]
        if not good:
            raise OSError("xtalk cal: no usable return rates")
        good.sort()
        med = good[len(good) // 2]
        self.writeReg16Bit(SYSRANGE__CROSSTALK_COMPENSATION_RATE, med)
        # Keep Pololu default valid-height for 1x
        self.writeReg(SYSRANGE__CROSSTALK_VALID_HEIGHT, 20)
        return med

    def set_ece(self, enabled):
        """Pololu enables ECE at 1x; glass modules often need it off until xtalk is set."""
        rce = self.readReg(SYSRANGE__RANGE_CHECK_ENABLES)
        if enabled:
            self.writeReg(SYSRANGE__RANGE_CHECK_ENABLES, rce | 0x01)
        else:
            self.writeReg(SYSRANGE__RANGE_CHECK_ENABLES, rce & 0xFE)
