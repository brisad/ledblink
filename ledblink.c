#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <libusb.h>

#define HID_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02

#define OUTPUT_BUF_SIZE 1024

#define RAND_HALF (RAND_MAX / 2)

#define BOUNCE_END_USLEEP    300000
#define BOUNCE_MIDDLE_USLEEP 100000
#define GLOW_PERIOD_USLEEP    10000
#define RANDBLINK_USLEEP     100000

#define GLOW_STEPS 70

char *speed[] = {"UNKNOWN"
                 "Low (1.5MBit/s)",
                 "Full (12MBit/s)",
                 "High (480MBit/s)",
                 "Super (5000MBit/s)",
                 "Super plus (10000MBit/s)"};

struct leds {
    libusb_device_handle *handle;
    unsigned char ifnum;
    unsigned char report_id;
    size_t total_size;
    int num_lock;
    int caps_lock;
    int scroll_lock;
};

size_t item_data(unsigned char *ptr, unsigned char size) {
    size_t result = 0;
    for (size_t i = 0; i < size; i++) {
        result += *ptr++ << (i * 8);
    }
    return result;
}

#define ERR_RETURN_IF(cond, msg)                                        \
    if (cond) {                                                         \
        fprintf(stderr, "Error: " msg);                                 \
        return false;                                                   \
    }                                                                   \

#define LIBUSB_ERROR(err, msg) fprintf(stderr, msg, libusb_strerror((enum libusb_error)err));

/**
 * Fill passed led struct with data from HID report.
 * Returns true on success, false otherwise.
 * If false is returned, the leds struct is not touched.
 */
bool leds_from_hid_report(unsigned char *report_buf, size_t len, struct leds *leds) {
    unsigned char *end = report_buf + len;
    unsigned char *ptr = report_buf;

    size_t report_size = 0;
    size_t report_count = 0;
    size_t usage_minimum = 0;
    size_t usage_maximum = 0;
    unsigned char report_id = 0;
    bool is_led_usage_page = false;
    bool found_leds = false;

    size_t total_size = 0;

    struct leds result;

    unsigned char tag_and_type;
    size_t data;  // Great variable name

    while (ptr < end) {
        // I could just ignore long items, but I'm lazy here and bail out.
        ERR_RETURN_IF(*ptr == 0xFE, "Found a long item in HID report. Not supported.\n");      

        int size = *ptr & 0x3;
        if (size == 3) size = 4;

        ERR_RETURN_IF(ptr + size >= end, "Item exceeds past HID report buffer.\n");

        tag_and_type = *ptr++ & 0xFC;
        data = item_data(ptr, size);

        switch (tag_and_type) {
        case 0x04:  // Usage Page (Global)
            is_led_usage_page = data == 0x8;
            break;
        case 0x08:  // Usage (Local)
            // I'm skipping this and just assume minimum and maximum
            // is used instead.
            break;
        case 0x18:  // Usage Minimum (Local)
            usage_minimum = data;
            break;
        case 0x28:  // Usage Maximum (Local)
            usage_maximum = data;
            break;
        case 0x74:  // Report Size (Global)
            report_size = data;
            break;
        case 0x84:  // Report ID (Global)
            // report_id is just one byte, so we risk losing data
            // here. We'll just ignore it.
            report_id = data;
            break;
        case 0x94:  // Report Count (Global)
            report_count = data;
            break;
        case 0x90:  // Output (Main)
            if (is_led_usage_page && data == 0x2) {
                // 0x2 means: Data Variable Absolute No_Wrap Linear
                // Preferred_State No_Null_Position Non_volatile Bitfield
                ERR_RETURN_IF(report_size != 1, "Report size for leds is not 1.\n");
                ERR_RETURN_IF(report_count != 3, "Report count for leds is not 3.\n");
                ERR_RETURN_IF(usage_minimum != 1, "Usage minimum is not 1.\n");
                ERR_RETURN_IF(usage_maximum != 3, "Usage maximum is not 3.\n");

                found_leds = true;
                result.report_id = report_id;
                result.num_lock = total_size;
                result.caps_lock = total_size + 1;
                result.scroll_lock = total_size + 2;
            }
            total_size += report_size * report_count;
            // fall through
        case 0x80:  // Input (Main)
        case 0xA0:  // Collection (Main)
        case 0xB0:  // Feature (Main)
        case 0xC0:  // End Collection (Main)
            usage_minimum = 0;
            usage_maximum = 0;
        }
    }

    if (!found_leds) {
        printf("  Found no LEDs in HID report\n");
        return false;
    }

    // Change from size in bits to bytes
    total_size = (total_size + 7) / 8;
    ERR_RETURN_IF(total_size > OUTPUT_BUF_SIZE, "HID report total output size too big.\n");

    *leds = result;
    leds->total_size = total_size;
    return true;
}

/**
 * Find leds on handle and interface, put them in the struct.
 * Returns true on success, false otherwise.
 * If false is returned, the leds struct is not touched.
 */
bool find_leds(libusb_device_handle *handle, int ifnum, struct leds *leds) {
    unsigned char buf[256];
    // Read the HID report descriptor
    int r = libusb_control_transfer(handle,
                                    LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_INTERFACE,
                                    LIBUSB_REQUEST_GET_DESCRIPTOR,
                                    (LIBUSB_DT_REPORT << 8) | 0,
                                    ifnum, buf, sizeof(buf), 1000);
    if (r < 0) {
        LIBUSB_ERROR(r, "Cannot read HID report descriptor: %s\n");
        return false;
    }
    if (!leds_from_hid_report(buf, r, leds)) {
        return false;
    }
    leds->handle = handle;
    leds->ifnum = ifnum;
    return true;
}

void write_output_report(libusb_device_handle *handle, unsigned char ifnum,
                         unsigned char report_id, unsigned char *outdata, size_t outsize) {
    int err;
    err = libusb_control_transfer(handle,
                                  LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS
                                  | LIBUSB_RECIPIENT_INTERFACE,
                                  HID_SET_REPORT,
                                  HID_REPORT_TYPE_OUTPUT << 8 | report_id,
                                  ifnum, outdata, outsize, 1);
    if (err < 0) {
        LIBUSB_ERROR(err, "Cannot write output report: %s\n");
    }
}

void set_output(struct leds *leds, bool num_lock, bool caps_lock, bool scroll_lock,
                unsigned char *output) {
    memset(output, 0, leds->total_size);
    if (leds->report_id) {
        *output++ = leds->report_id;
    }
    output[leds->num_lock / 8]    |= num_lock    ? (1 << leds->num_lock) : 0;
    output[leds->caps_lock / 8]   |= caps_lock   ? (1 << leds->caps_lock) : 0;
    output[leds->scroll_lock / 8] |= scroll_lock ? (1 << leds->scroll_lock) : 0;
}

void bounce(struct leds *leds, unsigned int rounds) {
    unsigned char output_buf[OUTPUT_BUF_SIZE];
    while (rounds--) {
        set_output(leds, true, false, false, output_buf);
        write_output_report(leds->handle, leds->ifnum, leds->report_id, output_buf, leds->total_size);
        usleep(BOUNCE_END_USLEEP);
        set_output(leds, false, true, false, output_buf);
        write_output_report(leds->handle, leds->ifnum, leds->report_id, output_buf, leds->total_size);
        usleep(BOUNCE_MIDDLE_USLEEP);
        set_output(leds, false, false, true, output_buf);
        write_output_report(leds->handle, leds->ifnum, leds->report_id, output_buf, leds->total_size);
        usleep(BOUNCE_END_USLEEP);
        set_output(leds, false, true, false, output_buf);
        write_output_report(leds->handle, leds->ifnum, leds->report_id, output_buf, leds->total_size);
        usleep(BOUNCE_MIDDLE_USLEEP);
    }
}

void glow(struct leds *leds, unsigned int rounds) {
    unsigned char on_buf[OUTPUT_BUF_SIZE];
    unsigned char off_buf[OUTPUT_BUF_SIZE];
    set_output(leds, true, true, true, on_buf);
    set_output(leds, false, false, false, off_buf);
    while (rounds--) {
        for (int i = 0; i < GLOW_STEPS * 2; i++) {
            double p = (i >= GLOW_STEPS ? (GLOW_STEPS * 2 - i) : i) / (double)GLOW_STEPS;
            write_output_report(leds->handle, leds->ifnum, leds->report_id, on_buf, leds->total_size);
            usleep(GLOW_PERIOD_USLEEP * p);
            write_output_report(leds->handle, leds->ifnum, leds->report_id, off_buf, leds->total_size);
            usleep(GLOW_PERIOD_USLEEP * (1 - p));
        }
    }
}

void randblink(struct leds *leds, unsigned int rounds) {
    unsigned char buf[OUTPUT_BUF_SIZE];
    bool num_lock;
    bool caps_lock;
    bool scroll_lock;
    while (rounds--) {
        num_lock = rand() > RAND_HALF;
        caps_lock = rand() > RAND_HALF;
        scroll_lock = rand() > RAND_HALF;
        set_output(leds, num_lock, caps_lock, scroll_lock, buf);
        write_output_report(leds->handle, leds->ifnum, leds->report_id, buf, leds->total_size);      
        usleep(RANDBLINK_USLEEP);
    }
}

void manipulate_leds(struct libusb_device *device) {
    libusb_device_handle *handle;

    printf("Bus %d, port %d, address: %d, speed: %s:",
           libusb_get_bus_number(device),
           libusb_get_port_number(device),
           libusb_get_device_address(device),
           speed[libusb_get_device_speed(device)]);

    struct libusb_device_descriptor dev_desc;
    libusb_get_device_descriptor(device, &dev_desc);
    if (dev_desc.bDeviceClass != LIBUSB_CLASS_PER_INTERFACE) {
        printf(" Not a HID device. Ignoring.\n");
        return;
    }
    putchar('\n');

    int err = libusb_open(device, &handle);
    if (err < 0) {
        LIBUSB_ERROR(err, "Couldn't open device: %s\n");
        return;
    }

    err = libusb_set_auto_detach_kernel_driver(handle, 1);
    if (err < 0) {
        LIBUSB_ERROR(err, "Couldn't enable automatic kernel driver detachment: %s\n");
        goto close;
    }

    struct libusb_config_descriptor *conf_desc;
    err = libusb_get_config_descriptor(device, 0, &conf_desc);
    if (err < 0) {
        LIBUSB_ERROR(err, "Couldn't read config descriptor: %s\n");
        goto close;
    }

    for (int i = 0; i < conf_desc->bNumInterfaces; i++) {
        printf(" Interface #%d", i);
        struct libusb_interface_descriptor if_desc = conf_desc->interface[i].altsetting[0];

        if (if_desc.bInterfaceClass != LIBUSB_CLASS_HID) {
            printf(" is not a HID interface. Ignoring.\n");
            continue;
        }

        printf(" is a HID interface\n");
        err = libusb_claim_interface(handle, i);
        if (err < 0) {
            LIBUSB_ERROR(err, "Cannot claim interface: %s\n");
            continue;
        }

        struct leds leds;
        if (find_leds(handle, i, &leds)) {
            bounce(&leds, 3);
            glow(&leds, 3);
            randblink(&leds, 30);
        }
        err = libusb_release_interface(handle, i);
        if (err < 0) {
            LIBUSB_ERROR(err, "Cannot release interface: %s\n");
        }

    }
    libusb_free_config_descriptor(conf_desc);

close:
    libusb_close(handle);
}

int main(void) {
    libusb_device **devices;

    srand(time(NULL));
    if (libusb_init(NULL) < 0) return EXIT_FAILURE;

    ssize_t cnt = libusb_get_device_list(NULL, &devices);

    for (ssize_t i = 0; i < cnt; i++) {
        manipulate_leds(devices[i]);
    }

    if (cnt >= 0) libusb_free_device_list(devices, 1);
    libusb_exit(NULL);
    return EXIT_SUCCESS;
}
