import { useMemo, useState, type ReactNode } from 'react';
import { Settings, Globe, Shield, HardDrive, Monitor, TestTube2, Save, Info } from 'lucide-react';
import api, { type ApiClientSettings, type DumpType } from '../api';
import { t, getLanguage, setLanguage, type Language } from '../i18n';

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

const tabLabelMap: Record<TabId, string> = {
  Connection: 'Connection',
  Injection: 'Injection',
  Dump: 'Dump',
  Display: 'Display',
};

export default function SettingsView() {
  const [activeTab, setActiveTab] = useState<TabId>('Connection');
  const [settings, setSettings] = useState<ApiClientSettings>(api.getSettings());
  const [display, setDisplay] = useState<DisplaySettings>(readDisplaySettings());
  const [saving, setSaving] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [currentLang, setCurrentLang] = useState<Language>(getLanguage());

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
    setMessage(persisted ? t('Settings saved') : t('Settings saved (native sync failed)'));
  };

  const testConnection = async () => {
    api.updateSettings(settings);
    await api.persistConnectionSettings();
    const ok = await api.healthCheck();
    setMessage(ok ? t('Health check: alive') : t('Health check failed'));
  };

  const handleLanguageChange = (lang: Language) => {
    setCurrentLang(lang);
    setLanguage(lang);
    window.location.reload();
  };

  return (
    <div className="flex-1 flex overflow-hidden bg-background-base">
      <div className="w-[280px] flex-none border-r border-border-subtle bg-surface-dark flex flex-col z-10">
        <div className="h-14 flex items-center px-6 border-b border-transparent" />
        <div className="p-4 space-y-1">
          {tabs.map((tab) => (
            <button
              key={tab.id}
              onClick={() => setActiveTab(tab.id)}
              className={`w-full flex items-center gap-3 px-3 py-2 rounded-xl transition-all duration-200 cursor-pointer ${activeTab === tab.id ? 'bg-primary text-white shadow-md shadow-primary/20' : 'text-text-mid hover:bg-surface-stripe hover:text-text-high'
                }`}
            >
              <div className={`w-7 h-7 rounded-[8px] flex items-center justify-center ${activeTab === tab.id ? 'bg-white/20' : 'bg-background-base border border-border-subtle'}`}>
                <tab.icon className={`w-3.5 h-3.5 stroke-[2] ${activeTab === tab.id ? 'text-white' : 'text-text-low'}`} />
              </div>
              <span className="text-[13px] font-medium tracking-tight font-display">{t(tabLabelMap[tab.id])}</span>
            </button>
          ))}
        </div>
      </div>

      <div className="flex-1 overflow-y-auto w-full max-w-4xl px-12 py-10">
        <div className="mb-8 flex items-center justify-between">
          <h1 className="text-3xl font-semibold text-text-high tracking-tight font-display">{t(tabLabelMap[activeTab])}</h1>
          <div className="flex items-center gap-2">
            <button
              onClick={() => void testConnection()}
              className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high border border-border-subtle text-sm flex items-center gap-2 font-display"
            >
              <TestTube2 className="w-4 h-4 text-text-low" />
              {t('Test')}
            </button>
            <button
              onClick={() => void saveAll()}
              disabled={saving}
              className="px-4 py-1.5 rounded-lg bg-primary hover:bg-primary/90 text-white text-sm flex items-center gap-2 disabled:opacity-50 font-display"
            >
              <Save className="w-4 h-4" />
              {saving ? t('Saving...') : t('Save')}
            </button>
          </div>
        </div>

        {message && <div className="mb-6 text-sm text-primary font-medium">{message}</div>}

        <div className="space-y-6">
          {activeTab === 'Connection' && (
            <>
              <Card>
                <SettingLine label={t('API Port')} desc={t('Port used by the DLL HTTP server')}>
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
                    className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-24 text-center outline-none focus:border-primary"
                  />
                </SettingLine>
                <SettingLine label={t('API Token')} desc={t('Shared secret for X-UExplorer-Token header')}>
                  <input
                    type="text"
                    value={settings.token}
                    onChange={(e) => setSettingsField('token', e.target.value)}
                    className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-56 outline-none focus:border-primary"
                  />
                </SettingLine>
              </Card>

              <Card>
                <SettingLine label={t('Auto Reconnect')} desc={t('Automatically reconnect when connection is lost')}>
                  <Toggle checked={settings.autoReconnect} onChange={(v) => setSettingsField('autoReconnect', v)} />
                </SettingLine>
              </Card>
            </>
          )}

          {activeTab === 'Injection' && (
            <Card>
              <SettingLine label={t('DLL File Path')} desc={t('Path to UExplorerCore.dll')}>
                <input
                  type="text"
                  value={settings.dllPath}
                  onChange={(e) => setSettingsField('dllPath', e.target.value)}
                  className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-[420px] outline-none focus:border-primary"
                />
              </SettingLine>
              <SettingLine label={t('Injection Method')} desc={t('Method used to inject DLL into target process')}>
                <select
                  value={settings.injectionMethod}
                  onChange={(e) => setSettingsField('injectionMethod', e.target.value as ApiClientSettings['injectionMethod'])}
                  className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                >
                  <option>CreateRemoteThread</option>
                  <option>DLL Proxy (xinput1_3)</option>
                </select>
              </SettingLine>
            </Card>
          )}

          {activeTab === 'Dump' && (
            <Card>
              <SettingLine label={t('Default Dump Format')} desc={t('Default format for SDK generation')}>
                <select
                  value={settings.defaultDumpFormat}
                  onChange={(e) => setSettingsField('defaultDumpFormat', e.target.value as DumpType)}
                  className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                >
                  <option value="sdk">sdk</option>
                  <option value="usmap">usmap</option>
                  <option value="dumpspace">dumpspace</option>
                  <option value="ida-script">ida-script</option>
                </select>
              </SettingLine>
              <SettingLine label={t('Output Directory')} desc={t('Directory for generated SDK files')}>
                <input
                  type="text"
                  value={settings.outputDir}
                  onChange={(e) => setSettingsField('outputDir', e.target.value)}
                  className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-[420px] outline-none focus:border-primary"
                />
              </SettingLine>
            </Card>
          )}

          {activeTab === 'Display' && (
            <>
              <Card>
                <SettingLine label={t('Language')} desc={t('Interface language')}>
                  <select
                    value={currentLang}
                    onChange={(e) => handleLanguageChange(e.target.value as Language)}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                  >
                    <option value="zh">{t('Chinese')}</option>
                    <option value="en">{t('English')}</option>
                  </select>
                </SettingLine>
                <SettingLine label={t('Theme')} desc={t('Application color theme')}>
                  <select
                    value={display.theme}
                    onChange={(e) => setDisplayField('theme', e.target.value as DisplaySettings['theme'])}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                  >
                    <option value="Dark">{t('Dark')}</option>
                    <option value="Light">{t('Light')}</option>
                  </select>
                </SettingLine>
                <SettingLine label={t('Address Format')} desc={t('How memory addresses are displayed')}>
                  <select
                    value={display.addressFormat}
                    onChange={(e) => setDisplayField('addressFormat', e.target.value as DisplaySettings['addressFormat'])}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                  >
                    <option value="0x Prefix">{t('0x Prefix')}</option>
                    <option value="No Prefix">{t('No Prefix')}</option>
                  </select>
                </SettingLine>
                <SettingLine label={t('Number Format')} desc={t('Default number display format')}>
                  <select
                    value={display.numberFormat}
                    onChange={(e) => setDisplayField('numberFormat', e.target.value as DisplaySettings['numberFormat'])}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-1.5 outline-none focus:border-primary"
                  >
                    <option value="Hex">{t('Hexadecimal')}</option>
                    <option value="Dec">{t('Decimal')}</option>
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
                    className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary"
                  />
                </SettingLine>
                <SettingLine label="GNames" desc="Legacy name pool override.">
                  <input
                    type="text"
                    value={display.gnamesOverride}
                    onChange={(e) => setDisplayField('gnamesOverride', e.target.value)}
                    className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary"
                  />
                </SettingLine>
                <SettingLine label="ProcessEvent" desc="ProcessEvent offset override.">
                  <input
                    type="text"
                    value={display.processEventOverride}
                    onChange={(e) => setDisplayField('processEventOverride', e.target.value)}
                    className="bg-background-base border border-border-subtle text-text-high text-xs font-mono placeholder:text-text-low/50 rounded-lg px-3 py-1.5 w-52 outline-none focus:border-primary"
                  />
                </SettingLine>
              </Card>
            </>
          )}

          <Card>
            <div className="p-4 flex items-center gap-3 text-text-mid bg-surface-stripe/30">
              <Settings className="w-4 h-4 text-text-low" />
              <span className="text-sm font-medium font-display">{t('UExplorer Desktop Configuration')}</span>
              <span className="ml-auto text-xs text-text-low font-mono bg-background-base px-2 py-0.5 rounded border border-border-subtle">v0.1-pre</span>
            </div>
            <div className="px-5 py-4 text-[11px] text-text-low flex items-center gap-2 border-t border-border-subtle bg-surface-dark/50">
              <Info className="w-3.5 h-3.5 text-primary" />
              {t('API token/port changes take effect immediately.')}
            </div>
          </Card>
        </div>
      </div>
    </div>
  );
}

function Card({ children }: { children: ReactNode }) {
  return <div className="bg-surface-dark border border-border-subtle rounded-xl overflow-hidden shadow-sm mb-6 max-w-2xl">{children}</div>;
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
    <div className="px-5 py-4 flex items-center justify-between border-b border-border-subtle last:border-b-0 bg-transparent gap-6 hover:bg-surface-stripe/30 transition-colors">
      <div>
        <div className="text-[13px] font-medium text-text-high mb-0.5 font-display">{label}</div>
        <div className="text-[11px] text-text-low leading-relaxed max-w-sm">{desc}</div>
      </div>
      <div className="flex-none">{children}</div>
    </div>
  );
}

function Toggle({ checked, onChange }: { checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <label className="relative inline-flex items-center cursor-pointer">
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} className="sr-only peer" />
      <div className="w-9 h-5 bg-background-base border border-border-subtle rounded-full peer peer-checked:bg-primary peer-checked:border-primary transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-text-high after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full shadow-sm" />
    </label>
  );
}
