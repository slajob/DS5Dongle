#include "bt_link.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "ds5_proto.h"
#include "log.h"

namespace {

constexpr unsigned kBusBluetooth = 0x0005;

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> out;
    DIR *dir = ::opendir(path.c_str());
    if (!dir) return out;
    while (dirent *e = ::readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        out.emplace_back(e->d_name);
    }
    ::closedir(dir);
    return out;
}

std::string basename_of(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string real_path(const std::string &path) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return {};
    return buf;
}

bool write_file(const std::string &path, const std::string &value, std::string *error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    const ssize_t n = ::write(fd, value.data(), value.size());
    const int err = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        if (error) *error = "write " + path + ": " + std::strerror(err);
        return false;
    }
    return true;
}

class HidrawLink final : public BtLink {
public:
    HidrawLink(ControllerInfo info, int fd, std::vector<int> grabbed)
        : info_(std::move(info)), fd_(fd), grabbed_(std::move(grabbed)) {}

    ~HidrawLink() override {
        for (int g : grabbed_) {
            ::ioctl(g, EVIOCGRAB, 0);
            ::close(g);
        }
        if (fd_ >= 0) ::close(fd_);
    }

    int input_fd() const override { return fd_; }

    ssize_t read_input(uint8_t *buf, size_t cap) override {
        while (true) {
            const ssize_t n = ::read(fd_, buf, cap);
            if (n > 0) return n;
            if (n == 0) return -1;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            LOGI("%s: read failed (%s), controller disconnected", info_.hidraw_path.c_str(), std::strerror(errno));
            return -1;
        }
    }

    bool write_output(const uint8_t *data, size_t len) override {
        const ssize_t n = ::write(fd_, data, len);
        if (n != static_cast<ssize_t>(len)) {
            LOGW("%s: write of report 0x%02X (%zu bytes) failed: %s", info_.hidraw_path.c_str(), data[0], len,
                 n < 0 ? std::strerror(errno) : "short write");
            return false;
        }
        return true;
    }

    ssize_t get_feature(uint8_t report_id, uint8_t *buf, size_t cap) override {
        if (cap < 2) return -1;
        std::memset(buf, 0, cap);
        buf[0] = report_id;
        const int n = ::ioctl(fd_, HIDIOCGFEATURE(cap), buf);
        if (n < 0) {
            LOGW("%s: get feature 0x%02X failed: %s", info_.hidraw_path.c_str(), report_id, std::strerror(errno));
            return -1;
        }
        return n;
    }

    bool set_feature(const uint8_t *data, size_t len) override {
        std::vector<uint8_t> copy(data, data + len);
        const int n = ::ioctl(fd_, HIDIOCSFEATURE(len), copy.data());
        if (n < 0) {
            LOGW("%s: set feature 0x%02X failed: %s", info_.hidraw_path.c_str(), data[0], std::strerror(errno));
            return false;
        }
        return true;
    }

    const ControllerInfo &info() const override { return info_; }

private:
    ControllerInfo info_;
    int fd_;
    std::vector<int> grabbed_;
};

class FakeLink final : public BtLink {
public:
    FakeLink(ControllerInfo info, int intr, int ctl) : info_(std::move(info)), intr_(intr), ctl_(ctl) {}
    ~FakeLink() override {
        ::close(intr_);
        ::close(ctl_);
    }

    int input_fd() const override { return intr_; }

    ssize_t read_input(uint8_t *buf, size_t cap) override {
        const ssize_t n = ::recv(intr_, buf, cap, MSG_DONTWAIT);
        if (n > 0) return n;
        if (n == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }

    bool write_output(const uint8_t *data, size_t len) override {
        return ::send(intr_, data, len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
    }

    ssize_t get_feature(uint8_t report_id, uint8_t *buf, size_t cap) override {
        const uint8_t req[4] = {'G', report_id, static_cast<uint8_t>(cap & 0xFF), static_cast<uint8_t>(cap >> 8)};
        if (::send(ctl_, req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) return -1;
        const ssize_t n = ::recv(ctl_, buf, cap, 0);
        return n > 0 ? n : -1;
    }

    bool set_feature(const uint8_t *data, size_t len) override {
        std::vector<uint8_t> req(1 + len);
        req[0] = 'S';
        std::memcpy(req.data() + 1, data, len);
        if (::send(ctl_, req.data(), req.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(req.size())) return false;
        uint8_t status = 0;
        return ::recv(ctl_, &status, 1, 0) == 1 && status == 0;
    }

    const ControllerInfo &info() const override { return info_; }

private:
    ControllerInfo info_;
    int intr_;
    int ctl_;
};

int connect_seqpacket(const std::string &path, std::string *error) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "connect " + path + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

std::vector<ControllerInfo> scan_bluetooth_controllers() {
    std::vector<ControllerInfo> found;
    for (const auto &entry : list_dir("/sys/class/hidraw")) {
        const std::string base = "/sys/class/hidraw/" + entry;
        std::ifstream uevent(base + "/device/uevent");
        if (!uevent) continue;
        ControllerInfo info;
        unsigned bus = 0, vid = 0, pid = 0;
        bool have_id = false;
        std::string line;
        while (std::getline(uevent, line)) {
            if (line.rfind("HID_ID=", 0) == 0) {
                have_id = std::sscanf(line.c_str() + 7, "%x:%x:%x", &bus, &vid, &pid) == 3;
            } else if (line.rfind("HID_NAME=", 0) == 0) {
                info.name = line.substr(9);
            } else if (line.rfind("HID_UNIQ=", 0) == 0) {
                info.mac = line.substr(9);
            } else if (line.rfind("DRIVER=", 0) == 0) {
                info.driver = line.substr(7);
            }
        }
        if (!have_id || bus != kBusBluetooth || vid != ds5::kSonyVid) continue;
        if (pid != ds5::kDualSensePid && pid != ds5::kDualSenseEdgePid) continue;
        info.id = entry;
        info.hidraw_path = "/dev/" + entry;
        info.hid_device = basename_of(real_path(base + "/device"));
        info.product_id = static_cast<uint16_t>(pid);
        found.push_back(std::move(info));
    }
    return found;
}

std::vector<std::string> evdev_nodes(const ControllerInfo &info) {
    std::vector<std::string> nodes;
    const std::string input_dir = "/sys/class/hidraw/" + info.id + "/device/input";
    for (const auto &input : list_dir(input_dir)) {
        for (const auto &child : list_dir(input_dir + "/" + input)) {
            if (child.rfind("event", 0) == 0) nodes.push_back("/dev/input/" + child);
        }
    }
    return nodes;
}

std::unique_ptr<BtLink> open_hidraw_link(const ControllerInfo &info, bool grab_evdev, std::string *error) {
    const int fd = ::open(info.hidraw_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "open " + info.hidraw_path + ": " + std::strerror(errno);
        return nullptr;
    }
    std::vector<int> grabbed;
    if (grab_evdev) {
        for (const auto &node : evdev_nodes(info)) {
            const int efd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (efd < 0) continue;
            if (::ioctl(efd, EVIOCGRAB, 1) != 0) {
                LOGW("EVIOCGRAB %s: %s", node.c_str(), std::strerror(errno));
                ::close(efd);
                continue;
            }
            grabbed.push_back(efd);
        }
    }
    return std::make_unique<HidrawLink>(info, fd, std::move(grabbed));
}

std::unique_ptr<BtLink> open_fake_link(const std::string &path, const ControllerInfo &info, std::string *error) {
    const int intr = connect_seqpacket(path, error);
    if (intr < 0) return nullptr;
    const int ctl = connect_seqpacket(path + ".ctl", error);
    if (ctl < 0) {
        ::close(intr);
        return nullptr;
    }
    timeval tv{2, 0};
    ::setsockopt(ctl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return std::make_unique<FakeLink>(info, intr, ctl);
}

bool node_is_private(const std::string &dev_path) {
    struct stat st{};
    if (::stat(dev_path.c_str(), &st) != 0) return false;
    if (st.st_mode & 0077) return false;
    return ::getxattr(dev_path.c_str(), "system.posix_acl_access", nullptr, 0) < 0;
}

bool make_node_private(const std::string &dev_path) {
    bool ok = ::chmod(dev_path.c_str(), 0600) == 0;
    if (::removexattr(dev_path.c_str(), "system.posix_acl_access") != 0 && errno != ENODATA &&
        errno != ENOTSUP) {
        ok = false;
    }
    return ok;
}

bool rebind_hid_device(const ControllerInfo &info, std::string *error) {
    if (info.driver.empty() || info.hid_device.empty()) {
        if (error) *error = "controller has no bound HID driver";
        return false;
    }
    const std::string dir = "/sys/bus/hid/drivers/" + info.driver;
    if (!write_file(dir + "/unbind", info.hid_device, error)) return false;
    return write_file(dir + "/bind", info.hid_device, error);
}
