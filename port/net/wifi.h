/*
 * Wi-Fi on the XIAO ESP32S3 (port/net/wifi.c): ESP-IDF's Wi-Fi driver in
 * station mode.  This header does not include ESP-IDF's: its users (the
 * shell's Wi-Fi interface, shell/port) see only these types.
 */
#ifndef PORT_NET_WIFI_H
#define PORT_NET_WIFI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Brings the driver up: the default event loop, esp_wifi_init(), station
 * mode with the configuration in RAM, esp_wifi_start() (which powers the
 * radio and runs the PHY's calibration), power saving off.  What happens
 * goes into the RAM log ("dmesg").  Once, from a thread.  Returns 0, or -1
 * when the driver is not running: a step failed (logged), or the last run
 * ended with a fatal error in the thread that is calling now (in this
 * bring-up, for all that can be told), which is not repeated.
 */
int net_wifi_start(void);

#define NET_WIFI_MAC_LEN        6

/* The security an access point announces. */
enum net_wifi_security {
    NET_WIFI_SECURITY_OPEN,
    NET_WIFI_SECURITY_WEP,
    NET_WIFI_SECURITY_WPA_PSK,
    NET_WIFI_SECURITY_WPA2_PSK,
    NET_WIFI_SECURITY_WPA_WPA2_PSK,     /* WPA or WPA2, PSK */
    NET_WIFI_SECURITY_WPA3_SAE,
    NET_WIFI_SECURITY_WPA2_WPA3,        /* WPA2-PSK or WPA3-SAE */
    NET_WIFI_SECURITY_WAPI,
    NET_WIFI_SECURITY_OWE,
    NET_WIFI_SECURITY_ENTERPRISE,       /* 802.1X, any version */
    NET_WIFI_SECURITY_DPP,
    NET_WIFI_SECURITY_UNKNOWN,
};

/*
 * Management frame protection (802.11w) of an access point, as far as its
 * security implies it: the driver's scan records do not carry the RSN
 * capabilities.  WPA3-SAE and OWE require it, WPA2/WPA3 transition mode
 * offers it, and there is none without RSN (open, WEP, WPA); for the
 * others it is not known.
 */
enum net_wifi_mfp {
    NET_WIFI_MFP_DISABLED,
    NET_WIFI_MFP_OPTIONAL,
    NET_WIFI_MFP_REQUIRED,
    NET_WIFI_MFP_UNKNOWN,
};

#define NET_WIFI_SSID_MAX_LEN   32

/* One access point a scan found (2.4 GHz band only). */
struct net_wifi_scan_entry {
    uint8_t ssid[NET_WIFI_SSID_MAX_LEN + 1];    /* terminated */
    uint8_t ssid_len;
    uint8_t bssid[NET_WIFI_MAC_LEN];
    uint8_t channel;
    int8_t rssi;                                /* dBm */
    enum net_wifi_security security;
    enum net_wifi_mfp mfp;
};

/*
 * Scans all channels (the driver's default active scan).  Returns 0 when
 * the scan has started; then result() is called for each access point and
 * done() once at the end with 0, or a negative errno value when the scan
 * failed, both from the event loop's task ("sys_evt").  -ENETDOWN: the
 * driver is not running; -EBUSY: a scan is in progress; -EIO: the driver
 * refused.  From a thread.
 */
int net_wifi_scan(void (*result)(const struct net_wifi_scan_entry *entry, void *arg),
                  void (*done)(int status, void *arg), void *arg);

/* What the station is doing. */
enum net_wifi_state {
    NET_WIFI_STATE_DOWN,        /* the driver is not running */
    NET_WIFI_STATE_IDLE,        /* running, not connected */
    NET_WIFI_STATE_SCANNING,
};

enum net_wifi_state net_wifi_state(void);

#endif
