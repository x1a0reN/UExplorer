import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Search, Filter, TerminalSquare, Play, Info, List, History, Power, RefreshCw, Cpu } from 'lucide-react';
import { t } from '../i18n';
import api, { type ClassFunction, type HookItem, type HookLogEntry, type ObjectDetail, type ObjectItem } from '../api';

type FunctionTab = 'Info' | 'Parameters' | 'Call' | 'Hook' | 'Decompile';
type FlagTab = 'All' | 'Native' | 'Blueprint';
type FunctionsViewMode = 'function' | 'hookManager';
type CallMode = 'instance' | 'static' | 'batch';

interface FunctionsProps {
  viewMode?: FunctionsViewMode;
  onViewModeChange?: (mode: FunctionsViewMode) => void;
}

interface FunctionItem {
  index: number;
  name: string;
  className: string;
  address: string;
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

function parseObjectIndices(raw: string): number[] {
  const seen = new Set<number>();
  raw
    .split(/[,\s]+/)
    .map((v) => v.trim())
    .filter(Boolean)
    .forEach((v) => {
      const parsed = Number(v);
      if (!Number.isNaN(parsed) && parsed >= 0) {
        seen.add(parsed);
      }
    });
  return [...seen];
}

function extractFunctionParts(detail: ObjectDetail | null): { className: string; functionName: string; functionPath: string } {
  if (!detail?.full_name) return { className: '', functionName: detail?.name || '', functionPath: '' };
  const full = detail.full_name;
  const spaced = full.includes(' ') ? full.split(' ').slice(1).join(' ') : full;
  const chunks = spaced.split('.');
  const functionName = detail.name;
  const className = chunks.length >= 2 ? chunks[chunks.length - 2] : detail.class;
  const functionPath = className && functionName ? `${className}.${functionName}` : '';
  return { className, functionName, functionPath };
}

export default function Functions({ viewMode = 'function', onViewModeChange }: FunctionsProps) {
  const [activeTab, setActiveTab] = useState<FunctionTab>('Call');
  const [flagTab, setFlagTab] = useState<FlagTab>('All');
  const [search, setSearch] = useState('');
  const [classFilter, setClassFilter] = useState('');
  const [items, setItems] = useState<FunctionItem[]>([]);
  const [selected, setSelected] = useState<FunctionItem | null>(null);
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState<string | null>(null);

  const [detail, setDetail] = useState<ObjectDetail | null>(null);
  const [functionMeta, setFunctionMeta] = useState<ClassFunction | null>(null);
  const [detailLoading, setDetailLoading] = useState(false);
  const [detailError, setDetailError] = useState<string | null>(null);

  const [targetIndex, setTargetIndex] = useState('');
  const [paramInputs, setParamInputs] = useState<Record<string, string>>({});
  const [callMode, setCallMode] = useState<CallMode>('instance');
  const [staticClassName, setStaticClassName] = useState('');
  const [batchObjectIndices, setBatchObjectIndices] = useState('');
  const [callResult, setCallResult] = useState<string>('');
  const [calling, setCalling] = useState(false);

  const [hooks, setHooks] = useState<HookItem[]>([]);
  const [hookLog, setHookLog] = useState<HookLogEntry[]>([]);
  const [hookBusy, setHookBusy] = useState(false);
  const [hookFilterKeyword, setHookFilterKeyword] = useState('');
  const [hookFilterClass, setHookFilterClass] = useState('');
  const [hookPage, setHookPage] = useState(1);
  const [managerSelectedHookId, setManagerSelectedHookId] = useState<number | null>(null);
  const [hookLogPage, setHookLogPage] = useState(1);
  const hookPollTimerRef = useRef<number | null>(null);
  const hookPollInFlightRef = useRef(false);
  const hookReconnectTimerRef = useRef<number | null>(null);
  const hookReconnectDelayRef = useRef(800);

  const [bytecode, setBytecode] = useState('');
  const [decompiled, setDecompiled] = useState('');
  const [blueprintPath, setBlueprintPath] = useState('');
  const [decompileLoading, setDecompileLoading] = useState(false);

  const functionTabs = useMemo(
    () => [
      { id: 'Info' as FunctionTab, icon: Info },
      { id: 'Parameters' as FunctionTab, icon: List },
      { id: 'Call' as FunctionTab, icon: Play },
      { id: 'Hook' as FunctionTab, icon: Power },
      { id: 'Decompile' as FunctionTab, icon: TerminalSquare },
    ],
    []
  );

  const currentParts = extractFunctionParts(detail);
  const currentHook = hooks.find((h) => h.function_path === currentParts.functionPath);
  const activeHookId = viewMode === 'hookManager' ? (managerSelectedHookId ?? 0) : (currentHook?.id ?? 0);
  const hookPageSize = 50;
  const hookLogPageSize = 100;

  const filteredHooks = useMemo(() => {
    const keyword = hookFilterKeyword.trim().toLowerCase();
    const classKw = hookFilterClass.trim().toLowerCase();
    return hooks.filter((h) => {
      const path = h.function_path.toLowerCase();
      if (keyword && !path.includes(keyword)) return false;
      if (classKw) {
        const className = path.includes('.') ? path.split('.')[0] : path;
        if (!className.includes(classKw)) return false;
      }
      return true;
    });
  }, [hookFilterClass, hookFilterKeyword, hooks]);

  const pagedHooks = useMemo(() => {
    const start = (hookPage - 1) * hookPageSize;
    return filteredHooks.slice(start, start + hookPageSize);
  }, [filteredHooks, hookPage]);

  const pagedHookLogs = useMemo(() => {
    const start = (hookLogPage - 1) * hookLogPageSize;
    return hookLog.slice(start, start + hookLogPageSize);
  }, [hookLog, hookLogPage]);

  const hookTotalPages = Math.max(1, Math.ceil(filteredHooks.length / hookPageSize));
  const hookLogTotalPages = Math.max(1, Math.ceil(hookLog.length / hookLogPageSize));

  useEffect(() => {
    void loadFunctions();
  }, [search, classFilter, flagTab]);

  useEffect(() => {
    if (!selected) return;
    void loadFunctionDetail(selected.index);
  }, [selected]);

  useEffect(() => {
    setHookPage(1);
  }, [hookFilterKeyword, hookFilterClass]);

  useEffect(() => {
    setHookLogPage(1);
  }, [activeHookId]);

  useEffect(() => {
    if (hookPage > hookTotalPages) {
      setHookPage(hookTotalPages);
    }
  }, [hookPage, hookTotalPages]);

  useEffect(() => {
    if (hookLogPage > hookLogTotalPages) {
      setHookLogPage(hookLogTotalPages);
    }
  }, [hookLogPage, hookLogTotalPages]);

  useEffect(() => {
    if (viewMode !== 'hookManager') return;
    if (managerSelectedHookId && hooks.some((h) => h.id === managerSelectedHookId)) return;
    setManagerSelectedHookId(hooks.length > 0 ? hooks[0].id : null);
  }, [hooks, managerSelectedHookId, viewMode]);

  const refreshHooks = useCallback(async () => {
    const res = await api.listHooks();
    if (res.success && res.data) {
      setHooks(res.data.hooks);
    }
  }, []);

  const refreshHookLog = useCallback(async () => {
    const res = await api.getHookLog(activeHookId);
    if (res.success && res.data) {
      setHookLog(res.data.entries.slice(-1000).reverse());
    }
  }, [activeHookId]);

  useEffect(() => {
    if (activeHookId > 0) {
      void refreshHookLog();
    } else {
      setHookLog([]);
    }
  }, [activeHookId, refreshHookLog]);

  const stopHookPolling = useCallback(() => {
    if (hookPollTimerRef.current !== null) {
      window.clearInterval(hookPollTimerRef.current);
      hookPollTimerRef.current = null;
    }
  }, []);

  const runHookFallbackTick = useCallback(async () => {
    if (hookPollInFlightRef.current) return;
    hookPollInFlightRef.current = true;
    try {
      await refreshHooks();
      await refreshHookLog();
    } finally {
      hookPollInFlightRef.current = false;
    }
  }, [refreshHookLog, refreshHooks]);

  const startHookPolling = useCallback(
    (immediate = true) => {
      if (hookPollTimerRef.current !== null) return;
      if (immediate) {
        void runHookFallbackTick();
      }
      hookPollTimerRef.current = window.setInterval(() => {
        void runHookFallbackTick();
      }, 300);
    },
    [runHookFallbackTick]
  );

  const clearHookReconnectTimer = useCallback(() => {
    if (hookReconnectTimerRef.current !== null) {
      window.clearTimeout(hookReconnectTimerRef.current);
      hookReconnectTimerRef.current = null;
    }
  }, []);

  useEffect(() => {
    if (activeTab !== 'Hook' && viewMode !== 'hookManager') {
      clearHookReconnectTimer();
      hookReconnectDelayRef.current = 800;
      stopHookPolling();
      return;
    }

    let disposed = false;
    let unsubscribe: (() => void) | null = null;

    const connect = () => {
      if (disposed) return;
      unsubscribe = api.subscribeEventStream(
        '/events/hooks',
        () => {
          void refreshHookLog();
          void refreshHooks();
        },
        {
          onOpen: () => {
            clearHookReconnectTimer();
            hookReconnectDelayRef.current = 800;
            stopHookPolling();
            void refreshHookLog();
            void refreshHooks();
          },
          onError: () => {
            if (disposed) return;
            startHookPolling(true);
          },
          onClose: (reason) => {
            if (disposed || reason === 'abort') return;
            startHookPolling(true);

            if (hookReconnectTimerRef.current !== null) return;
            const delay = hookReconnectDelayRef.current;
            hookReconnectTimerRef.current = window.setTimeout(() => {
              hookReconnectTimerRef.current = null;
              connect();
            }, delay);
            hookReconnectDelayRef.current = Math.min(hookReconnectDelayRef.current * 2, 5000);
          },
        }
      );
    };

    connect();
    return () => {
      disposed = true;
      unsubscribe?.();
      clearHookReconnectTimer();
      hookReconnectDelayRef.current = 800;
      stopHookPolling();
    };
  }, [activeTab, clearHookReconnectTimer, refreshHookLog, refreshHooks, startHookPolling, stopHookPolling, viewMode]);

  const loadFunctions = async () => {
    setListLoading(true);
    setListError(null);
    try {
      const res = await api.searchObjects(search.trim(), {
        class: 'Function',
        package: classFilter.trim() || undefined,
        offset: 0,
        limit: 300,
      });
      if (!res.success || !res.data) throw new Error(res.error || t('Failed to load functions'));

      let next = res.data.items.map((it: ObjectItem) => ({
        index: it.index,
        name: it.name,
        className: it.class,
        address: it.address,
      }));

      if (flagTab !== 'All') {
        next = next.filter((it) => {
          const n = it.name.toLowerCase();
          if (flagTab === 'Native') return n.includes('native') || !n.includes('bp');
          return n.includes('bp') || n.includes('k2_') || n.includes('blueprint');
        });
      }

      setItems(next);
    } catch (error) {
      setListError(error instanceof Error ? error.message : String(error));
      setItems([]);
    } finally {
      setListLoading(false);
    }
  };

  const loadFunctionDetail = async (index: number) => {
    setDetailLoading(true);
    setDetailError(null);
    setFunctionMeta(null);
    setCallResult('');
    setBytecode('');
    setDecompiled('');

    try {
      const detailRes = await api.getObjectByIndex(index);
      if (!detailRes.success || !detailRes.data) {
        throw new Error(detailRes.error || t('Failed to load function detail'));
      }
      setDetail(detailRes.data);

      const parts = extractFunctionParts(detailRes.data);
      if (!parts.className) return;
      setStaticClassName(parts.className);
      setBlueprintPath(parts.functionPath);

      const classFuncRes = await api.getClassFunctions(parts.className);
      if (!classFuncRes.success || !classFuncRes.data) {
        throw new Error(classFuncRes.error || t('Failed to load class functions'));
      }
      const found = classFuncRes.data.find((f) => f.name === parts.functionName) || null;
      setFunctionMeta(found);

      if (found) {
        setCallMode(found.flags.toLowerCase().includes('static') ? 'static' : 'instance');
        const inputMap: Record<string, string> = {};
        found.params
          .filter((p) => !p.flags.includes('OutParm') && !p.flags.includes('ReturnParm'))
          .forEach((p) => {
            inputMap[p.name] = '';
          });
        setParamInputs(inputMap);
      }

      const classSearch = await api.searchObjects(parts.className, { class: 'Class', limit: 1 });
      if (classSearch.success && classSearch.data && classSearch.data.items.length > 0) {
        setTargetIndex(String(classSearch.data.items[0].index));
      } else {
        setTargetIndex('');
      }

      await refreshHookLog();
      await refreshHooks();
    } catch (error) {
      setDetailError(error instanceof Error ? error.message : String(error));
    } finally {
      setDetailLoading(false);
    }
  };

  const executeCall = async () => {
    if (!detail) return;

    const parseParamValue = async (raw: string): Promise<unknown> => {
      const trimmed = raw.trim();
      const enumMatch = trimmed.match(/^([A-Za-z0-9_]+)::([A-Za-z0-9_]+)$/);
      if (!enumMatch) return parseInputValue(raw);

      const [, enumName, enumValueName] = enumMatch;
      const enumRes = await api.getEnumByName(enumName);
      if (!enumRes.success || !enumRes.data) {
        return parseInputValue(raw);
      }

      const found = enumRes.data.values.find((v) => v.name === enumValueName || v.name.endsWith(`::${enumValueName}`));
      return found ? found.value : parseInputValue(raw);
    };

    const params: Record<string, unknown> = {};
    for (const [name, raw] of Object.entries(paramInputs)) {
      params[name] = await parseParamValue(raw);
    }

    setCalling(true);
    try {
      if (callMode === 'static') {
        if (!staticClassName.trim()) {
          setCallResult('Static call requires class name');
          return;
        }

        const classCheck = await api.getClassByName(staticClassName.trim());
        if (!classCheck.success || !classCheck.data) {
          setCallResult(classCheck.error || 'Class not found');
          return;
        }

        const res = await api.callStaticFunction(detail.name, {
          className: staticClassName.trim(),
          params,
          useGameThread: true,
        });
        if (!res.success || !res.data) {
          setCallResult(res.error || 'Static call failed');
          return;
        }
        setCallResult(JSON.stringify(res.data, null, 2));
        return;
      }

      if (callMode === 'batch') {
        const indices = parseObjectIndices(batchObjectIndices);
        if (indices.length === 0) {
          setCallResult('Batch call requires object indices, e.g. 100,101,102');
          return;
        }

        const res = await api.callFunctionBatch(indices, detail.name, params, true);
        if (!res.success || !res.data) {
          setCallResult(res.error || 'Batch call failed');
          return;
        }
        setCallResult(JSON.stringify(res.data, null, 2));
        return;
      }

      if (!targetIndex.trim()) {
        setCallResult('Target object index is required');
        return;
      }

      const objectIndex = Number(targetIndex);
      if (Number.isNaN(objectIndex)) {
        setCallResult('Target object index is invalid');
        return;
      }

      const res = await api.callFunction(objectIndex, detail.name, params, true);
      if (!res.success || !res.data) {
        setCallResult(res.error || 'Call failed');
        return;
      }
      setCallResult(JSON.stringify(res.data.result, null, 2));
    } finally {
      setCalling(false);
    }
  };

  const addHook = async () => {
    if (!currentParts.functionPath) return;
    setHookBusy(true);
    const res = await api.addHook(currentParts.functionPath);
    setHookBusy(false);
    if (!res.success) {
      setDetailError(res.error || 'Failed to add hook');
      return;
    }
    await refreshHooks();
  };

  const toggleHook = async () => {
    if (!currentHook) return;
    setHookBusy(true);
    const res = await api.setHookEnabled(currentHook.id, !currentHook.enabled);
    setHookBusy(false);
    if (!res.success) {
      setDetailError(res.error || 'Failed to update hook');
      return;
    }
    await refreshHooks();
  };

  const removeHook = async () => {
    if (!currentHook) return;
    setHookBusy(true);
    const res = await api.removeHook(currentHook.id);
    setHookBusy(false);
    if (!res.success) {
      setDetailError(res.error || 'Failed to remove hook');
      return;
    }
    await refreshHooks();
  };

  const bulkSetHooksEnabled = async (enabled: boolean) => {
    const targets = filteredHooks;
    if (targets.length === 0) return;
    setHookBusy(true);
    try {
      for (const hook of targets) {
        if (hook.enabled === enabled) continue;
        await api.setHookEnabled(hook.id, enabled);
      }
      await refreshHooks();
      await refreshHookLog();
    } finally {
      setHookBusy(false);
    }
  };

  const loadDecompile = async () => {
    if (!selected) return;
    setDecompileLoading(true);
    const path = blueprintPath.trim();
    let byteRes;
    let decompileRes;

    if (path) {
      [byteRes, decompileRes] = await Promise.all([
        api.getBlueprintBytecodeByPath(path),
        api.decompileBlueprintByPath(path),
      ]);
      if (!byteRes.success || !decompileRes.success) {
        [byteRes, decompileRes] = await Promise.all([
          api.getBlueprintBytecode(selected.index),
          api.decompileBlueprint(selected.index),
        ]);
      }
    } else {
      [byteRes, decompileRes] = await Promise.all([
        api.getBlueprintBytecode(selected.index),
        api.decompileBlueprint(selected.index),
      ]);
    }

    if (byteRes.success && byteRes.data) {
      setBytecode(byteRes.data.hex);
    } else {
      setBytecode(byteRes.error || 'No bytecode');
    }
    if (decompileRes.success && decompileRes.data) {
      setDecompiled(decompileRes.data.pseudocode);
    } else {
      setDecompiled(decompileRes.error || 'No pseudocode');
    }
    setDecompileLoading(false);
  };

  return (
    <div className="flex-1 flex overflow-hidden bg-background-base">
      <div className="w-[340px] flex-none border-r border-border-subtle flex flex-col bg-surface-dark relative z-10 backdrop-blur-md">
        <div className="p-4 border-b border-border-subtle space-y-3">
          <div className="relative group">
            <Search className="w-4 h-4 text-text-low absolute left-3 top-2.5" />
            <input
              type="text"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              placeholder="ClassName::FunctionName..."
              className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary transition-all font-mono placeholder:text-text-low/50"
            />
          </div>
          <div className="relative group">
            <Filter className="w-4 h-4 text-text-low absolute left-3 top-2.5" />
            <input
              type="text"
              value={classFilter}
              onChange={(e) => setClassFilter(e.target.value)}
              placeholder="Package/class filter"
              className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary transition-all font-mono placeholder:text-text-low/50"
            />
          </div>

          <div className="flex items-center justify-between">
            <div className="flex gap-1 bg-surface-stripe p-1 rounded-lg border border-border-subtle">
              {(['All', 'Native', 'Blueprint'] as FlagTab[]).map((tab) => (
                <button
                  key={tab}
                  onClick={() => setFlagTab(tab)}
                  className={`px-2.5 py-1 rounded-md text-[11px] font-semibold tracking-tight transition-colors font-display ${flagTab === tab ? 'bg-background-base text-text-high shadow-sm border border-border-subtle' : 'text-text-low hover:text-text-high border border-transparent'
                    }`}
                >
                  {tab}
                </button>
              ))}
            </div>
            <button
              onClick={() => void loadFunctions()}
              className="w-7 h-7 flex items-center justify-center rounded-lg hover:bg-surface-stripe text-text-low hover:text-text-high transition-colors"
            >
              <RefreshCw className="w-3.5 h-3.5" />
            </button>
          </div>
        </div>

        <div className="flex-1 overflow-y-auto p-2 space-y-1 custom-scrollbar">
          {listLoading && <div className="text-text-low text-xs p-3 font-display">Loading...</div>}
          {listError && <div className="text-accent-red text-xs p-3 font-display">{listError}</div>}
          {!listLoading && !listError && items.length === 0 && <div className="text-text-low text-xs p-3 font-display">No functions</div>}
          {items.map((item) => {
            const active = selected?.index === item.index;
            return (
              <div
                key={item.index}
                onClick={() => setSelected(item)}
                className={`flex items-center gap-3 px-3 py-2 rounded-xl cursor-pointer transition-colors ${active ? 'bg-primary text-white shadow-sm' : 'hover:bg-surface-stripe text-text-mid'
                  }`}
              >
                <div className={`w-7 h-7 rounded-lg flex items-center justify-center flex-none ${active ? 'bg-white/20' : 'bg-background-base border border-border-subtle'}`}>
                  <TerminalSquare className={`w-3.5 h-3.5 ${active ? 'text-white' : 'text-primary'}`} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className={`text-[12px] font-mono truncate ${active ? 'text-white' : 'text-text-high'}`}>{item.name}</div>
                  <div className={`text-[10px] font-bold tracking-widest truncate font-display ${active ? 'text-white/70' : 'text-text-low'}`}>{item.className}</div>
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className="flex-1 flex flex-col min-w-0 bg-background-base relative">
        <div className="h-14 border-b border-border-subtle bg-surface-dark backdrop-blur-3xl flex items-center px-6 gap-6 z-20">
          <div className="flex items-center gap-1 bg-background-base border border-border-subtle rounded-lg p-1">
            <button
              onClick={() => onViewModeChange?.('function')}
              className={`px-3 py-1.5 rounded-md text-xs font-semibold font-display transition-colors ${viewMode === 'function' ? 'bg-surface-dark text-text-high shadow-sm border border-border-subtle' : 'text-text-low hover:text-text-high border border-transparent'}`}
            >
              函数工作台
            </button>
            <button
              onClick={() => onViewModeChange?.('hookManager')}
              className={`px-3 py-1.5 rounded-md text-xs font-semibold font-display transition-colors ${viewMode === 'hookManager' ? 'bg-surface-dark text-text-high shadow-sm border border-border-subtle' : 'text-text-low hover:text-text-high border border-transparent'}`}
            >
              Hook 管理
            </button>
          </div>
          {viewMode === 'function' && (
            <nav className="flex items-center gap-4">
              {functionTabs.map((tab) => (
                <button
                  key={tab.id}
                  onClick={() => setActiveTab(tab.id)}
                  className={`relative h-14 flex items-center gap-2 text-[13px] font-medium tracking-tight transition-colors font-display ${activeTab === tab.id ? 'text-text-high' : 'text-text-low hover:text-text-high'
                    }`}
                >
                  <tab.icon className={`w-4 h-4 ${activeTab === tab.id ? 'text-primary' : 'text-text-low'}`} />
                  {tab.id}
                  {activeTab === tab.id && (
                    <div className="absolute bottom-0 left-0 right-0 h-[2px] bg-primary rounded-t-full shadow-[0_-2px_8px_rgba(10,132,255,0.3)]" />
                  )}
                </button>
              ))}
            </nav>
          )}
        </div>

        <div className="flex-1 overflow-auto p-8 relative">
          {viewMode === 'hookManager' ? (
            <div className="max-w-6xl space-y-6">
              <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 shadow-sm">
                <div className="grid grid-cols-4 gap-4 text-sm font-display">
                  <div>
                    <div className="text-text-low text-xs mb-1">Total Hooks</div>
                    <div className="text-text-high text-xl font-mono">{hooks.length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">Enabled</div>
                    <div className="text-accent-green text-xl font-mono">{hooks.filter((h) => h.enabled).length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">Filtered</div>
                    <div className="text-primary text-xl font-mono">{filteredHooks.length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">Selected Hook</div>
                    <div className="text-text-high text-sm font-mono truncate bg-background-base px-2 py-1 rounded inline-block border border-border-subtle">
                      {hooks.find((h) => h.id === managerSelectedHookId)?.function_path || '-'}
                    </div>
                  </div>
                </div>
              </div>

              <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                <div className="grid grid-cols-[1fr_1fr_auto_auto_auto] gap-3">
                  <input
                    type="text"
                    value={hookFilterKeyword}
                    onChange={(e) => setHookFilterKeyword(e.target.value)}
                    placeholder={t('Search function name...')}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-2 outline-none focus:border-primary font-mono placeholder:text-text-low/50"
                  />
                  <input
                    type="text"
                    value={hookFilterClass}
                    onChange={(e) => setHookFilterClass(e.target.value)}
                    placeholder={t('Search class name...')}
                    className="bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-2 outline-none focus:border-primary font-mono placeholder:text-text-low/50"
                  />
                  <button
                    onClick={() => void bulkSetHooksEnabled(true)}
                    disabled={hookBusy || filteredHooks.length === 0}
                    className="px-4 py-2 rounded-lg bg-accent-green/10 hover:bg-accent-green/20 text-accent-green text-xs font-display border border-accent-green/20 transition-colors disabled:opacity-50"
                  >
                    批量启用
                  </button>
                  <button
                    onClick={() => void bulkSetHooksEnabled(false)}
                    disabled={hookBusy || filteredHooks.length === 0}
                    className="px-4 py-2 rounded-lg bg-accent-yellow/10 hover:bg-accent-yellow/20 text-accent-yellow text-xs font-display border border-accent-yellow/20 transition-colors disabled:opacity-50"
                  >
                    批量停用
                  </button>
                  <button
                    onClick={() => {
                      void refreshHooks();
                      void refreshHookLog();
                    }}
                    className="px-4 py-2 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high text-xs font-display border border-border-subtle transition-colors"
                  >
                    刷新
                  </button>
                </div>

                <div className="border border-border-subtle rounded-xl overflow-hidden bg-background-base">
                  <div className="grid grid-cols-[90px_1fr_120px_140px] bg-surface-dark text-text-low text-[10px] uppercase font-bold tracking-widest px-4 py-2.5 font-display border-b border-border-subtle">
                    <div>ID</div>
                    <div>Function Path</div>
                    <div>Status</div>
                    <div>Hit Count</div>
                  </div>
                  <div className="max-h-[380px] overflow-auto">
                    {pagedHooks.map((hook) => (
                      <button
                        key={hook.id}
                        onClick={() => setManagerSelectedHookId(hook.id)}
                        className={`w-full grid grid-cols-[90px_1fr_120px_140px] px-4 py-2.5 text-left text-xs border-b border-border-subtle transition-colors ${managerSelectedHookId === hook.id ? 'bg-primary/10 text-primary border-l-2 border-l-primary' : 'text-text-mid hover:bg-surface-stripe border-l-2 border-l-transparent'
                          }`}
                      >
                        <div className="font-mono">{hook.id}</div>
                        <div className="font-mono truncate">{hook.function_path}</div>
                        <div className="font-display">{hook.enabled ? <span className="text-accent-green">Enabled</span> : <span className="text-text-low">Disabled</span>}</div>
                        <div className="font-mono">{hook.hit_count}</div>
                      </button>
                    ))}
                    {pagedHooks.length === 0 && <div className="p-6 text-text-low text-xs font-display text-center">没有匹配的 Hook</div>}
                  </div>
                </div>

                <div className="flex items-center justify-between text-xs text-text-low font-display">
                  <div>第 {hookPage} / {hookTotalPages} 页（每页 {hookPageSize} 条）</div>
                  <div className="flex gap-2">
                    <button
                      onClick={() => setHookPage((p) => Math.max(1, p - 1))}
                      disabled={hookPage <= 1}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      上一页
                    </button>
                    <button
                      onClick={() => setHookPage((p) => Math.min(hookTotalPages, p + 1))}
                      disabled={hookPage >= hookTotalPages}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      下一页
                    </button>
                  </div>
                </div>
              </div>

              <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                <div className="flex items-center justify-between">
                  <h3 className="text-text-high font-semibold font-display">Hook Log</h3>
                  <div className="text-xs text-text-low font-mono bg-background-base px-2 py-1 rounded border border-border-subtle">
                    Active Hook ID: {managerSelectedHookId ?? '-'}
                  </div>
                </div>

                <div className="max-h-[320px] overflow-auto border border-border-subtle rounded-xl p-3 bg-background-base space-y-1.5">
                  {pagedHookLogs.map((entry, idx) => (
                    <div key={`${entry.timestamp}-${idx}`} className="text-xs font-mono text-text-high border-b border-border-subtle pb-1.5 pt-1">
                      <div className="text-text-low/60">{new Date(entry.timestamp).toLocaleTimeString()}</div>
                      <div className="text-primary mt-0.5">{entry.function_name}</div>
                    </div>
                  ))}
                  {pagedHookLogs.length === 0 && <div className="text-text-low text-xs font-display text-center py-4">No hook logs</div>}
                </div>
                <div className="flex items-center justify-between text-xs text-text-low font-display">
                  <div>第 {hookLogPage} / {hookLogTotalPages} 页（每页 {hookLogPageSize} 条）</div>
                  <div className="flex gap-2">
                    <button
                      onClick={() => setHookLogPage((p) => Math.max(1, p - 1))}
                      disabled={hookLogPage <= 1}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      上一页
                    </button>
                    <button
                      onClick={() => setHookLogPage((p) => Math.min(hookLogTotalPages, p + 1))}
                      disabled={hookLogPage >= hookLogTotalPages}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      下一页
                    </button>
                  </div>
                </div>
              </div>
            </div>
          ) : (
            <>
              {!selected && <div className="text-white/40 text-sm">Select a function on the left panel.</div>}
              {selected && (
                <div className="max-w-5xl space-y-6">
                  <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 relative overflow-hidden shadow-sm">
                    <div className="flex gap-5 relative z-10 items-center">
                      <div className="w-16 h-16 rounded-[14px] bg-primary/10 border border-primary/20 flex items-center justify-center shadow-lg flex-none">
                        <TerminalSquare className="w-8 h-8 text-primary stroke-[1.5]" />
                      </div>
                      <div className="flex-1">
                        <h1 className="text-[20px] font-mono text-text-high tracking-tight leading-tight mb-2">{detail?.full_name || `${selected.className}::${selected.name}`}</h1>
                        <div className="flex flex-wrap gap-2">
                          <span className="px-2.5 py-1 rounded-[6px] bg-accent-green/10 border border-accent-green/20 text-[11px] font-bold tracking-widest uppercase text-accent-green font-display">
                            {selected.className}
                          </span>
                          <span className="px-2.5 py-1 rounded-[6px] bg-background-base border border-border-subtle text-[11px] font-mono text-text-mid">
                            Param Size: {functionMeta?.param_size ?? '-'}
                          </span>
                          <span className="px-2.5 py-1 rounded-[6px] bg-background-base border border-border-subtle text-[11px] font-mono text-text-mid">
                            Flags: {functionMeta?.flags || '-'}
                          </span>
                        </div>
                      </div>
                    </div>
                  </div>

                  {detailLoading && <div className="text-white/40 text-sm">Loading function detail...</div>}
                  {detailError && <div className="text-red-300 text-sm">{detailError}</div>}

                  {activeTab === 'Info' && (
                    <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-2 text-sm shadow-sm">
                      <InfoLine k="Name" v={detail?.name || selected.name} />
                      <InfoLine k="Class" v={currentParts.className || selected.className} />
                      <InfoLine k="Address" v={functionMeta?.address || selected.address} />
                      <InfoLine k="Param Size" v={String(functionMeta?.param_size ?? '-')} />
                      <InfoLine k="Flags" v={functionMeta?.flags || '-'} />
                      <InfoLine k="Has Script" v={String(functionMeta?.has_script ?? false)} />
                    </div>
                  )}

                  {activeTab === 'Parameters' && (
                    <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 shadow-sm overflow-hidden">
                      <table className="w-full text-left text-xs">
                        <thead>
                          <tr className="text-text-low border-b border-border-subtle font-display">
                            <th className="py-2.5 font-medium">Name</th>
                            <th className="font-medium">Type</th>
                            <th className="font-medium">Flags</th>
                            <th className="font-medium">Offset</th>
                          </tr>
                        </thead>
                        <tbody>
                          {functionMeta?.params.map((p) => (
                            <tr key={`${p.name}-${p.offset}`} className="border-b border-border-subtle last:border-0 hover:bg-surface-stripe/30 transition-colors">
                              <td className="py-2.5 text-text-high font-mono">{p.name}</td>
                              <td className="text-text-mid font-mono">{p.type}</td>
                              <td className="text-text-low font-display text-[11px]">{p.flags}</td>
                              <td className="text-text-low font-mono">{p.offset}</td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                      {!functionMeta && <div className="text-text-low text-sm font-display text-center py-4">No metadata</div>}
                    </div>
                  )}

                  {activeTab === 'Call' && (
                    <div className="grid grid-cols-2 gap-6">
                      <div className="space-y-6">
                        <div className="bg-surface-dark border border-border-subtle rounded-xl overflow-hidden p-6 shadow-sm">
                          <h3 className="text-sm font-semibold text-text-high mb-4 tracking-tight font-display">Invoke Parameters</h3>
                          <div className="space-y-4">
                            <div className="space-y-1.5">
                              <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">Call Mode</label>
                              <div className="flex gap-2 bg-background-base p-1 rounded-lg border border-border-subtle inline-flex">
                                {([
                                  { id: 'instance' as CallMode, label: 'Instance' },
                                  { id: 'static' as CallMode, label: 'Static' },
                                  { id: 'batch' as CallMode, label: 'Batch' },
                                ]).map((mode) => (
                                  <button
                                    key={mode.id}
                                    onClick={() => setCallMode(mode.id)}
                                    className={`px-3 py-1.5 rounded-md text-xs font-semibold font-display transition-colors ${callMode === mode.id
                                      ? 'bg-surface-dark text-text-high shadow-sm border border-border-subtle'
                                      : 'text-text-low hover:text-text-high border border-transparent'
                                      }`}
                                  >
                                    {mode.label}
                                  </button>
                                ))}
                              </div>
                            </div>

                            {callMode === 'instance' && (
                              <div className="space-y-1.5">
                                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">Target Object Index</label>
                                <input
                                  type="text"
                                  value={targetIndex}
                                  onChange={(e) => setTargetIndex(e.target.value)}
                                  placeholder="Object index"
                                  className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                />
                              </div>
                            )}

                            {callMode === 'static' && (
                              <div className="space-y-1.5">
                                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">Target Class Name</label>
                                <input
                                  type="text"
                                  value={staticClassName}
                                  onChange={(e) => setStaticClassName(e.target.value)}
                                  placeholder="例如 BP_ItemGridWDT_C"
                                  className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                />
                              </div>
                            )}

                            {callMode === 'batch' && (
                              <div className="space-y-1.5">
                                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">Object Indices</label>
                                <input
                                  type="text"
                                  value={batchObjectIndices}
                                  onChange={(e) => setBatchObjectIndices(e.target.value)}
                                  placeholder="100,101,102"
                                  className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                />
                              </div>
                            )}

                            {functionMeta?.params
                              .filter((p) => !p.flags.includes('OutParm') && !p.flags.includes('ReturnParm'))
                              .map((p) => (
                                <div key={p.name} className="space-y-1.5">
                                  <label className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center justify-between font-display">
                                    <span>{p.name}</span>
                                    <span className="text-primary lowercase font-mono">{p.type}</span>
                                  </label>
                                  <input
                                    type="text"
                                    value={paramInputs[p.name] ?? ''}
                                    onChange={(e) =>
                                      setParamInputs((prev) => ({
                                        ...prev,
                                        [p.name]: e.target.value,
                                      }))
                                    }
                                    className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                  />
                                </div>
                              ))}

                            <div className="text-[11px] text-text-low font-display">
                              参数支持 JSON/数字/布尔值；枚举可使用 <span className="font-mono text-text-mid bg-surface-stripe px-1 py-0.5 rounded">EnumName::ValueName</span>
                            </div>

                            <button
                              onClick={() => void executeCall()}
                              disabled={calling}
                              className="w-full py-2.5 rounded-lg bg-primary hover:bg-primary/90 text-white font-semibold text-[13px] font-display tracking-tight shadow-sm active:scale-[0.98] transition-all flex items-center justify-center gap-2 mt-4 disabled:opacity-50"
                            >
                              <Play className="w-4 h-4 fill-current" />
                              {calling
                                ? 'Calling...'
                                : callMode === 'instance'
                                  ? 'Execute ProcessEvent'
                                  : callMode === 'static'
                                    ? 'Execute Static Call'
                                    : 'Execute Batch Call'}
                            </button>
                          </div>
                        </div>
                      </div>

                      <div className="space-y-6">
                        <div className="bg-surface-dark border border-border-subtle rounded-xl overflow-hidden p-6 flex flex-col h-full shadow-sm">
                          <div className="flex items-center justify-between mb-4">
                            <h3 className="text-sm font-semibold text-text-high tracking-tight font-display">Execution Result</h3>
                            <History className="w-4 h-4 text-text-low" />
                          </div>
                          <pre className="flex-1 bg-background-base border border-border-subtle rounded-xl p-4 font-mono text-[12px] text-accent-green overflow-y-auto whitespace-pre-wrap custom-scrollbar">
                            {callResult || 'No execution yet'}
                          </pre>
                        </div>
                      </div>
                    </div>
                  )}

                  {activeTab === 'Hook' && (
                    <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                      <div className="flex items-center gap-3">
                        <button
                          onClick={() => void addHook()}
                          disabled={hookBusy || !!currentHook || !currentParts.functionPath}
                          className="px-4 py-2 rounded-lg bg-primary hover:bg-primary/90 text-white text-xs font-display font-medium disabled:opacity-50 transition-colors shadow-sm"
                        >
                          Add Hook
                        </button>
                        <button
                          onClick={() => void toggleHook()}
                          disabled={hookBusy || !currentHook}
                          className="px-4 py-2 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high border border-border-subtle text-xs font-display font-medium disabled:opacity-50 transition-colors"
                        >
                          {currentHook?.enabled ? 'Disable' : 'Enable'}
                        </button>
                        <button
                          onClick={() => void removeHook()}
                          disabled={hookBusy || !currentHook}
                          className="px-4 py-2 rounded-lg bg-accent-red/10 hover:bg-accent-red/20 text-accent-red border border-accent-red/20 text-xs font-display font-medium disabled:opacity-50 transition-colors"
                        >
                          Remove
                        </button>
                        <button
                          onClick={() => void refreshHookLog()}
                          className="ml-auto w-8 h-8 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle flex items-center justify-center transition-colors shadow-sm"
                        >
                          <RefreshCw className="w-4 h-4 text-text-high" />
                        </button>
                      </div>

                      <div className="text-xs text-text-low font-mono bg-background-base p-2 rounded-lg border border-border-subtle inline-block font-display">
                        Path: {currentParts.functionPath || '-'} | Current Hook ID: {currentHook?.id ?? '-'} | Hit Count: <span className="text-text-high">{currentHook?.hit_count ?? 0}</span>
                      </div>

                      <div className="max-h-[300px] overflow-auto border border-border-subtle rounded-xl p-3 bg-background-base space-y-1.5 custom-scrollbar">
                        {hookLog.map((entry, idx) => (
                          <div key={`${entry.timestamp}-${idx}`} className="text-xs font-mono text-text-high border-b border-border-subtle pb-1.5 pt-1">
                            <div className="text-text-low/60">{new Date(entry.timestamp).toLocaleTimeString()}</div>
                            <div className="text-primary mt-0.5">{entry.function_name}</div>
                          </div>
                        ))}
                        {hookLog.length === 0 && <div className="text-text-low text-xs font-display text-center py-4">No hook logs</div>}
                      </div>
                    </div>
                  )}

                  {activeTab === 'Decompile' && (
                    <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                      <div className="space-y-1.5">
                        <div className="text-text-low text-[11px] font-bold uppercase tracking-widest font-display">Function Path（优先走 /blueprint/:funcpath/*）</div>
                        <input
                          type="text"
                          value={blueprintPath}
                          onChange={(e) => setBlueprintPath(e.target.value)}
                          placeholder="例如 BP_ItemGridWDT_C.ExecuteUbergraph_BP_ItemGridWDT"
                          className="w-full bg-background-base border border-border-subtle text-text-high text-[13px] font-mono rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                        />
                      </div>
                      <button
                        onClick={() => void loadDecompile()}
                        disabled={decompileLoading}
                        className="px-4 py-2.5 rounded-lg bg-primary hover:bg-primary/90 text-white text-xs font-display font-medium shadow-sm active:scale-[0.98] transition-all disabled:opacity-50 flex items-center justify-center gap-2"
                      >
                        <Cpu className="w-4 h-4" />
                        {decompileLoading ? 'Loading...' : 'Load Bytecode + Decompile'}
                      </button>

                      <div className="grid grid-cols-2 gap-4 pt-2">
                        <div>
                          <div className="text-text-low text-[10px] uppercase font-bold tracking-widest font-display mb-1.5">Bytecode</div>
                          <textarea
                            readOnly
                            value={bytecode}
                            className="w-full h-[320px] bg-background-base border border-border-subtle rounded-lg p-3 text-xs font-mono text-text-high custom-scrollbar outline-none shadow-inner"
                          />
                        </div>
                        <div>
                          <div className="text-text-low text-[10px] uppercase font-bold tracking-widest font-display mb-1.5">Pseudocode</div>
                          <textarea
                            readOnly
                            value={decompiled}
                            className="w-full h-[320px] bg-background-base border border-border-subtle rounded-lg p-3 text-xs font-mono text-accent-green custom-scrollbar outline-none shadow-inner"
                          />
                        </div>
                      </div>
                    </div>
                  )}
                </div>
              )}
            </>
          )}
        </div>
      </div>
    </div>
  );
}

function InfoLine({ k, v }: { k: string; v: string }) {
  return (
    <div className="grid grid-cols-[140px_1fr] gap-3 border-b border-border-subtle py-2.5 last:border-b-0 hover:bg-surface-stripe/30 transition-colors rounded px-2 -mx-2">
      <span className="text-text-low font-display text-xs flex items-center">{k}</span>
      <span className="text-text-high font-mono break-all text-xs flex items-center">{v}</span>
    </div>
  );
}

