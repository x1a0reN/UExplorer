import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Terminal, Binary, Bookmark, Search, ArrowRight, ArrowLeft, RefreshCw, Layers } from 'lucide-react';
import { t } from '../i18n';
import api from '../services';
import type { WatchDrainData, WatchEvent, WatchHistoryData, WatchItem, WatchPushEventData, WatchValue } from '../contracts';
import { useSession } from '../session/SessionProvider';
import { addAddress, parseAddress } from '../features/address/address';
import { DomainError } from '../features/shared/DomainError';
import { isAbortError, useQueryRunner } from '../features/query/useQueryRunner';

const READ_SIZE = 256;

function parseOffsets(raw: string): string[] {
  if (!raw.trim()) return [];
  const tokens = raw.split(',').map((token) => token.trim());
  if (tokens.some((token) => !/^(0|-?[1-9][0-9]*)$/.test(token))) {
    throw new Error('Every pointer offset must be a canonical signed decimal integer');
  }
  const minimum = -(1n << 63n);
  const maximum = (1n << 63n) - 1n;
  return tokens.map((token) => {
    const parsed = BigInt(token);
    if (parsed < minimum || parsed > maximum) {
      throw new Error('Pointer offset is outside the signed 64-bit range');
    }
    return parsed.toString(10);
  });
}

function bytesToArray(input: string): number[] {
  const trimmed = input.trim();
  if (!trimmed) throw new Error('Raw write requires at least one byte');
  const tokens = trimmed.split(/[\s,]+/);
  if (tokens.some((token) => !/^(?:0[xX])?[0-9A-Fa-f]{1,2}$/.test(token))) {
    throw new Error('Every raw byte must be one or two hexadecimal digits');
  }
  return tokens.map((token) => Number.parseInt(token.replace(/^0x/i, ''), 16));
}

function watchValueText(value: WatchValue | null): string {
  if (!value) return 'No sampled value';
  return value.display_value || value.canonical_value || '(empty value)';
}

function apiError(code: string | null | undefined, message: string | null, fallback: string): string {
  if (code && message) return `${code}: ${message}`;
  return code || message || fallback;
}

export default function Memory() {
  const session = useSession();
  const { run } = useQueryRunner();
  const lastWatchEventSeq = useRef(0);
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
  const [watches, setWatches] = useState<WatchItem[]>([]);
  const [watchError, setWatchError] = useState<string | null>(null);
  const [watchListLoading, setWatchListLoading] = useState(false);
  const [expandedWatchId, setExpandedWatchId] = useState<number | null>(null);
  const [watchSnapshot, setWatchSnapshot] = useState<WatchHistoryData | null>(null);
  const [watchSnapshotLoadingId, setWatchSnapshotLoadingId] = useState<number | null>(null);
  const [watchDrain, setWatchDrain] = useState<WatchDrainData | null>(null);
  const [watchDrainLoading, setWatchDrainLoading] = useState(false);

  const [consoleInput, setConsoleInput] = useState('');
  const [consoleLogs, setConsoleLogs] = useState<string[]>([
    'UExplorer local command adapter',
    'Supported: get/instances/mem.read/mem.write (object property set and calls are disabled)',
  ]);

  const rows = useMemo(() => {
    const result: Array<{ addr: string; chunk: string[]; ascii: string }> = [];
    try {
      parseAddress(currentAddress);
    } catch {
      return result;
    }
    for (let i = 0; i < hexBytes.length; i += 16) {
      const chunk = hexBytes.slice(i, i + 16);
      const addr = addAddress(currentAddress, i);
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
    setConsoleLogs((prev) => [...prev, line].slice(-500));
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
    let normalized: string;
    try {
      normalized = parseAddress(addr);
    } catch (error) {
      setReadError(error instanceof Error ? error.message : String(error));
      setLoading(false);
      return;
    }
    let res;
    try {
      res = await run('memory:read', async () => api.readMemory(normalized, READ_SIZE));
    } catch (error) {
      if (isAbortError(error)) return;
      setReadError(error instanceof Error ? error.message : String(error));
      setLoading(false);
      return;
    }
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
  }, [run, updateHistoryIndex]);

  const loadTypedValues = useCallback(async () => {
    const at = addAddress(currentAddress, cursorOffset);
    const types = ['byte', 'int32', 'uint32', 'int64', 'uint64', 'float', 'double', 'pointer'];

    let results;
    try {
      results = await run('memory:typed', async () => Promise.all(types.map((type) => api.readTypedMemory(at, type))));
    } catch (error) {
      if (isAbortError(error)) return;
      setReadError(error instanceof Error ? error.message : String(error));
      return;
    }
    const next: Record<string, string> = {};
    results.forEach((res, i) => {
      next[types[i]] = res.success && res.data ? String(res.data.value) : '-';
    });
    setTypedValues(next);
  }, [currentAddress, cursorOffset, run]);

  useEffect(() => {
    if (!currentAddress || currentAddress === '0x0') return;
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
    try {
      const at = addAddress(currentAddress, cursorOffset);
      const res = await api.writeTypedMemory(at, writeType, writeValue.trim());
      if (!res.success) {
        setReadError(res.error || 'Write failed');
        return;
      }
      await loadMemory(currentAddress, false);
    } catch (error) {
      setReadError(error instanceof Error ? error.message : String(error));
    }
  };

  const resolvePointerChain = async () => {
    try {
      const offsets = parseOffsets(pointerOffsets);
      const res = await api.resolvePointerChain(parseAddress(pointerBase), offsets);
      if (!res.success || !res.data) {
        setPointerResult(res.error || 'Pointer chain failed');
        return;
      }
      setPointerResult(JSON.stringify(res.data, null, 2));
    } catch (error) {
      setPointerResult(error instanceof Error ? error.message : String(error));
    }
  };

  const refreshWatches = useCallback(async () => {
    setWatchListLoading(true);
    setWatchError(null);
    try {
      const res = await api.listWatches();
      if (!res.success || !res.data) {
        setWatchError(apiError(res.error_code, res.error, 'Watch list unavailable'));
        return;
      }
      setWatches(res.data.subscriptions);
    } catch (error) {
      setWatchError(error instanceof Error ? error.message : String(error));
    } finally {
      setWatchListLoading(false);
    }
  }, []);

  useEffect(() => {
    lastWatchEventSeq.current = 0;
  }, [session.status?.pid]);

  useEffect(() => {
    const pending = session.watchEvents.filter(({ event }) => event.seq > lastWatchEventSeq.current);
    for (const { event, host_dropped_before: hostDroppedBefore } of pending) {
      lastWatchEventSeq.current = Math.max(lastWatchEventSeq.current, event.seq);
      const data = event.data as WatchPushEventData;
          const pushed: WatchEvent = {
            sequence: data.source_sequence,
            id: data.id,
            kind: event.kind.slice('watch.'.length) as WatchEvent['kind'],
            captured_at_monotonic_us: data.captured_at_monotonic_us,
            value: data.value,
            reason_code: data.reason_code,
            reason: data.reason,
            drop_count: data.push_dropped_before,
            coalesce_count: data.push_coalesced_before,
          };
          setWatchDrain((current) => ({
            events: [...(current?.events ?? []), pushed].slice(-32),
            count: Math.min((current?.events.length ?? 0) + 1, 32),
            dropped_total: Math.min(
              Number.MAX_SAFE_INTEGER,
              data.push_dropped_before
                + data.publisher_dropped_before
                + event.dropped_before
                + hostDroppedBefore,
            ),
            coalesced_total: data.push_coalesced_before,
            more_available: false,
          }));
          setWatches((current) => current.map((watch) => {
            if (watch.id !== data.id) return watch;
            if (data.source_sequence <= watch.last_change_sequence) return watch;
            return {
              ...watch,
              last_change_sequence: data.source_sequence,
              last_sampled_at_monotonic_us: data.captured_at_monotonic_us,
              sample_count: watch.sample_count + 1,
              failure_count: event.kind === 'watch.sample_failed'
                || event.kind === 'watch.sample_unavailable'
                || event.kind === 'watch.terminal_stale'
                ? watch.failure_count + 1 : watch.failure_count,
              history_count: data.value ? Math.min(watch.history_count + 1, 128) : watch.history_count,
              state: event.kind === 'watch.terminal_stale' ? 'terminal' : watch.state,
              terminal_reason_code: event.kind === 'watch.terminal_stale'
                ? data.reason_code : watch.terminal_reason_code,
              terminal_reason: event.kind === 'watch.terminal_stale'
                ? data.reason : watch.terminal_reason,
            };
          }));
          if (expandedWatchId === data.id) {
            setWatchSnapshot((current) => {
              if (!current || current.subscription.id !== data.id) return current;
              if (data.source_sequence <= current.subscription.last_change_sequence) return current;
              const terminal = event.kind === 'watch.terminal_stale';
              const subscription: WatchItem = {
                ...current.subscription,
                last_change_sequence: data.source_sequence,
                last_sampled_at_monotonic_us: data.captured_at_monotonic_us,
                sample_count: current.subscription.sample_count + 1,
                failure_count: event.kind === 'watch.sample_failed'
                  || event.kind === 'watch.sample_unavailable'
                  || event.kind === 'watch.terminal_stale'
                  ? current.subscription.failure_count + 1 : current.subscription.failure_count,
                history_count: data.value
                  ? current.subscription.history_count + 1 : current.subscription.history_count,
                state: terminal ? 'terminal' : current.subscription.state,
                terminal_reason_code: terminal
                  ? data.reason_code : current.subscription.terminal_reason_code,
                terminal_reason: terminal
                  ? data.reason : current.subscription.terminal_reason,
              };
              if (!data.value) {
                return { ...current, subscription };
              }
              const history = [
                ...current.history,
                {
                  sequence: data.source_sequence,
                  captured_at_monotonic_us: data.captured_at_monotonic_us,
                  value: data.value,
                },
              ].slice(-32);
              const retainedHistoryCount = Math.min(
                current.subscription.history_count + 1,
                128,
              );
              return {
                ...current,
                subscription: {
                  ...subscription,
                  history_count: retainedHistoryCount,
                },
                last_value: data.value,
                history,
                history_returned: history.length,
                history_total: retainedHistoryCount,
                history_truncated: current.history_truncated
                  || retainedHistoryCount > 32
                  || current.history.length >= 32,
              };
            });
          }
    }
  }, [expandedWatchId, session.watchEvents]);

  const setWatchEnabled = async (watch: WatchItem) => {
    setWatchError(null);
    try {
      const res = await api.setWatchEnabled(watch.id, watch.state !== 'enabled');
      if (!res.success) {
        setWatchError(apiError(res.error_code, res.error, 'Watch state update failed'));
        return;
      }
      await refreshWatches();
    } catch (error) {
      setWatchError(error instanceof Error ? error.message : String(error));
    }
  };

  const removeWatch = async (id: number) => {
    setWatchError(null);
    try {
      const res = await api.removeWatch(id);
      if (!res.success) {
        setWatchError(apiError(res.error_code, res.error, 'Watch removal failed'));
        return;
      }
      if (expandedWatchId === id) {
        setExpandedWatchId(null);
        setWatchSnapshot(null);
      }
      await refreshWatches();
    } catch (error) {
      setWatchError(error instanceof Error ? error.message : String(error));
    }
  };

  const fetchWatchSnapshot = async (id: number) => {
    setWatchSnapshotLoadingId(id);
    setWatchError(null);
    try {
      const res = await api.getWatchHistory(id, 32);
      if (!res.success || !res.data) {
        setWatchError(apiError(res.error_code, res.error, 'Watch snapshot unavailable'));
        return;
      }
      setWatchSnapshot(res.data);
    } catch (error) {
      setWatchError(error instanceof Error ? error.message : String(error));
    } finally {
      setWatchSnapshotLoadingId(null);
    }
  };

  const toggleWatchSnapshot = async (id: number) => {
    if (expandedWatchId === id) {
      setExpandedWatchId(null);
      setWatchSnapshot(null);
      return;
    }
    setExpandedWatchId(id);
    setWatchSnapshot(null);
    await fetchWatchSnapshot(id);
  };

  const drainWatchEvents = async () => {
    setWatchDrainLoading(true);
    setWatchError(null);
    try {
      const res = await api.drainWatchEvents(32);
      if (!res.success || !res.data) {
        setWatchError(apiError(res.error_code, res.error, 'Watch event drain unavailable'));
        return;
      }
      setWatchDrain(res.data);
    } catch (error) {
      setWatchError(error instanceof Error ? error.message : String(error));
    } finally {
      setWatchDrainLoading(false);
    }
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
        throw new Error('CALL_HANDLE_REQUIRED');
      } else if (head === 'instances' && parts[1]) {
        const out = await api.getClassInstances(parts[1], null, 100);
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
          <div className="flex items-center gap-2">
            <Bookmark className="w-3.5 h-3.5" />
            {t('Bookmarks')}
          </div>
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
            <button
              disabled={watchDrainLoading}
              onClick={() => void drainWatchEvents()}
              className="ml-auto rounded border border-border-subtle px-2 py-0.5 text-[10px] text-text-mid hover:text-text-high disabled:opacity-40"
              title={t('Drain watch events')}
            >
              {watchDrainLoading ? t('Draining...') : t('Drain Events')}
            </button>
            <button
              disabled={watchListLoading}
              onClick={() => void refreshWatches()}
              className="ml-2 text-text-low hover:text-text-high transition-colors disabled:opacity-40"
              title={t('Refresh')}
            >
              <RefreshCw className={`w-3.5 h-3.5 ${watchListLoading ? 'animate-spin' : ''}`} />
            </button>
          </div>
          <div className="flex-1 p-4 overflow-auto space-y-2">
            <DomainError message={watchError} compact />
            {watchDrain && (
              <div className="rounded-lg border border-border-subtle bg-background-base p-3 space-y-2">
                <div className="flex items-center justify-between text-[10px] text-text-low">
                  <span>{watchDrain.count} events</span>
                  <span>{watchDrain.more_available ? t('More pending') : t('Queue drained')}</span>
                </div>
                <div className="grid grid-cols-2 gap-2 text-[10px] font-mono">
                  <span className={watchDrain.dropped_total > 0 ? 'text-accent-red' : 'text-text-mid'}>
                    dropped {watchDrain.dropped_total}
                  </span>
                  <span className={watchDrain.coalesced_total > 0 ? 'text-accent-yellow' : 'text-text-mid'}>
                    coalesced {watchDrain.coalesced_total}
                  </span>
                </div>
                {watchDrain.events.length === 0 ? (
                  <div className="text-[10px] text-text-low">{t('No pending watch events.')}</div>
                ) : (
                  <div className="max-h-40 overflow-auto space-y-1.5">
                    {watchDrain.events.map((event) => (
                      <div key={event.sequence} className="rounded border border-border-subtle bg-surface-dark p-2 space-y-1">
                        <div className="flex items-center gap-2 text-[10px] font-mono">
                          <span className="text-primary">#{event.sequence}</span>
                          <span className="text-text-low">watch {event.id}</span>
                          <span className="ml-auto text-text-mid">{event.kind}</span>
                        </div>
                        <div className="break-all text-[10px] text-text-high font-mono">
                          {event.value ? watchValueText(event.value) : event.reason || event.reason_code || t('No value')}
                        </div>
                        {(event.drop_count > 0 || event.coalesce_count > 0) && (
                          <div className="text-[10px] text-accent-yellow font-mono">
                            drop +{event.drop_count}, coalesce +{event.coalesce_count}
                          </div>
                        )}
                      </div>
                    ))}
                  </div>
                )}
              </div>
            )}
            {!watchListLoading && watches.length === 0 && (
              <div className="rounded-lg border border-border-subtle bg-background-base p-3 text-xs text-text-low font-display">
                {t('No active property watches. Add one from an object property.')}
              </div>
            )}
            {watches.map((watch) => (
              <div key={watch.id} className="rounded-lg border border-border-subtle bg-background-base p-3 space-y-2">
                <div className="flex items-center gap-2">
                  <span className="font-mono text-[11px] text-primary">#{watch.id}</span>
                  <span className="text-[10px] uppercase text-text-low">{watch.state}</span>
                  <span className="ml-auto text-[10px] text-text-low">{watch.sample_count} samples</span>
                </div>
                <div className="font-mono text-[11px] text-text-high break-all">
                  {watch.spec.declaring_type_path}.{watch.spec.property_name}
                  {watch.spec.array_index > 0 ? `[${watch.spec.array_index}]` : ''}
                </div>
                <div className="grid grid-cols-3 gap-1 text-[9px] text-text-low font-mono">
                  <span>history {watch.history_count}</span>
                  <span>fail {watch.failure_count}</span>
                  <span className={watch.history_drop_count > 0 ? 'text-accent-yellow' : ''}>
                    drop {watch.history_drop_count}
                  </span>
                </div>
                {watch.terminal_reason && (
                  <div className="rounded border border-accent-red/20 bg-accent-red/5 p-2 text-[10px] text-accent-red">
                    {watch.terminal_reason_code ? `${watch.terminal_reason_code}: ` : ''}{watch.terminal_reason}
                  </div>
                )}
                <button
                  disabled={watchSnapshotLoadingId !== null && watchSnapshotLoadingId !== watch.id}
                  onClick={() => void toggleWatchSnapshot(watch.id)}
                  className="w-full rounded border border-border-subtle py-1 text-[10px] text-text-mid hover:text-text-high disabled:opacity-40"
                >
                  {watchSnapshotLoadingId === watch.id
                    ? t('Loading Snapshot...')
                    : expandedWatchId === watch.id
                      ? t('Hide Snapshot')
                      : t('Load Snapshot')}
                </button>
                {expandedWatchId === watch.id && watchSnapshot?.subscription.id === watch.id && (
                  <div className="rounded border border-border-subtle bg-surface-dark p-2 space-y-2">
                    <div className="flex items-center justify-between">
                      <span className="text-[10px] font-bold uppercase tracking-wide text-text-low">{t('Last Value')}</span>
                      <button
                        disabled={watchSnapshotLoadingId === watch.id}
                        onClick={() => void fetchWatchSnapshot(watch.id)}
                        className="text-[10px] text-primary hover:underline disabled:opacity-40"
                      >
                        {t('Refresh')}
                      </button>
                    </div>
                    <div className="break-all text-[10px] text-text-high font-mono">
                      {watchValueText(watchSnapshot.last_value)}
                    </div>
                    {watchSnapshot.last_value && (
                      <div className="text-[9px] text-text-low font-mono break-all">
                        {watchSnapshot.last_value.type_name} / {watchSnapshot.last_value.canonical_value}
                      </div>
                    )}
                    <div className="flex items-center justify-between text-[9px] text-text-low">
                      <span>{watchSnapshot.history_returned}/{watchSnapshot.history_total} history</span>
                      {watchSnapshot.history_truncated && <span className="text-accent-yellow">{t('truncated')}</span>}
                    </div>
                    {watchSnapshot.history.length === 0 ? (
                      <div className="text-[10px] text-text-low">{t('No samples recorded.')}</div>
                    ) : (
                      <div className="max-h-40 overflow-auto space-y-1">
                        {watchSnapshot.history.map((entry) => (
                          <div key={entry.sequence} className="border-t border-border-subtle pt-1 text-[10px] font-mono">
                            <div className="flex justify-between text-text-low">
                              <span>#{entry.sequence}</span>
                              <span>{entry.captured_at_monotonic_us} us</span>
                            </div>
                            <div className="break-all text-text-high">{watchValueText(entry.value)}</div>
                          </div>
                        ))}
                      </div>
                    )}
                  </div>
                )}
                <div className="flex gap-2">
                  <button
                    disabled={watch.state === 'terminal'}
                    onClick={() => void setWatchEnabled(watch)}
                    className="flex-1 rounded border border-border-subtle py-1 text-[10px] text-text-mid hover:text-text-high disabled:opacity-40"
                  >
                    {watch.state === 'enabled' ? t('Disable') : t('Enable')}
                  </button>
                  <button
                    onClick={() => void removeWatch(watch.id)}
                    className="flex-1 rounded border border-accent-red/20 py-1 text-[10px] text-accent-red hover:bg-accent-red/10"
                  >
                    {t('Remove')}
                  </button>
                </div>
              </div>
            ))}
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
