import { useState, useEffect } from 'react';
import api from '../api';
import { t } from '../i18n';

interface ProcessInfo {
  pid: number;
  name: string;
  path: string;
}

interface ProcessSelectorProps {
  isOpen: boolean;
  onClose: () => void;
  onInjectSuccess: (pid: number) => void;
}

function isUEProcessCandidate(proc: ProcessInfo): boolean {
  const name = proc.name.toLowerCase();
  const path = (proc.path || '').toLowerCase();

  const denyByName = [
    'steam',
    'epic',
    'launcher',
    'updater',
    'helper',
    'service',
    'renderer',
    'crashreporter',
    'uexplorer',
    'app.exe',
  ];
  const denyByPath = [
    '\\windows\\system32\\',
    '\\windows\\syswow64\\',
    '\\microsoft\\edge\\',
    '\\google\\chrome\\',
    '\\mozilla firefox\\',
  ];
  if (denyByName.some((k) => name.includes(k))) {
    return false;
  }
  if (denyByPath.some((k) => path.includes(k))) {
    return false;
  }

  if (
    name.includes('ue4editor') ||
    name.includes('ue5editor') ||
    name.includes('unrealeditor') ||
    name.includes('ue4') ||
    name.includes('ue5') ||
    name.includes('unreal') ||
    name.includes('-win64-shipping') ||
    name.includes('-win64-development') ||
    name.includes('-win64-test')
  ) {
    return true;
  }

  let score = 0;
  const pathHints = [
    '\\engine\\binaries\\',
    '\\binaries\\win64\\',
    '\\windowsnoeditor\\',
    '\\saved\\stagedbuilds\\',
    '\\unrealengine\\',
  ];
  pathHints.forEach((hint) => {
    if (path.includes(hint)) score += 1;
  });
  if (name.includes('-win64-')) score += 1;
  if (name.endsWith('.exe')) score += 1;

  return score >= 2;
}

export default function ProcessSelector({ isOpen, onClose, onInjectSuccess }: ProcessSelectorProps) {
  const [processes, setProcesses] = useState<ProcessInfo[]>([]);
  const [loading, setLoading] = useState(false);
  const [selectedProcess, setSelectedProcess] = useState<ProcessInfo | null>(null);
  const [dllPath, setDllPath] = useState(api.getSettings().dllPath);
  const [injecting, setInjecting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [injectResult, setInjectResult] = useState<{ success: boolean; message: string } | null>(null);

  useEffect(() => {
    if (isOpen) {
      setDllPath(api.getSettings().dllPath);
      loadProcesses();
    }
  }, [isOpen]);

  const loadProcesses = async () => {
    setLoading(true);
    setError(null);
    try {
      const procs = await api.scanUEProcesses();
      const filtered = procs.filter(isUEProcessCandidate);
      setProcesses(filtered);
      if (selectedProcess && !filtered.some((p) => p.pid === selectedProcess.pid)) {
        setSelectedProcess(null);
      }
    } catch (err) {
      setError(t('Failed to scan processes'));
      console.error(err);
    } finally {
      setLoading(false);
    }
  };

  const handleInject = async () => {
    if (!selectedProcess) return;

    setInjecting(true);
    setInjectResult(null);
    setError(null);

    try {
      const result = await api.injectDLL(selectedProcess.pid, dllPath);
      setInjectResult(result);

      if (result.success) {
        api.updateSettings({ dllPath });
        onInjectSuccess(selectedProcess.pid);
        setTimeout(() => {
          onClose();
        }, 1500);
      }
    } catch (err) {
      setError(t('Injection failed'));
      console.error(err);
    } finally {
      setInjecting(false);
    }
  };

  if (!isOpen) return null;

  return (
    <div className="fixed inset-0 bg-black/60 flex items-center justify-center z-50">
      <div className="bg-[#1e1e1e] rounded-xl border border-white/10 w-[600px] max-h-[80vh] flex flex-col">
        {/* Header */}
        <div className="flex items-center justify-between px-6 py-4 border-b border-white/5">
          <div className="flex items-center gap-3">
            <span className="material-symbols-outlined text-primary text-[24px]">extension</span>
            <h2 className="text-white font-semibold text-lg">{t('Inject DLL')}</h2>
          </div>
          <button
            onClick={onClose}
            className="text-white/40 hover:text-white transition-colors"
          >
            <span className="material-symbols-outlined text-[20px]">close</span>
          </button>
        </div>

        {/* Content */}
        <div className="flex-1 overflow-auto p-6">
          {/* DLL Path */}
          <div className="mb-4">
            <label className="text-white/60 text-xs uppercase font-semibold tracking-wider mb-2 block">
              {t('DLL Path')}
            </label>
            <input
              type="text"
              value={dllPath}
              onChange={(e) => setDllPath(e.target.value)}
              className="w-full bg-[#252527] border border-white/10 rounded-lg px-3 py-2 text-white text-sm font-mono"
            />
          </div>

          {/* Process List */}
          <div className="mb-4">
            <div className="flex items-center justify-between mb-2">
              <label className="text-white/60 text-xs uppercase font-semibold tracking-wider">
                {t('Select Process')}
              </label>
              <button
                onClick={loadProcesses}
                className="text-primary hover:text-primary/80 text-xs flex items-center gap-1"
                disabled={loading}
              >
                <span className="material-symbols-outlined text-[14px]">refresh</span>
                {t('Refresh')}
              </button>
            </div>

            {loading ? (
              <div className="flex items-center justify-center py-8">
                <div className="w-6 h-6 border-2 border-primary border-t-transparent rounded-full animate-spin"></div>
              </div>
            ) : processes.length === 0 ? (
              <div className="text-white/40 text-center py-8 text-sm">
                {t('No Unreal Engine processes found')}
              </div>
            ) : (
              <div className="max-h-[200px] overflow-auto border border-white/5 rounded-lg">
                {processes.map((proc) => (
                  <div
                    key={proc.pid}
                    onClick={() => setSelectedProcess(proc)}
                    className={`px-3 py-2 cursor-pointer flex items-center justify-between transition-colors ${
                      selectedProcess?.pid === proc.pid
                        ? 'bg-primary/20 border-l-2 border-primary'
                        : 'hover:bg-white/5 border-l-2 border-transparent'
                    }`}
                  >
                    <div className="flex items-center gap-3">
                      <span className="material-symbols-outlined text-white/40 text-[18px]">sports_esports</span>
                      <div>
                        <div className="text-white text-sm font-medium">{proc.name}</div>
                        <div className="text-white/40 text-xs">{proc.path}</div>
                      </div>
                    </div>
                    <div className="text-white/40 text-xs font-mono">PID: {proc.pid}</div>
                  </div>
                ))}
              </div>
            )}
          </div>

          {/* Error / Result */}
          {error && (
            <div className="mb-4 p-3 bg-red-500/10 border border-red-500/20 rounded-lg">
              <div className="text-red-400 text-sm">{error}</div>
            </div>
          )}

          {injectResult && (
            <div className={`mb-4 p-3 rounded-lg ${
              injectResult.success
                ? 'bg-green-500/10 border border-green-500/20'
                : 'bg-red-500/10 border border-red-500/20'
            }`}>
              <div className={injectResult.success ? 'text-green-400' : 'text-red-400'}>
                {injectResult.message}
              </div>
            </div>
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-end gap-3 px-6 py-4 border-t border-white/5">
          <button
            onClick={onClose}
            className="px-4 py-2 rounded-lg text-white/60 hover:text-white hover:bg-white/5 transition-colors text-sm"
          >
            {t('Cancel')}
          </button>
          <button
            onClick={handleInject}
            disabled={!selectedProcess || injecting}
            className={`px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
              selectedProcess && !injecting
                ? 'bg-primary hover:bg-primary/90 text-white'
                : 'bg-white/10 text-white/40 cursor-not-allowed'
            }`}
          >
            {injecting ? (
              <span className="flex items-center gap-2">
                <div className="w-4 h-4 border-2 border-white/20 border-t-white rounded-full animate-spin"></div>
                {t('Injecting...')}
              </span>
            ) : (
              t('Inject DLL')
            )}
          </button>
        </div>
      </div>
    </div>
  );
}
