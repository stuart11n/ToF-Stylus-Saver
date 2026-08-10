import network
import time

try:
    from secrets import WIFI_SSID, WIFI_PASSWORD
except ImportError:
    WIFI_SSID = ""
    WIFI_PASSWORD = ""


def connect(ssid=WIFI_SSID, password=WIFI_PASSWORD, timeout_s=30):
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    if sta.isconnected():
        return sta

    # Prefer strongest matching BSSID when several APs share the SSID
    bssid = None
    try:
        aps = [a for a in sta.scan() if a[0] == ssid.encode()]
        if aps:
            aps.sort(key=lambda a: -a[3])
            bssid = aps[0][1]
    except OSError:
        pass

    if bssid:
        sta.connect(ssid, password, bssid=bssid)
    else:
        sta.connect(ssid, password)

    deadline = time.ticks_add(time.ticks_ms(), timeout_s * 1000)
    while not sta.isconnected():
        if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
            break
        time.sleep_ms(250)

    return sta


def ifconfig():
    sta = network.WLAN(network.STA_IF)
    return sta.ifconfig() if sta.active() else None
