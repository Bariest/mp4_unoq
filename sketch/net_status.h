/* net_status.h -- the network as Linux sees it, for the line under the eyes
 * (display_glue.cpp): the address to type and the web app's port. */
#ifndef NET_STATUS_H
#define NET_STATUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void     mpx_net_set(bool connected, const char *ip, const char *ssid);
bool     mpx_net_get(char *ip, size_t ipn, char *ssid, size_t ssidn);
uint32_t mpx_net_seq(void);            /* bumps on every change */
#ifdef __cplusplus
}
#endif
#endif
