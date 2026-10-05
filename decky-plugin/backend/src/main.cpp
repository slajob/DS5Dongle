// ds5bridged: bridges Bluetooth DualSense controllers to virtual wired ones.
//
// The daemon is the Steam Deck counterpart of the DS5Dongle firmware: the
// controller stays paired with the Deck's own Bluetooth, and for each one a
// virtual USB DualSense (same descriptors as the Pico) is attached through
// vhci-hcd. Games then use their wired-controller code paths: HD haptics and
// the speaker over the USB audio interface, the mic, adaptive triggers and
// the light bar through USB output reports.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "bridge.h"
#include "bt_link.h"
#include "config.h"
#include "json.h"
#include "log.h"
#include "usbip.h"

#ifndef DS5BRIDGE_VERSION
#define DS5BRIDGE_VERSION "dev"
#endif

namespace {

int64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

struct Options {
    std::string socket_path = "/run/ds5bridge/ctl.sock";
    std::string flag_file = "/run/ds5bridge/active";
    bool hide = true;
    std::string usbip_connect;
    std::string fake_bt;
    uint16_t fake_pid = 0x0CE6;
    std::string fake_mac = "00:11:22:33:44:55";
    std::vector<std::pair<std::string, std::string>> settings;
};

void usage() {
    std::fprintf(stderr,
                 "ds5bridged %s - DualSense Bluetooth to virtual wired bridge\n\n"
                 "  --socket PATH        control socket (default /run/ds5bridge/ctl.sock)\n"
                 "  --flag-file PATH     file whose presence makes the udev rule hide BT controllers\n"
                 "  --no-hide            do not hide the Bluetooth controller from other programs\n"
                 "  --set KEY=VALUE      initial setting (repeatable)\n"
                 "  --verbose            debug logging\n"
                 "test options:\n"
                 "  --usbip-connect H:P  speak USB/IP to H:P instead of attaching to vhci-hcd\n"
                 "  --fake-bt PATH       use a fake controller on PATH / PATH.ctl (SOCK_SEQPACKET)\n"
                 "  --fake-pid PID       product id of the fake controller (default 0x0ce6)\n",
                 DS5BRIDGE_VERSION);
}

bool parse_args(int argc, char **argv, Options &opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string &out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "--socket") {
            if (!value(opts.socket_path)) return false;
        } else if (arg == "--flag-file") {
            if (!value(opts.flag_file)) return false;
        } else if (arg == "--no-hide") {
            opts.hide = false;
        } else if (arg == "--verbose") {
            logging::threshold() = logging::kDebug;
        } else if (arg == "--usbip-connect") {
            if (!value(opts.usbip_connect)) return false;
        } else if (arg == "--fake-bt") {
            if (!value(opts.fake_bt)) return false;
        } else if (arg == "--fake-pid") {
            if (!value(v)) return false;
            opts.fake_pid = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 0));
        } else if (arg == "--set") {
            if (!value(v)) return false;
            const auto eq = v.find('=');
            if (eq == std::string::npos) return false;
            opts.settings.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else {
            return false;
        }
    }
    return true;
}

bool make_dirs_for(const std::string &path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string dir = path.substr(0, slash);
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return true;
}

class Daemon {
public:
    explicit Daemon(Options opts) : opts_(std::move(opts)) {}

    int run();

private:
    struct Client {
        int fd = -1;
        std::string in;
    };
    struct Registration {
        int bt_fd = -1;
        int worker_fd = -1;
        int usb_fd = -1;
        uint64_t usb_generation = ~0ull;
        bool usb_writable = false;
    };
    enum class Kind { kSignal, kTick, kHousekeeping, kListener, kClient, kBt, kWorker, kUsb };
    struct Source {
        Kind kind;
        std::string key; // bridge key or empty
    };

    bool setup(std::string *error);
    void shutdown();
    void watch(int fd, uint32_t events, Kind kind, const std::string &key = {});
    void unwatch(int fd);
    void sync_bridge_fds(const std::string &key);
    void remove_bridge(const std::string &key);
    void discover(int64_t now);
    void housekeeping(int64_t now);
    void handle_client(int fd);
    std::string handle_command(const std::string &line);
    std::string status_json();
    void set_flag_file(bool present);
    void restore_hidden_controllers();

    Options opts_;
    BridgeConfig config_;
    int epfd_ = -1;
    int sigfd_ = -1;
    int tickfd_ = -1;
    int housefd_ = -1;
    int listenfd_ = -1;
    bool running_ = true;
    std::map<int, Source> sources_;
    std::map<std::string, std::unique_ptr<Bridge>> bridges_; // key: MAC (or hidraw id)
    std::map<std::string, Registration> registrations_;
    std::map<int, Client> clients_;
    std::set<std::string> rebound_;  // controllers we re-bound to drop other openers
    std::map<std::string, std::string> failed_; // key -> last error
    int64_t next_fake_attempt_ = 0;
};

void Daemon::watch(int fd, uint32_t events, Kind kind, const std::string &key) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        LOGE("epoll_ctl ADD %d: %s", fd, std::strerror(errno));
        return;
    }
    sources_[fd] = Source{kind, key};
}

void Daemon::unwatch(int fd) {
    if (fd < 0) return;
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    sources_.erase(fd);
}

bool Daemon::setup(std::string *error) {
    for (const auto &[key, value] : opts_.settings) {
        std::string err;
        if (!config_.set(key, value, &err)) LOGW("ignoring --set %s: %s", key.c_str(), err.c_str());
    }

    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    ::sigprocmask(SIG_BLOCK, &mask, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    sigfd_ = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    watch(sigfd_, EPOLLIN, Kind::kSignal);

    // 1 ms clock driving the isochronous endpoints (a full-speed USB frame).
    tickfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    itimerspec tick{};
    tick.it_interval.tv_nsec = 1'000'000;
    tick.it_value.tv_nsec = 1'000'000;
    ::timerfd_settime(tickfd_, 0, &tick, nullptr);
    watch(tickfd_, EPOLLIN, Kind::kTick);

    housefd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    itimerspec house{};
    house.it_interval.tv_nsec = 500'000'000;
    house.it_value.tv_nsec = 1'000'000;
    ::timerfd_settime(housefd_, 0, &house, nullptr);
    watch(housefd_, EPOLLIN, Kind::kHousekeeping);

    if (!make_dirs_for(opts_.socket_path)) {
        *error = "cannot create directory for " + opts_.socket_path;
        return false;
    }
    ::unlink(opts_.socket_path.c_str());
    listenfd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, opts_.socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listenfd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listenfd_, 4) != 0) {
        *error = "cannot listen on " + opts_.socket_path + ": " + std::strerror(errno);
        return false;
    }
    ::chmod(opts_.socket_path.c_str(), 0600);
    watch(listenfd_, EPOLLIN, Kind::kListener);

    if (opts_.hide && opts_.fake_bt.empty()) set_flag_file(true);
    // Audio pacing benefits from a higher priority, like an audio server.
    ::setpriority(PRIO_PROCESS, 0, -11);
    return true;
}

void Daemon::set_flag_file(bool present) {
    if (present) {
        make_dirs_for(opts_.flag_file);
        const int fd = ::open(opts_.flag_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) ::close(fd);
    } else {
        ::unlink(opts_.flag_file.c_str());
    }
}

void Daemon::sync_bridge_fds(const std::string &key) {
    auto &reg = registrations_[key];
    Bridge &bridge = *bridges_.at(key);
    if (reg.bt_fd < 0) {
        reg.bt_fd = bridge.bt_fd();
        watch(reg.bt_fd, EPOLLIN, Kind::kBt, key);
    }
    if (reg.worker_fd < 0) {
        reg.worker_fd = bridge.worker_fd();
        watch(reg.worker_fd, EPOLLIN, Kind::kWorker, key);
    }
    if (reg.usb_generation != bridge.fd_generation() || reg.usb_fd != bridge.usb_fd()) {
        if (reg.usb_fd >= 0) unwatch(reg.usb_fd);
        reg.usb_fd = bridge.usb_fd();
        reg.usb_generation = bridge.fd_generation();
        reg.usb_writable = false;
        if (reg.usb_fd >= 0) watch(reg.usb_fd, EPOLLIN, Kind::kUsb, key);
    }
    const bool want_write = bridge.usb_wants_write();
    if (reg.usb_fd >= 0 && want_write != reg.usb_writable) {
        epoll_event ev{};
        ev.events = EPOLLIN | (want_write ? EPOLLOUT : 0u);
        ev.data.fd = reg.usb_fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, reg.usb_fd, &ev);
        reg.usb_writable = want_write;
    }
}

void Daemon::remove_bridge(const std::string &key) {
    auto reg = registrations_.find(key);
    if (reg != registrations_.end()) {
        unwatch(reg->second.bt_fd);
        unwatch(reg->second.worker_fd);
        unwatch(reg->second.usb_fd);
        registrations_.erase(reg);
    }
    auto it = bridges_.find(key);
    if (it != bridges_.end()) {
        LOGI("controller %s disconnected", key.c_str());
        bridges_.erase(it); // detaches the virtual controller
    }
}

void Daemon::discover(int64_t now) {
    if (!opts_.fake_bt.empty()) {
        if (!bridges_.empty() || now < next_fake_attempt_) return;
        next_fake_attempt_ = now + 1'000'000;
        ControllerInfo info;
        info.id = "fake";
        info.hidraw_path = opts_.fake_bt;
        info.product_id = opts_.fake_pid;
        info.name = info.product_id == ds5::kDualSenseEdgePid ? "DualSense Edge Wireless Controller"
                                                              : "DualSense Wireless Controller";
        info.mac = opts_.fake_mac;
        std::string error;
        auto link = open_fake_link(opts_.fake_bt, info, &error);
        if (!link) {
            LOGD("fake controller not available: %s", error.c_str());
            return;
        }
        BridgeOptions bridge_opts;
        bridge_opts.usbip_connect = opts_.usbip_connect;
        bridge_opts.allow_bt_disconnect = false;
        auto bridge = std::make_unique<Bridge>(std::move(link), config_, bridge_opts);
        if (!bridge->init(&error)) {
            LOGE("bridge init failed: %s", error.c_str());
            return;
        }
        bridges_[info.mac] = std::move(bridge);
        sync_bridge_fds(info.mac);
        return;
    }

    for (auto &info : scan_bluetooth_controllers()) {
        const std::string key = info.mac.empty() ? info.id : info.mac;
        if (bridges_.count(key)) continue;

        if (opts_.hide && !node_is_private(info.hidraw_path)) {
            if (!rebound_.count(key)) {
                // Someone (Steam) may already have the node open. Re-binding the
                // driver recreates the nodes, which udev now creates private
                // because the flag file exists.
                rebound_.insert(key);
                std::string error;
                if (rebind_hid_device(info, &error)) {
                    LOGI("re-bound %s so it can be hidden from other programs", info.hid_device.c_str());
                    continue; // pick it up on the next scan
                }
                LOGW("could not re-bind %s: %s", info.hid_device.c_str(), error.c_str());
            }
            make_node_private(info.hidraw_path);
            for (const auto &node : evdev_nodes(info)) make_node_private(node);
        }

        std::string error;
        auto link = open_hidraw_link(info, opts_.hide, &error);
        if (!link) {
            if (failed_[key] != error) LOGE("cannot open %s: %s", info.hidraw_path.c_str(), error.c_str());
            failed_[key] = error;
            continue;
        }
        failed_.erase(key);
        rebound_.insert(key);
        BridgeOptions bridge_opts;
        bridge_opts.usbip_connect = opts_.usbip_connect;
        auto bridge = std::make_unique<Bridge>(std::move(link), config_, bridge_opts);
        if (!bridge->init(&error)) {
            LOGE("bridge init failed: %s", error.c_str());
            continue;
        }
        bridges_[key] = std::move(bridge);
        sync_bridge_fds(key);
    }
    (void) now;
}

void Daemon::housekeeping(int64_t now) {
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    discover(now);
    for (auto &[key, bridge] : bridges_) {
        bridge->housekeeping(now);
        sync_bridge_fds(key);
    }
}

std::string Daemon::status_json() {
    std::string vhci_error;
    const bool vhci = !opts_.usbip_connect.empty() || usbip::vhci_available(&vhci_error);
    std::string out = "{\"version\":" + json_string(DS5BRIDGE_VERSION) + ",\"pid\":" + std::to_string(::getpid()) +
                      ",\"vhci\":{\"available\":" + json_bool(vhci) + ",\"error\":" + json_string(vhci_error) +
                      "},\"hiding\":" + json_bool(opts_.hide) + ",\"controllers\":[";
    bool first = true;
    for (const auto &[key, bridge] : bridges_) {
        if (!first) out += ",";
        first = false;
        out += bridge->status_json();
    }
    out += "],\"errors\":[";
    first = true;
    for (const auto &[key, error] : failed_) {
        if (!first) out += ",";
        first = false;
        out += json_string(key + ": " + error);
    }
    out += "],\"config\":" + config_.to_json() + "}";
    return out;
}

std::string Daemon::handle_command(const std::string &line) {
    std::string cmd = line;
    std::string rest;
    const auto space = line.find(' ');
    if (space != std::string::npos) {
        cmd = line.substr(0, space);
        rest = line.substr(space + 1);
    }
    if (cmd == "PING") return "PONG";
    if (cmd == "STATUS") return status_json();
    if (cmd == "GET_CONFIG") return config_.to_json();
    if (cmd == "SET") {
        const auto sp = rest.find(' ');
        if (sp == std::string::npos) return "ERR usage: SET <key> <value>";
        BridgeConfig updated = config_;
        std::string error;
        if (!updated.set(rest.substr(0, sp), rest.substr(sp + 1), &error)) return "ERR " + error;
        const BridgeConfig previous = config_;
        config_ = updated;
        for (auto &[key, bridge] : bridges_) {
            bridge->config_changed(previous);
            sync_bridge_fds(key);
        }
        return "OK";
    }
    if (cmd == "REATTACH") {
        for (auto &[key, bridge] : bridges_) {
            bridge->reattach();
            sync_bridge_fds(key);
        }
        return "OK";
    }
    if (cmd == "DISCONNECT") {
        for (auto &[key, bridge] : bridges_) bridge->disconnect_controller();
        return "OK";
    }
    if (cmd == "QUIT") {
        running_ = false;
        return "OK";
    }
    return "ERR unknown command";
}

void Daemon::handle_client(int fd) {
    auto &client = clients_[fd];
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            client.in.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        unwatch(fd);
        ::close(fd);
        clients_.erase(fd);
        return;
    }
    size_t nl;
    while ((nl = client.in.find('\n')) != std::string::npos) {
        std::string line = client.in.substr(0, nl);
        client.in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string reply = handle_command(line) + "\n";
        // Replies are small; a blocking send keeps this simple.
        ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    }
}

void Daemon::restore_hidden_controllers() {
    if (!opts_.hide || !opts_.fake_bt.empty()) return;
    // Re-binding makes udev recreate the nodes with their normal permissions
    // (the flag file is gone), so Steam can use the controller over plain
    // Bluetooth again without reconnecting it.
    for (const auto &info : scan_bluetooth_controllers()) {
        if (!node_is_private(info.hidraw_path)) continue;
        std::string error;
        if (!rebind_hid_device(info, &error)) LOGW("could not restore %s: %s", info.hid_device.c_str(), error.c_str());
    }
}

void Daemon::shutdown() {
    LOGI("shutting down");
    for (auto &client : clients_) ::close(client.first);
    clients_.clear();
    std::vector<std::string> keys;
    for (const auto &entry : bridges_) keys.push_back(entry.first);
    for (const auto &key : keys) remove_bridge(key);
    set_flag_file(false);
    restore_hidden_controllers();
    if (listenfd_ >= 0) {
        ::close(listenfd_);
        ::unlink(opts_.socket_path.c_str());
    }
}

int Daemon::run() {
    std::string error;
    if (!setup(&error)) {
        LOGE("%s", error.c_str());
        return 1;
    }
    LOGI("ds5bridged %s started%s", DS5BRIDGE_VERSION, opts_.fake_bt.empty() ? "" : " (fake controller)");

    epoll_event events[32];
    while (running_) {
        const int n = ::epoll_wait(epfd_, events, 32, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("epoll_wait: %s", std::strerror(errno));
            break;
        }
        const int64_t now = now_us();
        std::set<std::string> dead;
        std::set<std::string> touched;
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const auto src = sources_.find(fd);
            if (src == sources_.end()) continue;
            const Source source = src->second;
            const uint32_t ev = events[i].events;
            switch (source.kind) {
                case Kind::kSignal: {
                    signalfd_siginfo info{};
                    (void) !::read(fd, &info, sizeof(info));
                    LOGI("received signal %u", info.ssi_signo);
                    running_ = false;
                    break;
                }
                case Kind::kTick: {
                    uint64_t expirations = 0;
                    (void) !::read(fd, &expirations, sizeof(expirations));
                    for (auto &[key, bridge] : bridges_) {
                        bridge->tick(now);
                        touched.insert(key);
                    }
                    break;
                }
                case Kind::kHousekeeping: {
                    uint64_t expirations = 0;
                    (void) !::read(fd, &expirations, sizeof(expirations));
                    housekeeping(now);
                    break;
                }
                case Kind::kListener: {
                    while (true) {
                        const int cfd = ::accept4(listenfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                        if (cfd < 0) break;
                        clients_[cfd] = Client{cfd, {}};
                        watch(cfd, EPOLLIN, Kind::kClient);
                    }
                    break;
                }
                case Kind::kClient:
                    handle_client(fd);
                    break;
                case Kind::kBt: {
                    auto it = bridges_.find(source.key);
                    if (it == bridges_.end()) break;
                    if (!it->second->on_bt_readable(now) || (ev & (EPOLLHUP | EPOLLERR))) dead.insert(source.key);
                    touched.insert(source.key);
                    break;
                }
                case Kind::kWorker: {
                    auto it = bridges_.find(source.key);
                    if (it == bridges_.end()) break;
                    it->second->on_worker_event();
                    it->second->housekeeping(now);
                    touched.insert(source.key);
                    break;
                }
                case Kind::kUsb: {
                    auto it = bridges_.find(source.key);
                    if (it == bridges_.end()) break;
                    const bool readable = ev & (EPOLLIN | EPOLLHUP | EPOLLERR);
                    it->second->on_usb_event(readable, ev & EPOLLOUT, now);
                    touched.insert(source.key);
                    break;
                }
            }
        }
        for (const auto &key : dead) remove_bridge(key);
        for (const auto &key : touched) {
            if (bridges_.count(key)) sync_bridge_fds(key);
        }
    }
    shutdown();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        usage();
        return 2;
    }
    Daemon daemon(std::move(opts));
    return daemon.run();
}
