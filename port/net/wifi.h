/*
 * Wi-Fi on the XIAO ESP32S3 (port/net/wifi.c).
 */
#ifndef PORT_NET_WIFI_H
#define PORT_NET_WIFI_H

/*
 * Brings the radio up as far as this firmware goes: the Wi-Fi/Bluetooth
 * power domain on, the PHY enabled and calibrated for Wi-Fi (a full RF
 * calibration, every start).  The result goes into the RAM log ("dmesg").
 * Once, from a thread.  Returns 0, or -1 when the PHY was left off: the
 * last run ended with a fatal error in the thread that is calling now (in
 * this bring-up, for all that can be told), which is not repeated.
 */
int net_wifi_start(void);

#endif
