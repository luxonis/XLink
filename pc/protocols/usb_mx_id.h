#ifndef _USB_MX_ID_H_
#define _USB_MX_ID_H_

#include <stdint.h>
#include <stdbool.h>


const uint8_t* usb_mx_id_get_payload();
const uint8_t* usb_mx_id_get_payload_size();
int usb_mx_id_cache_store_entry(const char* mx_id, const char* compat_addr);
bool usb_mx_id_cache_get_entry(const char* compat_addr, char* mx_id);
void usb_mx_id_cache_init();


#endif