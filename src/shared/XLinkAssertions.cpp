#include <bits/stdint-uintn.h>
#include <cstddef>
#include "XLinkPrivateDefines.h"

static_assert(offsetof(xLinkEventHeader_t, id) == 0, "Offset to id is not 0");
static_assert(offsetof(xLinkEventHeader_t, type) == 4, "Offset to type is not 4");
static_assert(offsetof(xLinkEventHeader_t, streamName) == 8, "Offset to type is not 8");
static_assert(offsetof(xLinkEventHeader_t, tnsec) == 60, "Offset to type is not 60");
static_assert(offsetof(xLinkEventHeader_t, tsecLsb) == 64, "Offset to type is not 64");
static_assert(offsetof(xLinkEventHeader_t, tsecMsb) == 68, "Offset to type is not 68");
