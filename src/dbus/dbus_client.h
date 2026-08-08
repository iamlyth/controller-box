#ifndef CBX_DBUS_CLIENT_H
#define CBX_DBUS_CLIENT_H

#include "dbus_mock.h"

/* Production sd-bus backend. */
const ip_dbus_backend *ip_dbus_sd_backend(void);

/* Native InputPlumber property signature used by the production transport.
 * Exposed so compatibility tests cannot regress to a string-only mock model. */
const char *ip_dbus_property_signature(const char *property);

#endif
