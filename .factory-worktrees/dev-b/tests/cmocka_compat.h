/* cmocka_compat.h — Compatibility shim for system cmocka 1.1.7.
 *
 * Some system cmocka packages (e.g. Debian's libcmocka-dev 1.1.7) lack
 * functions present in cmocka 2.x (nix-shell).  This header provides
 * shims so test code compiles on both environments.
 */
#ifndef CBX_CMOCKA_COMPAT_H
#define CBX_CMOCKA_COMPAT_H

/* cmocka 2.x added assert_int_in_range; cmocka 1.1.7 only has
 * assert_in_range.  Map the newer name to the older function. */
#ifndef assert_int_in_range
#define assert_int_in_range(value, minimum, maximum) \
    assert_in_range((value), (minimum), (maximum))
#endif

#endif /* CBX_CMOCKA_COMPAT_H */
