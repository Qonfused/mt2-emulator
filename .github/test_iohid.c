// test_iohid.c — does IOHIDUserDeviceCreate work without an entitlement?
//
// Compile (no entitlement, no codesign):
//     clang -framework IOKit -framework CoreFoundation test_iohid.c -o test_iohid
//
// Run:
//     ./test_iohid
//
// If anything fails, dump the system log to see why:
//     log show --predicate 'process == "test_iohid"' --last 2m
//
// If the result is success across all three tests, the entitlement gate
// is not enforced on this macOS version (or this binary is somehow
// being granted the entitlement). If they all fail with "is not entitled"
// in the system log, the gate is enforced for unentitled userspace
// processes on this version.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// IOHIDUserDevice.h is not in the public macOS SDK; it lives in
// apple-oss-distributions/IOKitUser/hid.subproj/IOHIDUserDevice.h. The
// functions are exported from IOKit.framework's binary, so we just
// forward-declare the symbols we need and let the linker resolve them.
typedef struct __IOHIDUserDevice * IOHIDUserDeviceRef;

extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(
    CFAllocatorRef allocator,
    CFDictionaryRef properties);

// kIOHIDVirtualHIDevice lives in IOHIDPrivateKeys.h (also not in the
// public SDK). The string value is stable across versions.
#define kIOHIDVirtualHIDevice "VirtualHIDevice"

// Minimal generic mouse HID descriptor (52 bytes, well-formed, no Apple
// magic). Using this isolates the entitlement check from any "is this
// claiming to be Apple hardware" check that might happen separately.
static const uint8_t kMouseDescriptor[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    0x05, 0x09,  //     Usage Page (Button)
    0x19, 0x01,  //     Usage Minimum (1)
    0x29, 0x03,  //     Usage Maximum (3)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x95, 0x03,  //     Report Count (3)
    0x75, 0x01,  //     Report Size (1)
    0x81, 0x02,  //     Input (Data, Var, Abs)
    0x95, 0x01,  //     Report Count (1)
    0x75, 0x05,  //     Report Size (5)
    0x81, 0x03,  //     Input (Const, Var, Abs)
    0x05, 0x01,  //     Usage Page (Generic Desktop)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x02,  //     Report Count (2)
    0x81, 0x06,  //     Input (Data, Var, Rel)
    0xC0,        //   End Collection
    0xC0,        // End Collection
};

static CFMutableDictionaryRef MakeProps(uint16_t vid, uint16_t pid,
                                        const char* product,
                                        const uint8_t* desc, size_t desc_len) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32_t v = vid;
    CFNumberRef vidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(props, CFSTR(kIOHIDVendorIDKey), vidNum);
    CFRelease(vidNum);

    int32_t p = pid;
    CFNumberRef pidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(props, CFSTR(kIOHIDProductIDKey), pidNum);
    CFRelease(pidNum);

    CFStringRef productStr = CFStringCreateWithCString(
        kCFAllocatorDefault, product, kCFStringEncodingUTF8);
    CFDictionarySetValue(props, CFSTR(kIOHIDProductKey), productStr);
    CFRelease(productStr);

    CFDataRef descData = CFDataCreate(kCFAllocatorDefault, desc, (CFIndex)desc_len);
    CFDictionarySetValue(props, CFSTR(kIOHIDReportDescriptorKey), descData);
    CFRelease(descData);

    return props;
}

static int RunTest(const char* label, uint16_t vid, uint16_t pid,
                   const char* product) {
    printf("== %s ==\n", label);
    printf("  VID=0x%04X PID=0x%04X Product=\"%s\"\n", vid, pid, product);

    CFMutableDictionaryRef props = MakeProps(vid, pid, product,
                                             kMouseDescriptor,
                                             sizeof(kMouseDescriptor));

    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
    CFRelease(props);

    if (dev) {
        printf("  RESULT: SUCCESS — IOHIDUserDeviceCreate returned %p\n", dev);
        // Hold the device for a moment so the kernel actually registers it
        // and so anything reading the IO registry has a chance to see it.
        sleep(1);
        CFRelease(dev);
        printf("\n");
        return 0;
    } else {
        printf("  RESULT: FAILED — IOHIDUserDeviceCreate returned NULL\n");
        printf("  (check `log show --predicate 'process == \"test_iohid\"' --last 2m`\n"
               "   for an \"is not entitled\" message or other diagnostic)\n");
        printf("\n");
        return 1;
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("test_iohid — IOHIDUserDeviceCreate entitlement probe\n");
    printf("built: %s %s\n", __DATE__, __TIME__);
    printf("uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
    printf("\n");

    int failures = 0;

    // Test 1: a clearly-not-Apple generic mouse. If the entitlement gate
    // is on this version, this fails.
    failures += RunTest("Test 1: generic mouse",
                        0x1234, 0x5678, "test_iohid generic mouse");

    // Test 2: same generic mouse, but with the kIOHIDVirtualHIDevice
    // property explicitly set to true. The kernel sets this implicitly
    // anyway when no entitlement is present, but we set it explicitly to
    // make sure we're not tripping any "you're claiming to be real
    // hardware" check.
    {
        printf("== Test 2: generic mouse with kIOHIDVirtualHIDevice=true ==\n");
        printf("  VID=0x1234 PID=0x5678\n");
        CFMutableDictionaryRef props = MakeProps(
            0x1234, 0x5678, "test_iohid virtual mouse",
            kMouseDescriptor, sizeof(kMouseDescriptor));
        CFDictionarySetValue(props, CFSTR(kIOHIDVirtualHIDevice), kCFBooleanTrue);
        IOHIDUserDeviceRef dev = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
        CFRelease(props);
        if (dev) {
            printf("  RESULT: SUCCESS — returned %p\n", dev);
            sleep(1);
            CFRelease(dev);
        } else {
            printf("  RESULT: FAILED — returned NULL\n");
            failures++;
        }
        printf("\n");
    }

    // Test 3: spoof Apple Magic Trackpad 2 (BT MT2 PID).
    // If Test 1 succeeds but this fails, there's a separate "claiming to
    // be Apple hardware" check (we wouldn't expect that, but worth ruling
    // out). If Test 1 fails, this almost certainly fails too.
    failures += RunTest("Test 3: Magic Trackpad 2 spoof",
                        0x05AC, 0x030E, "Magic Trackpad 2");

    printf("=========================================================\n");
    if (failures == 0) {
        printf("ALL TESTS PASSED.\n");
        printf("\n");
        printf("On this macOS version, IOHIDUserDeviceCreate succeeded\n");
        printf("from an unentitled userspace binary. The mt2-emulator\n");
        printf("approach is viable here without disabling SIP.\n");
        printf("\n");
        printf("Run `ioreg -l -w 0 | grep -A 5 test_iohid` immediately after\n");
        printf("a successful test to see the device in the IO registry\n");
        printf("(it lives only for the 1s sleep above).\n");
    } else {
        printf("%d TEST(S) FAILED.\n", failures);
        printf("\n");
        printf("Run this to see the kernel's reason:\n");
        printf("  log show --predicate 'process == \"test_iohid\"' --last 2m\n");
        printf("\n");
        printf("If you see \"test_iohid is not entitled\", the entitlement\n");
        printf("gate is enforced for unentitled userspace processes on\n");
        printf("this macOS version. The mt2-emulator userspace approach\n");
        printf("will not work here without an Apple-granted entitlement\n");
        printf("or disabling SIP+AMFI.\n");
    }
    printf("=========================================================\n");

    return failures == 0 ? 0 : 1;
}
