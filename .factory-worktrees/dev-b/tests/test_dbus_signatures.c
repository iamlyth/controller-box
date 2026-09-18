#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "dbus/dbus_client.h"

static void test_native_inputplumber_signatures(void **state)
{
    (void)state;
    assert_string_equal(ip_dbus_property_signature("GamepadOrder"), "as");
    assert_string_equal(ip_dbus_property_signature("TargetDevices"), "as");
    assert_string_equal(ip_dbus_property_signature("SourceDevicePaths"), "as");
    assert_string_equal(ip_dbus_property_signature("DbusDevices"), "as");
    assert_string_equal(ip_dbus_property_signature("Capabilities"), "as");
    assert_string_equal(ip_dbus_property_signature("InterceptMode"), "u");
    assert_string_equal(ip_dbus_property_signature("Enabled"), "b");
    assert_string_equal(ip_dbus_property_signature("ManageAllDevices"), "b");
    assert_string_equal(ip_dbus_property_signature("Version"), "s");
    assert_null(ip_dbus_property_signature(NULL));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_native_inputplumber_signatures),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
