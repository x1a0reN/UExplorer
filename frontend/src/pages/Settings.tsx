import { useMemo, useState, type ReactNode } from 'react';
import { Settings, Globe, Shield, HardDrive, Monitor, TestTube2, Save, Info } from 'lucide-react';
import api, { type ApiClientSettings, type DumpType } from '../api';

type TabId = 'Connection' | 'Injection' | 'Dump' | 'Display';

type DisplaySettings = {
  theme: 'Light' | 'Dark';
  addressFormat: '0x Prefix' | 'No Prefix';
  numberFormat: 'Hex' | 'Dec';
  gobjectsOverride: string;
  gnamesOverride: string;
  processEventOverride: string;
};

const DISPLAY_KEY = 'uexplorer.display.settings';

function readDisplaySettings(): DisplaySettings {
  try {
    const raw = localStorage.getItem(DISPLAY_KEY);
    if (!raw) {
      return {
        theme: 'Dark',
        addressFormat: '0x Prefix',
        numberFormat: 'Hex',
        gobjectsOverride: '',
        gnamesOverride: '',
        processEventOverride: '',
      };
    }
    return {
      theme: 'Dark',
      addressFormat: '0x Prefix',
      numberFormat: 'Hex',
      gobjectsOverride: '',
      gnamesOverride: '',
      processEventOverride: '',
      ...(JSON.parse(raw) as Partial<DisplaySettings>),
    };
  } catch {
    return {
      theme: 'Dark',
      addressFormat: '0x Prefix',
      numberFormat: 'Hex',
      gobjectsOverride: '',
      gnamesOverride: '',
      processEventOverride: '',
    };
  }
}

export default function SettingsView() {
  const [activeTab, setActiveTab] = useState<TabId>('Connection');
  const [settings, setSettings] = useState<ApiClientSettings>(api.getSettings());
  const [display, setDisplay] = useState<DisplaySettings>(readDisplaySettings());
  const [saving, setSaving] = useState(false);
  const [message, setMessage] = useState<string | null>(null);

  const tabs = useMemo(
    () => [
      { id: 'Connection' as TabId, icon: Globe },
      { id: 'Injection' as TabId, icon: Shield },
      { id: 'Dump' as TabId, icon: HardDrive },
      { id: 'Display' as TabId, icon: Monitor },
    ],
    []
  );

  const setSettingsField = <K extends keyof ApiClientSettings>(key: K, value: ApiClientSettings[K]) => {
    setSettings((prev) => ({ ...prev, [key]: value }));
  };

  const setDisplayField = <K extends keyof DisplaySettings>(key: K, value: DisplaySettings[K]) => {
    setDisplay((prev) => ({ ...prev, [key]: value }));
  };

  const saveAll = async () => {
    setSaving(true);
    setMessage(null);
    api.updateSettings(settings);
    const persisted = await api.persistConnectionSettings();
    localStorage.setItem(DISPLAY_KEY, JSON.stringify(display));
    setSaving(false);
    setMessage(persisted ? 'Settings saved' : 'Settings saved (native sync failed)');
  };

  const testConnection = async () => {
    api.updateSettings(settings);
    await api.persistConnectionSettings();
    const ok = await api.healthCheck();
    setMessage(ok ? 'Health check: alive' : 'Health check failed');
  };

  return (
    <div className="flex-1 flex overflow-hidden bg-[#0A0A0C]">
      <div className="w-[280px] flex-none border-r border-white/5 bg-white/[0.02] flex flex-col z-10">
        <div className="h-14 flex items-center px-6 border-b border-transparent" />
        <div className="p-4 space-y-1">
          {tabs.map((tab) => (
            <button
              key={tab.id}
              onClick={() => setActiveTab(tab.id)}
              className={`w-full flex items-center gap-3 px-3 py-2 rounded-xl transition-all duration-200 cursor-pointer ${
                activeTab === tab.id ? 'bg-primary text-white shadow-md shadow-primary/20' : 'text-white/60 hover:bg-white/10 hover:text-white'
              }`}
            >
              <div className={`w-7 h-7 rounded-[8px] flex items-center justify-center ${activeTab === tab.id ? 'bg-white/20' : 'bg-white/5'}`}>
                <tab.icon className={`w-3.5 h-3.5 stroke-[2] ${activeTab === tab.id ? 'text-white' : 'text-white/50'}`} />
              </div>
              <span className="text-[13px] font-medium tracking-tight">{tab.id}</span>
            </button>
          ))}
        </div>
      </div>

      <div className="flex-1 overflow-y-auto w-full max-w-4xl px-12 py-8">
        <div className="mb-8 flex items-center justify-between">
          <h1 className="text-[28px] font-semibold text-white tracking-tight">{activeTab}</h1>
          <div className="flex items-center gap-2">
            <button
              onClick={() => void testConnection()}
              className="px-3 py-2 rounded-lg bg-white/10 hover:bg-white/20 text-white text-sm flex items-center gap-2"
            >
              <TestTube2 className="w-4 h-4" />
              Test
            </button>
            <button
              onClick={() => void saveAll()}
              disabled={saving}
              className="px-3 py-2 rounded-lg bg-primary hover:bg-primary-dark text-white text-sm flex items-center gap-2 disabled:opacity-60"
            >
              <Save className="w-4 h-4" />
              {saving ? 'Saving...' : 'Save'}
            </button>
          </div>
        </div>

        {message && <div className="mb-6 text-sm text-blue-200">{message}</div>}

        <div className="space-y-6">
          {activeTab === 'Connection' && (
            <>
              <Card>
                <SettingLine label="HTTP Port" desc="Port used to communicate with the injected DLL (0 = auto-select free port).">
                  <input
                    type="number"
                    value={settings.port}
                    onChange={(e) => {
                      const next = Number(e.target.value);
                      if (Number.isNaN(next)) {
                        setSettingsField('port', 27015);
                        return;
                      }
                      const clamped = Math.min(65535, Math.max(0, Math.trunc(next)));
                      setSettingsField('port', clamped);
                    }}
                    className="bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-1.5 w-24 text-center outline-none focus:border-primary/50"
                  />
                </SettingLine>
                <SettingLine label="Access Token" desc="Shared secret for API requests.">
                  <input
                    type="text"
                    value={settings.token}
                    onChange={(e) => setSettingsField('token', e.target.value)}
                    className="bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-1.5 w-56 outline-none focus:border-primary/50"
                  />
                </SettingLine>
              </Card>

              <Card>
                <SettingLine label="Auto Reconnect" desc="Poll and reconnect automatically when disconnected.">
                  <Toggle checked={settings.autoReconnect} onChange={(v) => setSettingsField('autoReconnect', v)} />
                </SettingLine>
              </Card>
            </>
          )}

          {activeTab === 'Injection' && (
            <Card>
              <SettingLine label="DLL Path" desc="Path to UExplorerCore.dll">
                <input
                  type="text"
                  value={settings.dllPath}
                  onChange={(e) => setSettingsField('dllPath', e.target.value)}
                  className="bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-lg px-3 py-1.5 w-[420px] outline-none focus:border-primary/50"
                />
              </SettingLine>
              <SettingLine label="Injection Method" desc="Remote thread or proxy mode.">
                <select
                  value={settings.injectionMethod}
                  onChange={(e) => setSettingsField('injectionMethod', e.target.value as ApiClientSettings['injectionMethod'])}
                  className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50"
                >
                  <option>CreateRemoteThread</option>
                  <option>DLL Proxy (xinput1_3)</option>
                </select>
              </SettingLine>
            </Card>
          )}

          {activeTab === 'Dump' && (
            <Card>
              <SettingLine label="Default Dump Format" desc="Default format shown in SDK Dump page.">
                <select
                  value={settings.defaultDumpFormat}
                  onChange={(e) => setSettingsField('defaultDumpFormat', e.target.value as DumpType)}
                  className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50"
                >
                  <option value="sdk">sdk</option>
                  <option value="usmap">usmap</option>
                  <option value="dumpspace">dumpspace</option>
                  <option value="ida-script">ida-script</option>
                </select>
              </SettingLine>
              <SettingLine label="Default Output Directory" desc="Metadata only for UI display.">
                <input
                  type="text"
                  value={settings.outputDir}
                  onChange={(e) => setSettingsField('outputDir', e.target.value)}
                  className="bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-lg px-3 py-1.5 w-[420px] outline-none focus:border-primary/50"
                />
              </SettingLine>
            </Card>
          )}

          {activeTab === 'Display' && (
            <>
              <Card>
                <SettingLine label="Theme" desc="UI theme preference.">
                  <select
                    value={display.theme}
                    onChange={(e) => setDisplayField('theme', e.target.value as DisplaySettings['theme'])}
                    className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50"
                  >
                    <option>Dark</option>
                    <option>Light</option>
                  </select>
                </SettingLine>
                <SettingLine label="Address Format" desc="How addresses should be displayed.">
                  <select
                    value={display.addressFormat}
                    onChange={(e) => setDisplayField('addressFormat', e.target.value as DisplaySettings['addressFormat'])}
                    className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50"
                  >
                    <option>0x Prefix</option>
                    <option>No Prefix</option>
                  </select>
                </SettingLine>
                <SettingLine label="Number Format" desc="Default numeric format for inspectors.">
                  <select
                    value={display.numberFormat}
                    onChange={(e) => setDisplayField('numberFormat', e.target.value as DisplaySettings['numberFormat'])}
                    className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50"
                  >
                    <option>Hex</option>
                    <option>Dec</option>
                  </select>
                </SettingLine>
              </Card>

              <Card>
                <div className="px-4 py-3 border-b border-white/5 text-white/70 text-sm font-medium">Manual Offset Override</div>
                <SettingLine label="GObjects" desc="Fallback when auto-scan fails.">
                  <input
                    type="text"
                    value={display.gobjectsOverride}
                    onChange={(e) => setDisplayField('gobjectsOverride', e.target.value)}
                    className="bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary/50"
                  />
                </SettingLine>
                <SettingLine label="GNames" desc="Legacy name pool override.">
                  <input
                    type="text"
                    value={display.gnamesOverride}
                    onChange={(e) => setDisplayField('gnamesOverride', e.target.value)}
                    className="bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary/50"
                  />
                </SettingLine>
                <SettingLine label="ProcessEvent" desc="ProcessEvent offset override.">
                  <input
                    type="text"
                    value={display.processEventOverride}
                    onChange={(e) => setDisplayField('processEventOverride', e.target.value)}
                    className="bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary/50"
                  />
                </SettingLine>
              </Card>
            </>
          )}

          <Card>
            <div className="p-4 flex items-center gap-3 text-white/70">
              <Settings className="w-4 h-4" />
              <span className="text-sm">UExplorer Desktop Configuration</span>
              <span className="ml-auto text-xs text-white/40 font-mono">v0.1-pre</span>
            </div>
            <div className="px-4 pb-4 text-xs text-white/40 flex items-center gap-2">
              <Info className="w-3.5 h-3.5" />
              API token/port 修改后会立即影响前端到 DLL 的请求。
            </div>
          </Card>
        </div>
      </div>
    </div>
  );
}

function Card({ children }: { children: ReactNode }) {
  return <div className="apple-glass-panel rounded-[16px] overflow-hidden">{children}</div>;
}

function SettingLine({
  label,
  desc,
  children,
}: {
  label: string;
  desc: string;
  children: ReactNode;
}) {
  return (
    <div className="p-4 flex items-center justify-between border-b border-white/5 last:border-b-0 bg-white/[0.01] gap-4">
      <div>
        <div className="text-[14px] font-medium text-white/90">{label}</div>
        <div className="text-[12px] text-white/40">{desc}</div>
      </div>
      {children}
    </div>
  );
}

function Toggle({ checked, onChange }: { checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <label className="relative inline-flex items-center cursor-pointer">
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} className="sr-only peer" />
      <div className="w-11 h-6 bg-white/10 rounded-full peer peer-checked:bg-[#28C840] transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-5 after:w-5 after:transition-all peer-checked:after:translate-x-full shadow-inner" />
    </label>
  );
}

