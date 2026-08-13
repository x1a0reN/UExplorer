import type { ApiClientSettings, DumpType } from '../contracts';

const SETTINGS_KEY = 'uexplorer.settings';

const DEFAULT_SETTINGS: ApiClientSettings = {
  dllPath: 'D:\\Projects\\UExplorer\\Dumper\\x64\\Release\\UExplorerCore.dll',
  defaultDumpFormat: 'sdk',
};

function isDumpType(value: unknown): value is DumpType {
  return value === 'sdk' || value === 'usmap' || value === 'dumpspace' || value === 'ida-script';
}

function loadSettings(): ApiClientSettings {
  if (typeof localStorage === 'undefined') return { ...DEFAULT_SETTINGS };
  try {
    const value = JSON.parse(localStorage.getItem(SETTINGS_KEY) ?? '{}') as Record<string, unknown>;
    return {
      dllPath: typeof value.dllPath === 'string' ? value.dllPath : DEFAULT_SETTINGS.dllPath,
      defaultDumpFormat: isDumpType(value.defaultDumpFormat)
        ? value.defaultDumpFormat
        : DEFAULT_SETTINGS.defaultDumpFormat,
    };
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
}

class SettingsService {
  private value = loadSettings();
  private listeners = new Set<(settings: ApiClientSettings) => void>();

  get(): ApiClientSettings {
    return { ...this.value };
  }

  update(next: Partial<ApiClientSettings>): ApiClientSettings {
    this.value = { ...this.value, ...next };
    if (typeof localStorage !== 'undefined') {
      localStorage.setItem(SETTINGS_KEY, JSON.stringify(this.value));
    }
    const current = this.get();
    this.listeners.forEach((listener) => listener(current));
    return current;
  }

  subscribe(listener: (settings: ApiClientSettings) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
}

export const settingsService = new SettingsService();
