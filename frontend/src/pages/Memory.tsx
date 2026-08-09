import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Terminal, Binary, Bookmark, Search, ArrowRight, ArrowLeft, RefreshCw, Layers } from 'lucide-react';
import { t } from '../i18n';
import api from '../api';

const READ_SIZE = 256;

function parseAddress(value: string): string {
  const v = value.trim();
  if (!v) return '0x0';
  if (v.startsWith('0x') || v.startsWith('0X')) return v;
  const asNum = Number(v);
  if (!Number.isNaN(asNum)) return `0x${asNum.toString(16).toUpperCase()}`;
  return v;
}

function parseOffsets(raw: string): number[] {
  return raw
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
    .map((s) => {
      if (s.startsWith('0x') || s.startsWith('0X')) return Number.parseInt(s.slice(2), 16);
      return Number.parseInt(s, 16) || Number.parseInt(s, 10);
    })
    .filter((n) => !Number.isNaN(n));
}

function bytesToArray(input: string): number[] {
  return input
    .split(/[\s,]+/)
    .map((s) => s.trim())
    .filter(Boolean)
    .map((token) => Number.parseInt(token.replace(/^0x/i, ''), 16))
    .filter((n) => !Number.isNaN(n) && n >= 0 && n <= 255);
}

export default function Memory() {
  const [addressInput, setAddressInput] = useState('0x0');
  const [currentAddress, setCurrentAddress] = useState('0x0');
  const [history, setHistory] = useState<string[]>(['0x0']);
  const [historyIndex, setHistoryIndex] = useState(0);
  const historyIndexRef = useRef(0);

  const [hexBytes, setHexBytes] = useState<string[]>([]);
  const [readError, setReadError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [cursorOffset, setCursorOffset] = useState(0);
  const [typedValues, setTypedValues] = useState<Record<string, string>>({});

  const [writeType, setWriteType] = useState('int32');
  const [writeValue, setWriteValue] = useState('0');

  const [pointerBase, setPointerBase] = useState('0x0');
  const [pointerOffsets, setPointerOffsets] = useState('10, 20');
  const [pointerResult, setPointerResult] = useState('');

  const [consoleInput, setConsoleInput] = useState('');
  const [consoleLogs, setConsoleLogs] = useState<string[]>([
    'UExplorer local command adapter',
    'Supported: get/call/instances/mem.read/mem.write (object property set is disabled)',
  ]);

  const rows = useMemo(() => {
    const result: Array<{ addr: string; chunk: string[]; ascii: string }> = [];
    const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
    for (let i = 0; i < hexBytes.length; i += 16) {
      const chunk = hexBytes.slice(i, i + 16);
      const addr = `0x${(base + i).toString(16).toUpperCase()}`;
      const ascii = chunk
        .map((b) => {
          const n = Number.parseInt(b, 16);
          if (Number.isNaN(n) || n < 32 || n > 126) return '.';
          return String.fromCharCode(n);
        })
        .join('');
      result.push({ addr, chunk, ascii });
    }
    return result;
  }, [hexBytes, currentAddress]);

  const consoleEndRef = useRef<HTMLDivElement>(null);

  function pushConsole(line: string) {
    setConsoleLogs((prev) => [...prev, line]);
  }

  useEffect(() => {
    consoleEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [consoleLogs]);

  const updateHistoryIndex = useCallback((next: number) => {
    historyIndexRef.current = next;
    setHistoryIndex(next);
  }, []);

  const loadMemory = useCallback(async (addr: string, pushHistory = true) => {
    setLoading(true);
    setReadError(null);
    const normalized = parseAddress(addr);
    const res = await api.readMemory(normalized, READ_SIZE);
    setLoading(false);

    if (!res.success || !res.data) {
      setReadError(res.error || 'Read failed');
      return;
    }

    const bytes = res.data.hex.split(' ').filter(Boolean);
    setHexBytes(bytes);
    setCurrentAddress(res.data.address || normalized);
    setAddressInput(res.data.address || normalized);

    if (pushHistory) {
      setHistory((prev) => {
        const next = [...prev.slice(0, historyIndexRef.current + 1), res.data?.address || normalized];
        updateHistoryIndex(next.length - 1);
        return next;
      });
    }
  }, [updateHistoryIndex]);

  const loadTypedValues = useCallback(async () => {
    const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
    const at = `0x${(base + cursorOffset).toString(16).toUpperCase()}`;
    const types = ['byte', 'int32', 'uint32', 'int64', 'uint64', 'float', 'double', 'pointer'];

    const results = await Promise.all(types.map((t) => api.readTypedMemory(at, t)));
    const next: Record<string, string> = {};
    results.forEach((res, i) => {
      next[types[i]] = res.success && res.data ? String(res.data.value) : '-';
    });
    setTypedValues(next);
  }, [currentAddress, cursorOffset]);

  useEffect(() => {
    const timer = window.setTimeout(() => void loadMemory('0x0', false), 0);
    return () => window.clearTimeout(timer);
  }, [loadMemory]);

  useEffect(() => {
    if (!currentAddress) return;
    const timer = window.setTimeout(() => void loadTypedValues(), 0);
    return () => window.clearTimeout(timer);
  }, [currentAddress, hexBytes.length, loadTypedValues]);

  const navigateToInput = async () => {
    await loadMemory(addressInput, true);
  };

  const navigateBack = async () => {
    if (historyIndex <= 0) return;
    const target = history[historyIndex - 1];
    updateHistoryIndex(historyIndex - 1);
    await loadMemory(target, false);
  };

  const navigateForward = async () => {
    if (historyIndex >= history.length - 1) return;
    const target = history[historyIndex + 1];
    updateHistoryIndex(historyIndex + 1);
    await loadMemory(target, false);
  };

  const writeTypedValue = async () => {
    const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
    const at = `0x${(base + cursorOffset).toString(16).toUpperCase()}`;
    const value = Number.isNaN(Number(writeValue)) ? writeValue : Number(writeValue);
    const res = await api.writeTypedMemory(at, writeType, value);
    if (!res.success) {
      setReadError(res.error || 'Write failed');
      return;
    }
    await loadMemory(currentAddress, false);
  };

  const resolvePointerChain = async () => {
    const offsets = parseOffsets(pointerOffsets);
    const res = await api.resolvePointerChain(parseAddress(pointerBase), offsets);
    if (!res.success || !res.data) {
      setPointerResult(res.error || 'Pointer chain failed');
      return;
    }
    setPointerResult(JSON.stringify(res.data, null, 2));
  };

  const runConsoleCommand = async () => {
    const command = consoleInput.trim();
    if (!command) return;
    setConsoleInput('');
    pushConsole(`> ${command}`);

    const parts = command.split(' ');
    const head = parts[0];

    try {
      if (head === 'get' && parts[1]) {
        const target = parts.slice(1).join(' ');
        let out: unknown;
        if (/^\d+$/.test(target)) {
          out = await api.getObjectByIndex(Number(target));
        } else if (target.startsWith('0x')) {
          out = await api.getObjectByAddress(target);
        } else {
          out = await api.getObjectByPath(target);
        }
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'set' && parts.length >= 3) {
        throw new Error('OBJECT_PROPERTY_WRITE_DISABLED');
      } else if (head === 'call' && parts.length >= 3) {
        const objectIndex = Number(parts[1]);
        const functionName = parts[2];
        const jsonParams = parts.slice(3).join(' ');
        const params = jsonParams ? (JSON.parse(jsonParams) as Record<string, unknown>) : {};
        const out = await api.callFunction(objectIndex, functionName, params, true);
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'instances' && parts[1]) {
        const out = await api.getClassInstances(parts[1], 0, 100);
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'mem.read' && parts.length >= 3) {
        const out = await api.readMemory(parts[1], Number(parts[2]));
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'mem.write' && parts.length >= 3) {
        const addr = parts[1];
        const bytes = bytesToArray(parts.slice(2).join(' '));
        const out = await api.writeMemory(addr, bytes);
        pushConsole(JSON.stringify(out, null, 2));
      } else {
        pushConsole('Unknown command');
      }
    } catch (error) {
      pushConsole(`Error: ${error instanceof Error ? error.message : String(error)}`);
    }
  };

  return (
    <div className="flex-1 flex flex-col bg-background-base overflow-hidden">
      <div className="flex-none h-12 bg-surface-dark border-b border-border-subtle backdrop-blur-3xl px-6 flex items-center justify-between z-20">
        <div className="flex items-center gap-3 w-1/2">
          <div className="flex items-center gap-1.5 mr-2">
            <button onClick={() => void navigateBack()} className="w-7 h-7 flex items-center justify-center rounded-lg hover:bg-surface-stripe text-text-low hover:text-text-high transition-colors">
              <ArrowLeft className="w-4 h-4" />
            </button>
            <button onClick={() => void navigateForward()} className="w-7 h-7 flex items-center justify-center rounded-lg hover:bg-surface-stripe text-text-low hover:text-text-high transition-colors">
              <ArrowRight className="w-4 h-4" />
            </button>
          </div>

          <div className="relative flex-1 group">
            <Search className="w-3.5 h-3.5 text-text-low absolute left-3 top-2" />
            <input
              type="text"
              value={addressInput}
              onChange={(e) => setAddressInput(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') void navigateToInput();
              }}
              className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg pl-9 pr-3 py-1 outline-none focus:border-primary focus:bg-background-base transition-all font-mono placeholder:text-text-low/50"
            />
          </div>

          <button onClick={() => void loadMemory(addressInput, true)} className="w-7 h-7 flex items-center justify-center rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle text-text-high transition-colors ml-2">
            <RefreshCw className={`w-3.5 h-3.5 text-text-low ${loading ? 'animate-spin' : ''}`} />
          </button>
        </div>

        <div className="flex items-center gap-4 text-xs font-medium text-text-low font-display">
          <button className="flex items-center gap-2 hover:text-text-high transition-colors">
            <Bookmark className="w-3.5 h-3.5" />
            {t('Bookmarks')}
          </button>
        </div>
      </div>

      <div className="flex-1 flex min-h-0">
        <div className="flex-1 flex flex-col min-w-0 border-r border-border-subtle bg-background-base">
          <div className="h-8 border-b border-border-subtle flex items-center gap-4 bg-surface-dark px-6">
            <span className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('Hex Editor')}</span>
            <span className="text-[10px] font-mono text-text-low">{t('16 Bytes/Row')}</span>
            {readError && <span className="text-[10px] text-accent-red font-medium">{readError}</span>}
          </div>
          <div className="flex-1 overflow-auto p-4 font-mono text-xs leading-none relative bg-background-base">
            <table className="w-full border-collapse">
              <tbody className="text-text-mid">
                {rows.map((row, rowIndex) => (
                  <tr key={row.addr} className="hover:bg-surface-stripe/50 group">
                    <td className="pr-4 py-1 text-text-low user-select-none border-r border-border-subtle whitespace-nowrap">{row.addr}</td>
                    <td className="px-4 py-1 tracking-[0.25em] text-accent-green font-medium whitespace-nowrap">
                      {row.chunk.map((b, colIndex) => {
                        const global = rowIndex * 16 + colIndex;
                        const selected = global === cursorOffset;
                        return (
                          <span
                            key={`${row.addr}-${colIndex}`}
                            onClick={() => setCursorOffset(global)}
                            className={`cursor-pointer rounded-sm px-0.5 transition-colors ${selected ? 'bg-primary text-white' : 'hover:text-text-high hover:bg-surface-stripe'}`}
                          >
                            {b}
                          </span>
                        );
                      })}
                    </td>
                    <td className="pl-4 py-1 text-text-low tracking-[0.15em] break-all whitespace-nowrap border-l border-border-subtle">{row.ascii}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>

        <div className="w-[320px] flex-none bg-surface-dark flex flex-col border-r border-border-subtle">
          <div className="h-8 border-b border-border-subtle flex items-center px-4 bg-surface-dark">
            <span className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center gap-1.5 font-display">
              <Binary className="w-3.5 h-3.5" /> {t('Data Inspector')}
            </span>
          </div>

          <div className="flex-1 overflow-y-auto p-4 space-y-5">
            <div className="space-y-2.5">
              {Object.entries(typedValues).map(([k, v]) => (
                <InspectorRow key={k} label={k} value={v} isLink={k === 'pointer'} />
              ))}
            </div>

            <div className="border-t border-border-subtle pt-5 space-y-3">
              <div className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('Typed Write')}</div>
              <div className="bg-background-base border border-border-subtle rounded-lg p-3 space-y-2.5 shadow-sm">
                <select
                  value={writeType}
                  onChange={(e) => setWriteType(e.target.value)}
                  className="w-full bg-surface-dark border border-border-subtle text-text-high text-xs rounded px-2 py-1 outline-none focus:border-primary font-mono"
                >
                  <option>byte</option>
                  <option>int32</option>
                  <option>float</option>
                  <option>double</option>
                </select>
                <input
                  type="text"
                  value={writeValue}
                  onChange={(e) => setWriteValue(e.target.value)}
                  className="w-full bg-surface-dark border border-border-subtle text-text-high text-xs rounded px-2 py-1 outline-none focus:border-primary font-mono placeholder:text-text-low/50"
                />
                <button onClick={() => void writeTypedValue()} className="w-full bg-primary hover:bg-primary/90 text-white text-xs py-1.5 rounded transition-colors font-display">
                  {t('Write At Cursor')}
                </button>
              </div>
            </div>

            <div className="border-t border-border-subtle pt-5 space-y-3">
              <div className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center justify-between font-display">
                <span>{t('Pointer Chain')}</span>
                <Layers className="w-3.5 h-3.5 text-text-mid" />
              </div>
              <div className="bg-background-base border border-border-subtle rounded-lg p-3 space-y-2.5 shadow-sm">
                <input
                  type="text"
                  value={pointerBase}
                  onChange={(e) => setPointerBase(e.target.value)}
                  className="w-full bg-surface-dark border border-border-subtle text-text-high font-mono text-xs rounded px-2 py-1 outline-none focus:border-primary placeholder:text-text-low/50"
                  placeholder="Base Address"
                />
                <input
                  type="text"
                  value={pointerOffsets}
                  onChange={(e) => setPointerOffsets(e.target.value)}
                  className="w-full bg-surface-dark border border-border-subtle text-text-high font-mono text-xs rounded px-2 py-1 outline-none focus:border-primary placeholder:text-text-low/50"
                  placeholder="Offsets (e.g. 10, 20)"
                />
                <button onClick={() => void resolvePointerChain()} className="w-full bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle text-text-high text-xs py-1.5 rounded transition-colors font-display">
                  {t('Resolve')}
                </button>
                <pre className="max-h-40 overflow-auto text-[10px] text-text-mid bg-surface-dark border border-border-subtle rounded p-2 whitespace-pre-wrap font-mono">{pointerResult || t('No result')}</pre>
              </div>
            </div>
          </div>
        </div>

        <div className="w-[320px] flex-none bg-surface-dark flex flex-col">
          <div className="h-8 border-b border-border-subtle flex items-center px-4 bg-surface-dark">
            <span className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('Watch Panel')}</span>
          </div>
          <div className="flex-1 p-4">
            <div className="rounded-lg border border-accent-yellow/20 bg-accent-yellow/5 p-3 text-xs text-text-mid font-display leading-relaxed">
              {t('Unavailable: legacy polling/SSE watches are disabled until the IPC WatchScheduler is active.')}
            </div>
          </div>
        </div>
      </div>

      <div className="h-[220px] flex-none border-t border-border-subtle bg-background-base flex flex-col">
        <div className="h-8 border-b border-border-subtle flex items-center px-4 bg-surface-dark">
          <span className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center gap-2 font-display">
            <Terminal className="w-3.5 h-3.5" /> {t('UExplorer Console')}
          </span>
          <span className="ml-auto text-[10px] font-mono font-medium px-2 py-0.5 rounded border text-text-mid border-border-subtle bg-surface-stripe">
            {t('LOCAL COMMANDS')}
          </span>
        </div>
        <div className="flex-1 overflow-auto p-4 font-mono text-xs space-y-1">
          {consoleLogs.map((line, idx) => (
            <div key={`${idx}-${line.slice(0, 12)}`} className="text-text-mid whitespace-pre-wrap">
              {line}
            </div>
          ))}
          <div ref={consoleEndRef} />
          <div className="flex items-center gap-2 mt-2">
            <span className="text-primary font-bold">{'>'}</span>
            <input
              type="text"
              value={consoleInput}
              onChange={(e) => setConsoleInput(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') {
                  void runConsoleCommand();
                }
              }}
              className="flex-1 bg-transparent text-text-high outline-none font-mono placeholder:text-text-low/50"
              placeholder={t('Enter command...')}
            />
          </div>
        </div>
      </div>
    </div>
  );
}

function InspectorRow({ label, value, isLink }: { label: string; value: string; isLink?: boolean }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <span className="text-[11px] font-medium text-text-low font-display">{label}</span>
      <span className={`font-mono text-[11px] bg-background-base px-1.5 py-0.5 rounded border border-border-subtle break-all font-medium ${isLink ? 'text-primary hover:underline cursor-pointer' : 'text-text-high'}`}>
        {value}
      </span>
    </div>
  );
}
