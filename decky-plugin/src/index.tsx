import {
  ButtonItem,
  ConfirmModal,
  DropdownItem,
  Field,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  showModal,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin, toaster, useQuickAccessVisible } from "@decky/api";
import { useCallback, useEffect, useState } from "react";
import { FaGamepad } from "react-icons/fa";

// ----------------------------------------------------------------------------
// Backend API (main.py)
// ----------------------------------------------------------------------------
interface Settings {
  enabled: boolean;
  haptics_gain: number;
  speaker_gain: number;
  speaker_select: number;
  mic_select: number;
  trigger_reduce: number;
  lock_volume: number;
  audio_buffer_length: number;
  controller_mode: number;
  inactive_time: number;
  lightbar_color: string;
}

interface ControllerStatus {
  id: string;
  name: string;
  mac: string;
  type: string;
  presented_as: string;
  battery: number;
  charging: boolean;
  headphones: boolean;
  usb: "starting" | "attached" | "connected" | "retrying" | "failed";
  usb_error: string;
  speaker_stream: boolean;
  mic_stream: boolean;
}

interface DaemonStatus {
  version: string;
  vhci: { available: boolean; error: string };
  controllers: ControllerStatus[];
  errors: string[];
}

interface State {
  settings: Settings;
  running: boolean;
  error: string;
  daemon: DaemonStatus | null;
}

type SettingKey = Exclude<keyof Settings, "enabled">;

const getState = callable<[], State>("get_state");
const setEnabled = callable<[enabled: boolean], State>("set_enabled");
const setSetting = callable<[key: string, value: number | string], { ok: boolean; error?: string }>("set_setting");
const resetSettings = callable<[], State>("reset_settings");
const reattach = callable<[], boolean>("reattach");
const disconnectController = callable<[], boolean>("disconnect_controller");
const getLog = callable<[lines: number], string>("get_log");

// ----------------------------------------------------------------------------
// Options (same meaning as the DS5Dongle web config)
// ----------------------------------------------------------------------------
const SPEAKER_OPTIONS = [
  { data: 0, label: "Auto (headset when plugged in)" },
  { data: 1, label: "Controller speaker" },
  { data: 2, label: "Headset jack" },
  { data: 3, label: "Off (haptics only)" },
];

const MIC_OPTIONS = [
  { data: 0, label: "Auto" },
  { data: 1, label: "Controller microphone" },
  { data: 2, label: "Headset microphone" },
  { data: 3, label: "Off" },
];

const MODE_OPTIONS = [
  { data: 2, label: "Auto (same as controller)" },
  { data: 0, label: "DualSense" },
  { data: 1, label: "DualSense Edge" },
];

const LIGHTBAR_OPTIONS = [
  { data: "#ffd700", label: "Yellow (DS5Dongle default)" },
  { data: "#0040ff", label: "Blue" },
  { data: "#ff0000", label: "Red" },
  { data: "#00ff40", label: "Green" },
  { data: "#a000ff", label: "Purple" },
  { data: "#ffffff", label: "White" },
  { data: "#000000", label: "Off" },
];

// ----------------------------------------------------------------------------
// Components
// ----------------------------------------------------------------------------
function usbDescription(c: ControllerStatus): string {
  switch (c.usb) {
    case "connected":
      return `Active: games see a wired ${c.presented_as || "DualSense"}`;
    case "attached":
      return "Virtual wired controller attached, waiting for SteamOS";
    case "starting":
      return "Reading controller information…";
    case "retrying":
      return `Retrying: ${c.usb_error || "attaching the virtual controller"}`;
    case "failed":
      return `Failed: ${c.usb_error}`;
  }
}

function ControllerRow({ c }: { c: ControllerStatus }) {
  const battery = c.battery >= 0 ? `${c.battery}%${c.charging ? " (charging)" : ""}` : "battery unknown";
  const streams = [
    c.speaker_stream ? "haptics/audio streaming" : "haptics/audio idle",
    c.mic_stream ? "mic in use" : "mic idle",
    c.headphones ? "headset plugged in" : null,
  ]
    .filter(Boolean)
    .join(" · ");
  return (
    <PanelSectionRow>
      <Field
        label={c.name || c.type}
        description={
          <div>
            <div>{usbDescription(c)}</div>
            <div>
              {battery} · {streams}
            </div>
          </div>
        }
        bottomSeparator="none"
      />
    </PanelSectionRow>
  );
}

function StatusSection({ state }: { state: State }) {
  const { settings, daemon, error, running } = state;
  let message: string | null = null;
  if (!settings.enabled) {
    message = "Bridge is off. A Bluetooth DualSense works as a normal Bluetooth controller.";
  } else if (error) {
    message = error;
  } else if (!running || !daemon) {
    message = "Starting the bridge…";
  } else if (!daemon.vhci.available) {
    message = daemon.vhci.error;
  } else if (daemon.controllers.length === 0) {
    message =
      "Waiting for a DualSense. Pair it once in Settings > Bluetooth (hold PS + Create until the light bar flashes), then just turn it on.";
  }
  return (
    <PanelSection title="Status">
      {message && (
        <PanelSectionRow>
          <Field description={message} bottomSeparator="none" />
        </PanelSectionRow>
      )}
      {settings.enabled && daemon?.controllers.map((c) => <ControllerRow key={c.id} c={c} />)}
      {settings.enabled &&
        daemon?.errors.map((e) => (
          <PanelSectionRow key={e}>
            <Field description={e} bottomSeparator="none" />
          </PanelSectionRow>
        ))}
    </PanelSection>
  );
}

function showLog() {
  getLog(120).then((log) => {
    showModal(
      <ConfirmModal
        strTitle="DS5 Bridge log"
        bAlertDialog
        strDescription={
          <pre style={{ whiteSpace: "pre-wrap", fontSize: "11px", maxHeight: "60vh", overflowY: "auto" }}>
            {log || "The log is empty."}
          </pre>
        }
      />,
    );
  });
}

function Content() {
  const [state, setState] = useState<State | null>(null);
  const visible = useQuickAccessVisible();

  const refresh = useCallback(() => {
    getState()
      .then(setState)
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    refresh();
    if (!visible) return undefined;
    const timer = setInterval(refresh, 1000);
    return () => clearInterval(timer);
  }, [visible, refresh]);

  if (!state) {
    return (
      <PanelSection>
        <PanelSectionRow>
          <Field description="Loading…" bottomSeparator="none" />
        </PanelSectionRow>
      </PanelSection>
    );
  }

  const s = state.settings;
  const update = (key: SettingKey, value: number | string) => {
    setState({ ...state, settings: { ...s, [key]: value } });
    setSetting(key, value).then((result) => {
      if (!result.ok) {
        toaster.toast({ title: "DS5 Bridge", body: result.error ?? "Could not change the setting" });
        refresh();
      }
    });
  };

  return (
    <>
      <PanelSection>
        <PanelSectionRow>
          <ToggleField
            label="Wireless wired-mode"
            description="Make a Bluetooth DualSense behave like a USB-connected one (HD haptics, speaker, mic, adaptive triggers)."
            checked={s.enabled}
            onChange={(enabled) => {
              setState({ ...state, settings: { ...s, enabled } });
              setEnabled(enabled).then(setState);
            }}
          />
        </PanelSectionRow>
      </PanelSection>

      <StatusSection state={state} />

      <PanelSection title="Haptics & triggers">
        <PanelSectionRow>
          <SliderField
            label="Haptics strength"
            value={Math.round(s.haptics_gain * 100)}
            min={100}
            max={200}
            step={10}
            showValue
            valueSuffix="%"
            onChange={(v) => update("haptics_gain", v / 100)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Adaptive trigger strength reduction"
            description="0 lets the game decide."
            value={s.trigger_reduce}
            min={0}
            max={10}
            step={1}
            showValue
            onChange={(v) => update("trigger_reduce", v)}
          />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Audio">
        <PanelSectionRow>
          <DropdownItem
            label="Speaker output"
            rgOptions={SPEAKER_OPTIONS}
            selectedOption={s.speaker_select}
            onChange={(o) => update("speaker_select", o.data)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Speaker boost"
            description="0 lets the game decide."
            value={s.speaker_gain}
            min={0}
            max={7}
            step={1}
            showValue
            onChange={(v) => update("speaker_gain", v)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <DropdownItem
            label="Microphone"
            rgOptions={MIC_OPTIONS}
            selectedOption={s.mic_select}
            onChange={(o) => update("mic_select", o.data)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <ToggleField
            label="Lock volume"
            description="Ignore volume and mute changes that games send to the controller."
            checked={!!s.lock_volume}
            onChange={(v) => update("lock_volume", v ? 1 : 0)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Controller audio buffer"
            description="Raise it if haptics or speaker audio stutter."
            value={s.audio_buffer_length}
            min={16}
            max={128}
            step={8}
            showValue
            onChange={(v) => update("audio_buffer_length", v)}
          />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Controller">
        <PanelSectionRow>
          <DropdownItem
            label="Present as"
            rgOptions={MODE_OPTIONS}
            selectedOption={s.controller_mode}
            onChange={(o) => update("controller_mode", o.data)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <DropdownItem
            label="Light bar on connect"
            rgOptions={LIGHTBAR_OPTIONS}
            selectedOption={s.lightbar_color}
            onChange={(o) => update("lightbar_color", o.data)}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Turn off when idle"
            description="Minutes without input before the controller is disconnected. 0 = never."
            value={s.inactive_time}
            min={0}
            max={60}
            step={5}
            showValue
            valueSuffix=" min"
            onChange={(v) => update("inactive_time", v)}
          />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Troubleshooting">
        <PanelSectionRow>
          <ButtonItem layout="below" disabled={!state.running} onClick={() => reattach().then(refresh)}>
            Reconnect virtual controller
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" disabled={!state.running} onClick={() => disconnectController().then(refresh)}>
            Turn off controller
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={showLog}>
            Show log
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => resetSettings().then(setState)}>
            Reset settings
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

export default definePlugin(() => ({
  name: "DS5 Bridge",
  titleView: <div className={staticClasses.Title}>DS5 Bridge</div>,
  content: <Content />,
  icon: <FaGamepad />,
}));
