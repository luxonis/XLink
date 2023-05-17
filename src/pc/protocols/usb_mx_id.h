#ifndef _USB_MX_ID_H_
#define _USB_MX_ID_H_

#ifdef __cplusplus
#define XLINK_NOEXCEPT noexcept
extern "C" {
#else
#define XLINK_NOEXCEPT
#endif

#include <stdint.h>
#include <stdbool.h>

const uint8_t* usb_mx_id_get_payload() XLINK_NOEXCEPT;
int usb_mx_id_get_payload_size() XLINK_NOEXCEPT;
const uint8_t* usb_mx_id_get_payload_end() XLINK_NOEXCEPT;
int usb_mx_id_get_payload_end_size() XLINK_NOEXCEPT;

int usb_mx_id_cache_store_entry(const char* mx_id, const char* compat_addr) XLINK_NOEXCEPT;
bool usb_mx_id_cache_get_entry(const char* compat_addr, char* mx_id) XLINK_NOEXCEPT;
void usb_mx_id_cache_init() XLINK_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef XLINK_NOEXCEPT

#endif