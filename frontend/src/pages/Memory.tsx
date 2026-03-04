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
    <div className="flex-1 flex flex-col bg-[#0A0A0C] overflow-hidden">
      <div className="flex-none h-14 bg-white/[0.02] border-b border-white/5 backdrop-blur-3xl px-6 flex items-center justify-between z-20">
        <div className="flex items-center gap-3 w-1/2">
          <div className="flex items-center gap-1.5 mr-2">
            <button onClick={() => void navigateBack()} className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/40 hover:text-white transition-colors">
              <ArrowLeft className="w-4 h-4" />
            </button>
            <button onClick={() => void navigateForward()} className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/40 hover:text-white transition-colors">
              <ArrowRight className="w-4 h-4" />
            </button>
          </div>

          <div className="relative flex-1 group">
            <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
            <input
              type="text"
              value={addressInput}
              onChange={(e) => setAddressInput(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') void navigateToInput();
              }}
              className="w-full bg-black/40 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-1.5 outline-none focus:border-primary/50 focus:bg-black/60 transition-all font-mono placeholder:text-white/30"
            />
          </div>

          <button onClick={() => void loadMemory(addressInput, true)} className="w-8 h-8 flex items-center justify-center rounded-[8px] bg-white/5 hover:bg-white/10 border border-white/10 text-white transition-colors ml-2">
            <RefreshCw className={`w-4 h-4 ${loading ? 'animate-spin' : ''}`} />
          </button>
        </div>

        <div className="flex items-center gap-4 text-[13px] font-medium text-white/50">
          <button className="flex items-center gap-2 hover:text-white transition-colors">
            <Bookmark className="w-4 h-4" />
            Bookmarks
          </button>
        </div>
      </div>

      <div className="flex-1 flex min-h-0">
        <div className="flex-1 flex flex-col min-w-0 border-r border-white/5 bg-[#0A0A0C]">
          <div className="p-2 border-b border-white/5 flex items-center gap-4 bg-black/40 px-6">
            <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest">Hex Editor</span>
            <span className="text-[11px] font-mono text-white/50">16 Bytes/Row</span>
            {readError && <span className="text-[11px] text-red-300">{readError}</span>}
          </div>
          <div className="flex-1 overflow-auto p-6 font-mono text-[13px] leading-relaxed relative">
            <table className="w-full border-collapse">
              <tbody className="text-white/60">
                {rows.map((row, rowIndex) => (
                  <tr key={row.addr} className="hover:bg-white/5">
                    <td className="pr-6 text-white/30 user-select-none border-r border-white/5">{row.addr}</td>
                    <td className="px-6 tracking-[0.2em] text-[#A3E635]">
                      {row.chunk.map((b, colIndex) => {
                        const global = rowIndex * 16 + colIndex;
                        const selected = global === cursorOffset;
                        return (
                          <span
                            key={`${row.addr}-${colIndex}`}
                            onClick={() => setCursorOffset(global)}
                            className={`cursor-pointer rounded-sm px-0.5 ${selected ? 'bg-primary/40 text-white' : 'hover:text-white hover:bg-white/20'}`}
                          >
                            {b}
                          </span>
                        );
                      })}
                    </td>
                    <td className="pl-6 text-white/40 tracking-widest break-all">{row.ascii}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>

        <div className="w-[360px] flex-none bg-black/40 backdrop-blur-md flex flex-col border-r border-white/5">
          <div className="p-4 border-b border-white/5">
            <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest flex items-center gap-2">
              <Binary className="w-3.5 h-3.5" /> Data Inspector
            </span>
          </div>

          <div className="flex-1 overflow-y-auto p-5 space-y-6">
            <div className="space-y-3">
              {Object.entries(typedValues).map(([k, v]) => (
                <InspectorRow key={k} label={k} value={v} isLink={k === 'pointer'} />
              ))}
            </div>

            <div className="border-t border-white/5 pt-6 space-y-4">
              <div className="text-[11px] font-bold text-white/30 uppercase tracking-widest">Typed Write</div>
              <div className="apple-glass-panel rounded-xl p-3 space-y-3">
                <select
                  value={writeType}
                  onChange={(e) => setWriteType(e.target.value)}
                  className="w-full bg-black/40 border border-white/10 text-white text-[11px] rounded p-1"
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
                  className="w-full bg-black/40 border border-white/10 text-white text-[11px] rounded p-1 font-mono"
                />
                <button onClick={() => void writeTypedValue()} className="w-full bg-primary/80 hover:bg-primary text-white text-[11px] py-1 rounded">
                  Write At Cursor
                </button>
              </div>
            </div>

            <div className="border-t border-white/5 pt-6 space-y-4">
              <div className="text-[11px] font-bold text-white/30 uppercase tracking-widest flex items-center justify-between">
                <span>Pointer Chain</span>
                <Layers className="w-3.5 h-3.5" />
              </div>
              <div className="apple-glass-panel rounded-xl p-3 space-y-3">
                <input
                  type="text"
                  value={pointerBase}
                  onChange={(e) => setPointerBase(e.target.value)}
                  className="w-full bg-black/40 border border-white/10 text-white font-mono text-[11px] rounded p-1"
                />
                <input
                  type="text"
                  value={pointerOffsets}
                  onChange={(e) => setPointerOffsets(e.target.value)}
                  className="w-full bg-black/40 border border-white/10 text-white font-mono text-[11px] rounded p-1"
                />
                <button onClick={() => void resolvePointerChain()} className="w-full bg-white/5 hover:bg-white/10 border border-white/10 text-white text-[11px] py-1 rounded">
                  Resolve
                </button>
                <pre className="max-h-40 overflow-auto text-[10px] text-white/70 bg-black/30 rounded p-2 whitespace-pre-wrap">{pointerResult || 'No result'}</pre>
              </div>
            </div>
          </div>
        </div>

        <div className="w-[320px] flex-none bg-black/30 backdrop-blur-md flex flex-col">
          <div className="p-4 border-b border-white/5 flex items-center justify-between">
            <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest">Watch Panel</span>
            <button
              onClick={() => void refreshWatches()}
              className="w-7 h-7 rounded-lg bg-white/10 hover:bg-white/20 flex items-center justify-center"
            >
              <RefreshCw className="w-3.5 h-3.5 text-white/70" />
            </button>
          </div>
          <div className="flex-1 overflow-auto p-3 space-y-2">
            {watches.map((w) => (
              <div key={w.id} className={`p-2 rounded-lg border ${w.changed ? 'border-green-500/30 bg-green-500/10' : 'border-white/10 bg-black/20'}`}>
                <div className="text-xs text-white/90 font-mono">#{w.id} obj:{w.object_index}</div>
                <div className="text-xs text-white/60">{w.property}</div>
                <div className="text-xs text-blue-300 font-mono break-all">{String(w.value)}</div>
                <div className="flex justify-end gap-1 mt-1">
                  <button
                    onClick={() => void toggleWatchHistory(w.id)}
                    className="px-2 h-6 rounded bg-white/10 hover:bg-white/20 text-[10px] text-white"
                  >
                    History
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
                    className="w-6 h-6 rounded bg-red-500/20 hover:bg-red-500/30 flex items-center justify-center"
                  >
                    <Trash2 className="w-3 h-3 text-red-200" />
                  </button>
                </div>
                {expandedWatchId === w.id && (
                  <div className="mt-2 rounded border border-white/10 bg-black/30 p-2 max-h-36 overflow-auto space-y-1">
                    {watchHistoryLoading[w.id] && <div className="text-[10px] text-white/50">Loading...</div>}
                    {!watchHistoryLoading[w.id] && (watchHistory[w.id] || []).length === 0 && (
                      <div className="text-[10px] text-white/40">No history</div>
                    )}
                    {!watchHistoryLoading[w.id] &&
                      (watchHistory[w.id] || []).map((entry, idx) => (
                        <div key={`${entry.timestamp}-${idx}`} className="text-[10px] text-white/70 border-b border-white/5 pb-1 last:border-b-0">
                          <div className="text-white/40">{new Date(entry.timestamp).toLocaleString()}</div>
                          <div className="font-mono break-all">{JSON.stringify(entry.value)}</div>
                        </div>
                      ))}
                  </div>
                )}
              </div>
            ))}
            {watches.length === 0 && <div className="text-white/40 text-xs">No watches</div>}
          </div>
          <div className="p-3 border-t border-white/5">
            <button
              onClick={() => {
                const base = Number.parseInt(currentAddress.replace(/^0x/i, ''), 16) || 0;
                const at = `0x${(base + cursorOffset).toString(16).toUpperCase()}`;
                setAddressInput(at);
              }}
              className="w-full flex items-center justify-center gap-2 py-1.5 rounded-lg bg-white/10 hover:bg-white/20 text-white text-xs"
            >
              <Plus className="w-3 h-3" />
              Use Cursor Address
            </button>
          </div>
        </div>
      </div>

      <div className="h-[250px] flex-none border-t border-white/10 bg-[#000000] flex flex-col">
        <div className="h-8 border-b border-white/5 flex items-center px-4 bg-white/[0.02]">
          <span className="text-[10px] font-bold text-white/40 uppercase tracking-widest flex items-center gap-2">
            <Terminal className="w-3 h-3" /> UExplorer Console
          </span>
          <span className={`ml-auto text-[10px] font-mono ${wsConsoleConnected ? 'text-green-300' : 'text-yellow-300'}`}>
            {wsConsoleConnected ? 'WS:CONNECTED' : 'WS:FALLBACK'}
          </span>
        </div>
        <div className="flex-1 overflow-auto p-4 font-mono text-[12px] space-y-1">
          {consoleLogs.map((line, idx) => (
            <div key={`${idx}-${line.slice(0, 12)}`} className="text-white/70 whitespace-pre-wrap">
              {line}
            </div>
          ))}
          <div className="flex items-center gap-2 mt-2">
            <span className="text-primary">{'>'}</span>
            <input
              type="text"
              value={consoleInput}
              onChange={(e) => setConsoleInput(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') {
                  void runConsoleCommand();
                }
              }}
              className="flex-1 bg-transparent text-white outline-none"
              placeholder="Enter command..."
            />
          </div>
        </div>
      </div>
    </div>
  );
}

function InspectorRow({ label, value, isLink }: { label: string; value: string; isLink?: boolean }) {
  return (
    <div className="flex items-center justify-between gap-2">
      <span className="text-[12px] font-medium text-white/50">{label}</span>
      <span className={`font-mono text-[11px] bg-black/40 px-2 py-0.5 rounded border border-white/5 break-all ${isLink ? 'text-blue-400' : 'text-white'}`}>
        {value}
      </span>
    </div>
  );
}

