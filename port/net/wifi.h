/*
 * Wi-Fi on the XIAO ESP32S3 (port/net/wifi.c): ESP-IDF's Wi-Fi driver in
 * station mode, with lwIP on top (port/net/wlanif.c).  This header does
 * not include ESP-IDF's or lwIP's: its users (the shell's Wi-Fi interface,
 * shell/port) see only these types.
 */
#ifndef PORT_NET_WIFI_H
#define PORT_NET_WIFI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Brings the driver up: the default event loop, esp_wifi_init(), station
 * mode with the configuration in RAM, esp_wifi_start() (which powers the
 * radio and runs the PHY's calibration), power saving off, then lwIP with
 * the station's interface (wlanif_start()).  What happens goes into the
 * RAM log ("dmesg").  Once, from a thread.  Returns 0, or -1 when the
 * driver is not running: a step failed (logged), or the last run ended
 * with a fatal error in the thread that is calling now (in this bring-up,
 * for all that can be told), which is not repeated.
 */
int net_wifi_start(void);

#define NET_WIFI_MAC_LEN        6
#define NET_WIFI_SSID_MAX_LEN   32

/* The station's MAC address (the factory MAC address in eFuse).  0 or -EIO.  From a thread. */
int net_wifi_mac(uint8_t mac[NET_WIFI_MAC_LEN]);

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
 * Management frame protection (802.11w), as far as the security implies
 * it: the driver's records of an access point do not carry the RSN
 * capabilities.  WPA3-SAE and OWE require it, there is none without RSN
 * (open, WEP, WPA).  For a scan's entry WPA2/WPA3 transition mode offers
 * it and the rest is not known; for the connection (net_wifi_status())
 * the station offers it (capable, not required), so it is optional with
 * any other RSN security.
 */
enum net_wifi_mfp {
    NET_WIFI_MFP_DISABLED,
    NET_WIFI_MFP_OPTIONAL,
    NET_WIFI_MFP_REQUIRED,
    NET_WIFI_MFP_UNKNOWN,
};

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
 * the scan has started; then done() is called once, from the event
 * loop's task ("sys_evt"), with 0 and the number of access points found,
 * or a negative errno value when the scan failed.  done() must not block:
 * the records are taken afterwards from any thread, one by one with
 * net_wifi_scan_next(), and net_wifi_scan_end() ends the scan (until then
 * the state stays NET_WIFI_STATE_SCANNING and no other scan or connection
 * starts).  -ENETDOWN: the driver is not running; -EBUSY: a scan or a
 * connection attempt is in progress; -EIO: the driver refused.  From a
 * thread.
 */
int net_wifi_scan(void (*done)(int status, unsigned int count, void *arg), void *arg);

/* The next record of the scan that ended: 0, or -ENOENT when there is none left. */
int net_wifi_scan_next(struct net_wifi_scan_entry *entry);

/* Frees what is left of the records; the scan is over. */
void net_wifi_scan_end(void);

/*
 * The end of a connection attempt (net_wifi_ops.connect_result()), with
 * the numbers of Zephyr's enum wifi_conn_status.
 */
enum net_wifi_conn_status {
    NET_WIFI_CONN_SUCCESS = 0,
    NET_WIFI_CONN_FAIL = 1,
    NET_WIFI_CONN_WRONG_PASSWORD = 2,
    NET_WIFI_CONN_TIMEOUT = 3,
    NET_WIFI_CONN_AP_NOT_FOUND = 4,
    NET_WIFI_CONN_AUTH_REJECT = 5,
    NET_WIFI_CONN_ASSOC_REJECT = 6,
};

/*
 * Why a connection ended (net_wifi_ops.disconnected()), with the numbers
 * of Zephyr's enum wifi_disconn_reason; NET_WIFI_DISCONN_REQUESTED (0)
 * for net_wifi_disconnect().
 */
enum net_wifi_disconn_reason {
    NET_WIFI_DISCONN_REQUESTED = 0,
    NET_WIFI_DISCONN_UNSPECIFIED = 1,
    NET_WIFI_DISCONN_AP_LEAVING = 3,
    NET_WIFI_DISCONN_INACTIVITY = 4,
};

/*
 * What happens to the connection, for the network interface's owner, in
 * the order of rp2040-connect's NINA driver (common/drivers/nina/include/
 * nina_wifi.h, struct nina_wifi_ops): an attempt ends with
 * ip_set(), link_up(), connect_result(0) once DHCP has given the address,
 * or with connect_result(status) alone; a connection ends with link_down()
 * (and ip_clear() when the address goes with it) and then disconnected().
 * After a connection is lost without a request the station connects again
 * by itself; the address stays until ip_clear(), at most
 * WLANIF_ADDRESS_HOLD_MS (120 s) after the loss, and a connection that
 * comes back ends with ip_set(), link_up(), connect_result(0) again.
 * Addresses are 4 bytes in network order.  Called with port/net/wifi.c's
 * lock held, from the event loop's task, an esp_timer callback or the
 * caller of net_wifi_disconnect(): they must not block.
 */
struct net_wifi_ops {
    void (*ip_set)(const uint8_t addr[4], const uint8_t netmask[4], const uint8_t gateway[4]);
    void (*ip_clear)(const uint8_t addr[4]);
    void (*link_up)(void);
    void (*link_down)(void);
    void (*connect_result)(int status);         /* enum net_wifi_conn_status */
    void (*disconnected)(int reason);           /* enum net_wifi_disconn_reason */
};

/* Keeps ops (static) for good.  Once, before net_wifi_start(). */
void net_wifi_set_ops(const struct net_wifi_ops *ops);

struct net_wifi_connect_params {
    const uint8_t *ssid;
    uint8_t ssid_len;                   /* 1 .. NET_WIFI_SSID_MAX_LEN */
    const uint8_t *passphrase;          /* NULL: an open network */
    uint8_t passphrase_len;             /* 8 .. 63, or 64 hexadecimal digits */
};

/*
 * Starts a connection: to an open network without a passphrase, else to
 * one with WPA2-PSK or better (WPA2/WPA3 transition, WPA3-SAE with
 * hash-to-element or hunt-and-peck), management frame protection offered.
 * 0: the attempt runs and ends with connect_result() (at most 30 s, the
 * time rp2040-connect's NINA driver waits); -ENETDOWN: the driver is not
 * running; -EALREADY: connected, or connecting again after a loss
 * (net_wifi_disconnect() first); -EBUSY: a scan or an attempt is in
 * progress; -EINVAL: bad parameters; -EIO: the driver refused.  From a
 * thread.
 */
int net_wifi_connect(const struct net_wifi_connect_params *params);

/*
 * Ends the connection, the attempt or the reconnection: DHCP releases the
 * address, the driver disconnects, then link_down(), ip_clear() if there
 * was an address, disconnected(NET_WIFI_DISCONN_REQUESTED), before this
 * returns.  0, -ENETDOWN, or -EALREADY when there is nothing to end.
 * From a thread.
 */
int net_wifi_disconnect(void);

/* What the station is doing. */
enum net_wifi_state {
    NET_WIFI_STATE_DOWN,            /* the driver is not running */
    NET_WIFI_STATE_IDLE,            /* running, not connected */
    NET_WIFI_STATE_SCANNING,
    NET_WIFI_STATE_ASSOCIATING,     /* an attempt, not associated yet */
    NET_WIFI_STATE_ASSOCIATED,      /* associated, waiting for DHCP */
    NET_WIFI_STATE_CONNECTED,       /* associated, with an address */
    NET_WIFI_STATE_RECONNECTING,    /* the connection was lost; connecting again */
};

/* The radio's mode on the connection. */
enum net_wifi_phy {
    NET_WIFI_PHY_11B,
    NET_WIFI_PHY_11G,
    NET_WIFI_PHY_11N,
    NET_WIFI_PHY_11AX,
    NET_WIFI_PHY_UNKNOWN,
};

struct net_wifi_status {
    enum net_wifi_state state;
    /* The access point, in the states ASSOCIATED and CONNECTED. */
    uint8_t ssid[NET_WIFI_SSID_MAX_LEN + 1];    /* terminated */
    uint8_t ssid_len;
    uint8_t bssid[NET_WIFI_MAC_LEN];
    uint8_t channel;
    int8_t rssi;                                /* dBm */
    enum net_wifi_security security;
    enum net_wifi_mfp mfp;
    enum net_wifi_phy phy;
};

/* Fills status.  0.  From a thread. */
int net_wifi_status(struct net_wifi_status *status);

#endif
