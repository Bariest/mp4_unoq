/* net_status.cpp -- see net_status.h */
#include <string.h>
#include "shim_esp.h"
#include "net_status.h"

namespace {
bool s_connected;
char s_ip[48], s_ssid[40];
volatile uint32_t s_seq;
}

extern "C" {

void mpx_net_set(bool connected, const char *ip, const char *ssid)
{
    mpx_critical_enter();
    const bool same = connected == s_connected && strcmp(ip ? ip : "", s_ip) == 0 &&
                      strcmp(ssid ? ssid : "", s_ssid) == 0;
    s_connected = connected;
    strncpy(s_ip, ip ? ip : "", sizeof s_ip - 1);
    strncpy(s_ssid, ssid ? ssid : "", sizeof s_ssid - 1);
    if (!same) ++s_seq;
    mpx_critical_exit();
}

bool mpx_net_get(char *ip, size_t ipn, char *ssid, size_t ssidn)
{
    mpx_critical_enter();
    strncpy(ip, s_ip, ipn - 1);
    ip[ipn - 1] = 0;
    strncpy(ssid, s_ssid, ssidn - 1);
    ssid[ssidn - 1] = 0;
    const bool c = s_connected;
    mpx_critical_exit();
    return c;
}

uint32_t mpx_net_seq(void) { return s_seq; }

}  // extern "C"
