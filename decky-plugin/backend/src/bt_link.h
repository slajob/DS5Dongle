// Link to a DualSense that is paired with the Steam Deck over Bluetooth.
//
// In the firmware the Pico owns the Bluetooth stack (BTstack + L2CAP). On the
// Deck, BlueZ and the kernel's HIDP layer own it, and the controller shows up
// as /dev/hidrawN: read() returns input reports (0x31...), write() sends output
// reports on the HID interrupt channel (the kernel adds the 0xA2 header) and
// the feature ioctls map to GET/SET_REPORT on the control channel. That is the
// full set of primitives the firmware uses, so the protocol logic ports 1:1.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

struct ControllerInfo {
    std::string id;          // stable key: hidraw sysfs name (e.g. "hidraw5")
    std::string hidraw_path; // /dev/hidrawN
    std::string hid_device;  // HID sysfs device name, e.g. "0005:054C:0CE6.0003"
    std::string driver;      // bound HID driver, e.g. "playstation"
    std::string name;
    std::string mac;
    uint16_t product_id = 0;
};

class BtLink {
public:
    virtual ~BtLink() = default;
    virtual int input_fd() const = 0;
    // Non-blocking. Returns report length, 0 when nothing is pending, -1 when
    // the controller is gone.
    virtual ssize_t read_input(uint8_t *buf, size_t cap) = 0;
    // Writes one output report (report id first, CRC already filled in).
    virtual bool write_output(const uint8_t *data, size_t len) = 0;
    // Blocking; only called from the feature worker thread. Returns the report
    // including its id, or -1.
    virtual ssize_t get_feature(uint8_t report_id, uint8_t *buf, size_t cap) = 0;
    virtual bool set_feature(const uint8_t *data, size_t len) = 0;
    virtual const ControllerInfo &info() const = 0;
};

// Lists DualSense / DualSense Edge controllers connected over Bluetooth.
std::vector<ControllerInfo> scan_bluetooth_controllers();

// Opens /dev/hidrawN and grabs the controller's evdev nodes so nothing else
// sees its input while it is bridged.
std::unique_ptr<BtLink> open_hidraw_link(const ControllerInfo &info, bool grab_evdev, std::string *error);

// Test double: talks to a fake controller over two SOCK_SEQPACKET sockets
// (<path> for interrupt reports, <path>.ctl for feature requests).
std::unique_ptr<BtLink> open_fake_link(const std::string &path, const ControllerInfo &info, std::string *error);

// --- hiding the raw Bluetooth controller from Steam / games ---------------
// True when nobody but root can open the node.
bool node_is_private(const std::string &dev_path);
// chmod 0600 and drop the uaccess ACL.
bool make_node_private(const std::string &dev_path);
// Unbinds and rebinds the HID driver so the device nodes are recreated (and
// every process that had them open loses them), letting udev re-evaluate its
// rules.
bool rebind_hid_device(const ControllerInfo &info, std::string *error);
// Evdev nodes that belong to the controller's HID device.
std::vector<std::string> evdev_nodes(const ControllerInfo &info);
