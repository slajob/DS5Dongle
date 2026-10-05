"""DS5 Bridge - Decky Loader backend.

Supervises the native ds5bridged daemon (bin/ds5bridged), which turns a
DualSense paired with the Steam Deck over Bluetooth into a virtual *wired*
DualSense - the same thing the DS5Dongle Pico firmware does, without the Pico.

This file only manages the daemon: prerequisites (vhci-hcd module, udev rule),
settings persistence, start/stop/restart and talking to its control socket.
"""
import asyncio
import json
import os
import time

import decky  # provided by Decky Loader

SOCKET_PATH = "/run/ds5bridge/ctl.sock"
FLAG_FILE = "/run/ds5bridge/active"
UDEV_RULE_PATH = "/etc/udev/rules.d/72-ds5bridge.rules"
VHCI_ATTACH = "/sys/devices/platform/vhci_hcd.0/attach"

# While the daemon runs (FLAG_FILE exists) the raw Bluetooth controller is made
# root-only so Steam and games only see the bridged, wired controller. Ordered
# after Steam's and systemd's uaccess rules (60-*, 70-*) and before
# 73-seat-late.rules applies the uaccess ACLs. When the bridge is off the rule
# does nothing.
UDEV_RULE = """# Installed by the DS5 Bridge Decky plugin. Safe to delete.
ACTION!="remove", SUBSYSTEM=="hidraw", KERNELS=="0005:054C:0CE6.*", TEST=="{flag}", MODE="0600", GROUP="root", TAG-="uaccess"
ACTION!="remove", SUBSYSTEM=="hidraw", KERNELS=="0005:054C:0DF2.*", TEST=="{flag}", MODE="0600", GROUP="root", TAG-="uaccess"
ACTION!="remove", SUBSYSTEM=="input", KERNELS=="0005:054C:0CE6.*", TEST=="{flag}", MODE="0600", GROUP="root", TAG-="uaccess"
ACTION!="remove", SUBSYSTEM=="input", KERNELS=="0005:054C:0DF2.*", TEST=="{flag}", MODE="0600", GROUP="root", TAG-="uaccess"
""".format(flag=FLAG_FILE)

# Same names, ranges and defaults as the firmware's web config.
DEFAULT_SETTINGS = {
    "enabled": True,
    "haptics_gain": 1.0,
    "speaker_gain": 2,
    "speaker_select": 0,
    "mic_select": 0,
    "trigger_reduce": 0,
    "lock_volume": 0,
    "audio_buffer_length": 48,
    "controller_mode": 2,
    "inactive_time": 30,
    "lightbar_color": "#ffd700",
}
DAEMON_KEYS = [k for k in DEFAULT_SETTINGS if k != "enabled"]
# Valid ranges (the daemon checks them too; this covers changes made while it is off).
RANGES = {
    "haptics_gain": (1.0, 2.0),
    "speaker_gain": (0, 7),
    "speaker_select": (0, 3),
    "mic_select": (0, 3),
    "trigger_reduce": (0, 10),
    "lock_volume": (0, 1),
    "audio_buffer_length": (16, 128),
    "controller_mode": (0, 2),
    "inactive_time": (0, 60),
}


def _validate(key: str, value):
    """Returns the normalised value or raises ValueError."""
    if key == "lightbar_color":
        text = str(value).lstrip("#")
        if len(text) != 6:
            raise ValueError("lightbar_color must look like #RRGGBB")
        int(text, 16)
        return "#" + text.lower()
    low, high = RANGES[key]
    number = float(value) if isinstance(low, float) else int(value)
    if not low <= number <= high:
        raise ValueError(f"{key} must be in [{low}, {high}]")
    return number


def _clean_env() -> dict:
    """Environment for system tools: Decky's bundled Python sets library paths
    that break host binaries."""
    env = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH"):
        env.pop(key, None)
    env["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    return env


async def _run(*cmd: str, timeout: float = 15.0) -> tuple[int, str]:
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, env=_clean_env())
        out, _ = await asyncio.wait_for(proc.communicate(), timeout)
        return proc.returncode, out.decode(errors="replace").strip()
    except (OSError, asyncio.TimeoutError) as exc:
        return -1, str(exc)


class Plugin:
    def __init__(self):
        self.settings = dict(DEFAULT_SETTINGS)
        self.proc = None
        self.supervisor = None
        self.stopping = False
        self.last_error = ""
        self.restarts = []
        self.log_file = None

    # ------------------------------------------------------------------ paths
    @property
    def settings_path(self) -> str:
        return os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "settings.json")

    @property
    def daemon_path(self) -> str:
        return os.path.join(decky.DECKY_PLUGIN_DIR, "bin", "ds5bridged")

    @property
    def daemon_log_path(self) -> str:
        return os.path.join(decky.DECKY_PLUGIN_LOG_DIR, "ds5bridged.log")

    # --------------------------------------------------------------- settings
    def _load_settings(self) -> None:
        try:
            with open(self.settings_path) as f:
                stored = json.load(f)
            for key, value in stored.items():
                if key == "enabled":
                    self.settings[key] = bool(value)
                elif key in DAEMON_KEYS:
                    try:
                        self.settings[key] = _validate(key, value)
                    except (TypeError, ValueError):
                        pass
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as exc:
            decky.logger.warning(f"could not read settings: {exc}")

    def _save_settings(self) -> None:
        os.makedirs(os.path.dirname(self.settings_path), exist_ok=True)
        tmp = self.settings_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.settings, f, indent=2)
        os.replace(tmp, self.settings_path)

    # ---------------------------------------------------------- prerequisites
    async def _prepare_system(self) -> str:
        """Loads vhci-hcd and installs the udev rule. Returns an error or ''."""
        if not os.path.exists(VHCI_ATTACH):
            code, out = await _run("modprobe", "vhci-hcd")
            if code != 0 or not os.path.exists(VHCI_ATTACH):
                return ("The SteamOS kernel did not load the vhci-hcd (USB/IP) module, which the bridge "
                        f"needs to create the virtual wired controller. modprobe said: {out or code}")
        try:
            current = open(UDEV_RULE_PATH).read() if os.path.exists(UDEV_RULE_PATH) else ""
            if current != UDEV_RULE:
                os.makedirs(os.path.dirname(UDEV_RULE_PATH), exist_ok=True)
                with open(UDEV_RULE_PATH, "w") as f:
                    f.write(UDEV_RULE)
                await _run("udevadm", "control", "--reload-rules")
        except OSError as exc:
            # Not fatal: the daemon still restricts the nodes itself.
            decky.logger.warning(f"could not install udev rule: {exc}")
        return ""

    # ----------------------------------------------------------------- daemon
    def _daemon_args(self) -> list:
        args = [self.daemon_path, "--socket", SOCKET_PATH, "--flag-file", FLAG_FILE]
        for key in DAEMON_KEYS:
            args += ["--set", f"{key}={self.settings[key]}"]
        return args

    async def _spawn(self) -> bool:
        if not os.path.exists(self.daemon_path):
            self.last_error = f"daemon binary missing: {self.daemon_path}"
            return False
        os.chmod(self.daemon_path, 0o755)  # zip extraction may drop the exec bit
        error = await self._prepare_system()
        if error:
            self.last_error = error
            decky.logger.error(error)
            return False
        os.makedirs(decky.DECKY_PLUGIN_LOG_DIR, exist_ok=True)
        if os.path.exists(self.daemon_log_path) and os.path.getsize(self.daemon_log_path) > 2 * 1024 * 1024:
            os.replace(self.daemon_log_path, self.daemon_log_path + ".old")
        self.log_file = open(self.daemon_log_path, "a")
        self.proc = await asyncio.create_subprocess_exec(
            *self._daemon_args(), stdout=self.log_file, stderr=self.log_file, env=_clean_env())
        decky.logger.info(f"started ds5bridged (pid {self.proc.pid})")
        self.last_error = ""
        return True

    async def _unhide_controllers(self) -> None:
        """Gives the raw Bluetooth controller back to SteamOS. The daemon does
        this itself on a clean exit; this covers crashes."""
        if not os.path.exists(FLAG_FILE):
            return
        try:
            os.remove(FLAG_FILE)
        except OSError:
            pass
        # Re-run the udev rules: without the flag file the normal permissions
        # and uaccess ACLs come back.
        for subsystem in ("hidraw", "input"):
            await _run("udevadm", "trigger", "--action=change", f"--subsystem-match={subsystem}")

    async def _supervise(self) -> None:
        while not self.stopping:
            if self.proc is None:
                if not await self._spawn():
                    await asyncio.sleep(10)
                    continue
            code = await self.proc.wait()
            self.proc = None
            if self.log_file:
                self.log_file.close()
                self.log_file = None
            await self._unhide_controllers()
            if self.stopping:
                break
            now = time.monotonic()
            self.restarts = [t for t in self.restarts if now - t < 60] + [now]
            self.last_error = f"ds5bridged exited with code {code}"
            decky.logger.warning(self.last_error)
            if len(self.restarts) > 5:
                self.last_error += " repeatedly; giving up. See the log for details."
                break
            await asyncio.sleep(2)

    async def _start(self) -> None:
        if self.supervisor and not self.supervisor.done():
            return
        self.stopping = False
        self.restarts = []
        self.supervisor = asyncio.get_event_loop().create_task(self._supervise())

    async def _stop(self) -> None:
        self.stopping = True
        if self.proc and self.proc.returncode is None:
            self.proc.terminate()
            try:
                await asyncio.wait_for(self.proc.wait(), 8)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
        if self.supervisor:
            try:
                await asyncio.wait_for(self.supervisor, 10)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self.supervisor.cancel()
            self.supervisor = None
        self.proc = None
        await self._unhide_controllers()

    async def _command(self, line: str, timeout: float = 2.0) -> str:
        reader, writer = await asyncio.wait_for(asyncio.open_unix_connection(SOCKET_PATH), timeout)
        try:
            writer.write(line.encode() + b"\n")
            await writer.drain()
            reply = await asyncio.wait_for(reader.readline(), timeout)
            return reply.decode().strip()
        finally:
            writer.close()

    # ------------------------------------------------------- frontend methods
    async def get_state(self) -> dict:
        running = self.proc is not None and self.proc.returncode is None
        daemon = None
        if running:
            try:
                daemon = json.loads(await self._command("STATUS"))
            except (OSError, ValueError, asyncio.TimeoutError):
                daemon = None
        return {
            "settings": self.settings,
            "running": running,
            "error": self.last_error,
            "daemon": daemon,
        }

    async def set_enabled(self, enabled: bool) -> dict:
        self.settings["enabled"] = bool(enabled)
        self._save_settings()
        if enabled:
            await self._start()
        else:
            await self._stop()
            self.last_error = ""
        return await self.get_state()

    async def set_setting(self, key: str, value) -> dict:
        if key not in DAEMON_KEYS:
            return {"ok": False, "error": f"unknown setting {key}"}
        try:
            value = _validate(key, value)
        except (TypeError, ValueError) as exc:
            return {"ok": False, "error": str(exc)}
        if self.proc is not None and self.proc.returncode is None:
            try:
                reply = await self._command(f"SET {key} {value}")
            except (OSError, asyncio.TimeoutError) as exc:
                reply = f"ERR {exc}"
            if reply != "OK":
                return {"ok": False, "error": reply[4:] if reply.startswith("ERR ") else reply}
        self.settings[key] = value
        self._save_settings()
        return {"ok": True}

    async def reset_settings(self) -> dict:
        enabled = self.settings.get("enabled", True)
        for key in DAEMON_KEYS:
            await self.set_setting(key, DEFAULT_SETTINGS[key])
        self.settings["enabled"] = enabled
        self._save_settings()
        return await self.get_state()

    async def reattach(self) -> bool:
        try:
            return await self._command("REATTACH") == "OK"
        except (OSError, asyncio.TimeoutError):
            return False

    async def disconnect_controller(self) -> bool:
        try:
            return await self._command("DISCONNECT") == "OK"
        except (OSError, asyncio.TimeoutError):
            return False

    async def get_log(self, lines: int = 80) -> str:
        try:
            with open(self.daemon_log_path, errors="replace") as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return ""

    # ------------------------------------------------------------- lifecycle
    async def _main(self):
        self._load_settings()
        if self.settings.get("enabled", True):
            await self._start()

    async def _unload(self):
        await self._stop()

    async def _uninstall(self):
        await self._stop()
        try:
            os.remove(UDEV_RULE_PATH)
            await _run("udevadm", "control", "--reload-rules")
        except FileNotFoundError:
            pass

    async def _migration(self):
        pass
