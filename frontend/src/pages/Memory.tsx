import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Terminal, Binary, Bookmark, Search, ArrowRight, ArrowLeft, RefreshCw, Layers, Plus, Trash2 } from 'lucide-react';
import { t } from '../i18n';
import api, { type WatchHistoryEntry, type WatchItem } from '../api';

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

function parseInputValue(raw: string): unknown {
  const trimmed = raw.trim();
  if (trimmed === '') return '';
  if (trimmed === 'true') return true;
  if (trimmed === 'false') return false;
  if (trimmed === 'null') return null;
  if (!Number.isNaN(Number(trimmed))) return Number(trimmed);
  try {
    return JSON.parse(trimmed);
  } catch {
    return trimmed;
  }
}

export default function Memory() {
  const [addressInput, setAddressInput] = useState('0x0');
  const [currentAddress, setCurrentAddress] = useState('0x0');
  const [history, setHistory] = useState<string[]>(['0x0']);
  const [historyIndex, setHistoryIndex] = useState(0);

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
    'UExplorer Console [Connected]',
    'Supported: get/set/call/watch/unwatch/instances/mem.read/mem.write',
  ]);
  const [wsConsoleConnected, setWsConsoleConnected] = useState(false);
  const consoleWsRef = useRef<ReturnType<typeof api.connectWebSocket> | null>(null);

  const [watches, setWatches] = useState<WatchItem[]>([]);
  const [watchHistory, setWatchHistory] = useState<Record<number, WatchHistoryEntry[]>>({});
  const [expandedWatchId, setExpandedWatchId] = useState<number | null>(null);
  const [watchHistoryLoading, setWatchHistoryLoading] = useState<Record<number, boolean>>({});
  const watchPollTimerRef = useRef<number | null>(null);
  const watchPollInFlightRef = useRef(false);
  const watchReconnectTimerRef = useRef<number | null>(null);
  const watchReconnectDelayRef = useRef(800);

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

  const refreshWatches = useCallback(async () => {
    const res = await api.listWatches();
    if (res.success && res.data) {
      setWatches(res.data.watches);
    }
  }, []);

  const stopWatchPolling = useCallback(() => {
    if (watchPollTimerRef.current !== null) {
      window.clearInterval(watchPollTimerRef.current);
      watchPollTimerRef.current = null;
    }
  }, []);

  const runWatchFallbackTick = useCallback(async () => {
    if (watchPollInFlightRef.current) return;
    watchPollInFlightRef.current = true;
    try {
      await refreshWatches();
    } finally {
      watchPollInFlightRef.current = false;
    }
  }, [refreshWatches]);

  const startWatchPolling = useCallback(
    (immediate = true) => {
      if (watchPollTimerRef.current !== null) return;
      if (immediate) {
        void runWatchFallbackTick();
      }
      watchPollTimerRef.current = window.setInterval(() => {
        void runWatchFallbackTick();
      }, 300);
    },
    [runWatchFallbackTick]
  );

  const clearWatchReconnectTimer = useCallback(() => {
    if (watchReconnectTimerRef.current !== null) {
      window.clearTimeout(watchReconnectTimerRef.current);
      watchReconnectTimerRef.current = null;
    }
  }, []);

  useEffect(() => {
    void loadMemory('0x0', false);
  }, []);

  useEffect(() => {
    const conn = api.connectWebSocket('/ws/console', {
      onOpen: () => {
        setWsConsoleConnected(true);
        pushConsole('[WS] console connected');
      },
      onClose: () => {
        setWsConsoleConnected(false);
        pushConsole('[WS] console disconnected');
      },
      onError: () => {
        setWsConsoleConnected(false);
      },
      onMessage: (payload) => {
        if (typeof payload === 'string') {
          pushConsole(`[WS] ${payload}`);
          return;
        }
        pushConsole(`[WS] ${JSON.stringify(payload)}`);
      },
    });

    consoleWsRef.current = conn;
    return () => {
      if (consoleWsRef.current === conn) {
        consoleWsRef.current = null;
      }
      conn.close();
    };
  }, []);

  useEffect(() => {
    let disposed = false;
    let unsubscribe: (() => void) | null = null;

    const connect = () => {
      if (disposed) return;
      unsubscribe = api.subscribeEventStream('/events/watches', () => {
        void refreshWatches();
      }, {
        onOpen: () => {
          clearWatchReconnectTimer();
          watchReconnectDelayRef.current = 800;
          stopWatchPolling();
          void refreshWatches();
        },
        onError: () => {
          if (disposed) return;
          startWatchPolling(true);
        },
        onClose: (reason) => {
          if (disposed || reason === 'abort') return;
          startWatchPolling(true);

          if (watchReconnectTimerRef.current !== null) return;
          const delay = watchReconnectDelayRef.current;
          watchReconnectTimerRef.current = window.setTimeout(() => {
            watchReconnectTimerRef.current = null;
            connect();
          }, delay);
          watchReconnectDelayRef.current = Math.min(watchReconnectDelayRef.current * 2, 5000);
        },
      });
    };

    connect();
    return () => {
      disposed = true;
      unsubscribe?.();
      clearWatchReconnectTimer();
      watchReconnectDelayRef.current = 800;
      stopWatchPolling();
    };
  }, [clearWatchReconnectTimer, refreshWatches, startWatchPolling, stopWatchPolling]);

  useEffect(() => {
    if (!currentAddress) return;
    void loadTypedValues();
  }, [currentAddress, cursorOffset, hexBytes.length]);

  function pushConsole(line: string) {
    setConsoleLogs((prev) => [...prev, line]);
  }

  const loadMemory = async (addr: string, pushHistory = true) => {
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
        const next = [...prev.slice(0, historyIndex + 1), res.data?.address || normalized];
        setHistoryIndex(next.length - 1);
        return next;
      });
    }
  };

  const loadTypedValues = async () => {
    const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
    const at = `0x${(base + cursorOffset).toString(16).toUpperCase()}`;
    const types = ['byte', 'int32', 'uint32', 'int64', 'uint64', 'float', 'double', 'pointer'];

    const results = await Promise.all(types.map((t) => api.readTypedMemory(at, t)));
    const next: Record<string, string> = {};
    results.forEach((res, i) => {
      next[types[i]] = res.success && res.data ? String(res.data.value) : '-';
    });
    setTypedValues(next);
  };

  const navigateToInput = async () => {
    await loadMemory(addressInput, true);
  };

  const navigateBack = async () => {
    if (historyIndex <= 0) return;
    const target = history[historyIndex - 1];
    setHistoryIndex((idx) => idx - 1);
    await loadMemory(target, false);
  };

  const navigateForward = async () => {
    if (historyIndex >= history.length - 1) return;
    const target = history[historyIndex + 1];
    setHistoryIndex((idx) => idx + 1);
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

    if (wsConsoleConnected && consoleWsRef.current) {
      consoleWsRef.current.send(command);
      return;
    }

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
        const [objAndProp, ...valueParts] = parts.slice(1);
        const dot = objAndProp.lastIndexOf('.');
        if (dot <= 0) throw new Error(t('Use: set <objectIndex>.<property> <value>'));
        const objectIndex = Number(objAndProp.slice(0, dot));
        const property = objAndProp.slice(dot + 1);
        const value = parseInputValue(valueParts.join(' '));
        const out = await api.setObjectProperty(objectIndex, property, value);
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'call' && parts.length >= 3) {
        const objectIndex = Number(parts[1]);
        const functionName = parts[2];
        const jsonParams = parts.slice(3).join(' ');
        const params = jsonParams ? (JSON.parse(jsonParams) as Record<string, unknown>) : {};
        const out = await api.callFunction(objectIndex, functionName, params, true);
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'watch' && parts.length >= 3) {
        const objectIndex = Number(parts[1]);
        const property = parts[2];
        const out = await api.addWatch(objectIndex, property);
        pushConsole(JSON.stringify(out, null, 2));
      } else if (head === 'unwatch' && parts[1]) {
        const out = await api.removeWatch(Number(parts[1]));
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

  const toggleWatchHistory = async (watchId: number) => {
    if (expandedWatchId === watchId) {
      setExpandedWatchId(null);
      return;
    }

    setExpandedWatchId(watchId);
    if (watchHistory[watchId]) return;

    setWatchHistoryLoading((prev) => ({ ...prev, [watchId]: true }));
    const res = await api.getWatchHistory(watchId, 100);
    setWatchHistoryLoading((prev) => ({ ...prev, [watchId]: false }));

    if (!res.success || !res.data) {
      pushConsole(`[WatchHistory] ${res.error || 'load failed'}`);
      return;
    }
    const historyData = res.data;

    setWatchHistory((prev) => ({
      ...prev,
      [watchId]: historyData.history,
    }));
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
          <div className="h-8 border-b border-border-subtle flex items-center justify-between px-4 bg-surface-dark">
            <span className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('Watch Panel')}</span>
            <button
              onClick={() => void refreshWatches()}
              className="w-6 h-6 rounded flex items-center justify-center hover:bg-surface-stripe transition-colors"
            >
              <RefreshCw className="w-3.5 h-3.5 text-text-low hover:text-text-high" />
            </button>
          </div>
          <div className="flex-1 overflow-auto p-2.5 space-y-2">
            {watches.map((w) => (
              <div key={w.id} className={`p-2.5 rounded-lg border text-sm ${w.changed ? 'border-accent-green/30 bg-accent-green/10' : 'border-border-subtle bg-background-base'}`}>
                <div className="text-xs text-text-high font-mono">#{w.id} obj:{w.object_index}</div>
                <div className="text-xs text-text-low font-display mt-0.5">{w.property}</div>
                <div className="text-xs text-primary font-mono break-all mt-1">{String(w.value)}</div>
                <div className="flex justify-end gap-1.5 mt-2 border-t border-border-subtle/50 pt-2">
                  <button
                    onClick={() => void toggleWatchHistory(w.id)}
                    className="px-2 py-1 rounded bg-surface-stripe hover:bg-surface-stripe/80 text-[10px] text-text-high font-display border border-border-subtle transition-colors"
                  >
                    {t('History')}
                  </button>
                  <button
                    onClick={() => {
                      void (async () => {
                        await api.removeWatch(w.id);
                        setWatchHistory((prev) => {
                          const next = { ...prev };
                          delete next[w.id];
                          return next;
                        });
                        if (expandedWatchId === w.id) {
                          setExpandedWatchId(null);
                        }
                        await refreshWatches();
                      })();
                    }}
                    className="w-6 h-6 rounded bg-accent-red/10 hover:bg-accent-red/20 flex items-center justify-center border border-accent-red/20 transition-colors"
                  >
                    <Trash2 className="w-3 h-3 text-accent-red" />
                  </button>
                </div>
                {expandedWatchId === w.id && (
                  <div className="mt-2 text-xs rounded border border-border-subtle bg-surface-dark p-2 max-h-36 overflow-auto space-y-1">
                    {watchHistoryLoading[w.id] && <div className="text-[10px] text-text-low">{t('Loading...')}</div>}
                    {!watchHistoryLoading[w.id] && (watchHistory[w.id] || []).length === 0 && (
                      <div className="text-[10px] text-text-low font-display">{t('No history')}</div>
                    )}
                    {!watchHistoryLoading[w.id] &&
                      (watchHistory[w.id] || []).map((entry, idx) => (
                        <div key={`${entry.timestamp}-${idx}`} className="text-[10px] text-text-mid border-b border-border-subtle pb-1 last:border-b-0 space-y-0.5">
                          <div className="text-text-low/60">{new Date(entry.timestamp).toLocaleString()}</div>
                          <div className="font-mono break-all">{JSON.stringify(entry.value)}</div>
                        </div>
                      ))}
                  </div>
                )}
              </div>
            ))}
            {watches.length === 0 && <div className="text-text-low text-xs text-center mt-4 font-display">{t('No watches active')}</div>}
          </div>
          <div className="p-2 border-t border-border-subtle bg-surface-dark">
            <button
              onClick={() => {
                const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
                const at = `0x${(base + cursorOffset).toString(16).toUpperCase()}`;
                setAddressInput(at);
              }}
              className="w-full flex items-center justify-center gap-2 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high text-xs font-display border border-border-subtle transition-colors"
            >
              <Plus className="w-3.5 h-3.5 text-text-low" />
              {t('Use Cursor Address')}
            </button>
          </div>
        </div>
      </div>

      <div className="h-[220px] flex-none border-t border-border-subtle bg-background-base flex flex-col">
        <div className="h-8 border-b border-border-subtle flex items-center px-4 bg-surface-dark">
          <span className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center gap-2 font-display">
            <Terminal className="w-3.5 h-3.5" /> {t('UExplorer Console')}
          </span>
          <span className={`ml-auto text-[10px] font-mono font-medium px-2 py-0.5 rounded border ${wsConsoleConnected ? 'text-accent-green border-accent-green/20 bg-accent-green/10' : 'text-accent-yellow border-accent-yellow/20 bg-accent-yellow/10'}`}>
            {wsConsoleConnected ? t('WS:CONNECTED') : t('WS:FALLBACK')}
          </span>
        </div>
        <div className="flex-1 overflow-auto p-4 font-mono text-xs space-y-1">
          {consoleLogs.map((line, idx) => (
            <div key={`${idx}-${line.slice(0, 12)}`} className="text-text-mid whitespace-pre-wrap">
              {line}
            </div>
          ))}
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

