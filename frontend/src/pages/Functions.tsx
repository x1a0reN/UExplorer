import { useCallback, useEffect, useMemo, useState } from 'react';
import { Search, Filter, TerminalSquare, Play, Info, List, History, RefreshCw, Cpu } from 'lucide-react';
import { t } from '../i18n';
import api, {
  isHookPushEventData,
  type FunctionCallArgument,
  type FunctionDetail,
  type HookItem,
  type HookLogEntry,
  type ObjectDetail,
  type ObjectItem,
  type StableObjectHandle,
} from '../api';

type FunctionTab = 'Info' | 'Parameters' | 'Call' | 'Hook' | 'Disassembly';
type FlagTab = 'All' | 'Native' | 'Blueprint';
type FunctionsViewMode = 'function' | 'hookManager';
type CallMode = 'instance' | 'static';

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

function extractFunctionParts(detail: ObjectDetail | null): { classPath: string; functionName: string; functionPath: string } {
  if (!detail?.full_name) return { classPath: '', functionName: detail?.name || '', functionPath: '' };
  const prefixedPath = detail.full_name;
  const fullPath = prefixedPath.includes(' ') ? prefixedPath.split(' ').slice(1).join(' ') : prefixedPath;
  const separator = fullPath.lastIndexOf('.');
  const functionName = detail.name;
  const classPath = separator > 0 ? fullPath.slice(0, separator) : '';
  return { classPath, functionName, functionPath: classPath ? fullPath : '' };
}

function hasFunctionFlag(flags: string, mask: bigint): boolean {
  if (!/^0x[0-9A-F]{16}$/.test(flags)) {
    throw new Error(`Invalid canonical function flags: ${flags}`);
  }
  return (BigInt(flags) & mask) !== 0n;
}

function parseObjectIndex(raw: string, label: string): number {
  if (!/^(0|[1-9][0-9]*)$/.test(raw)) {
    throw new Error(`${label} must be a canonical decimal object index`);
  }
  const index = Number(raw);
  if (!Number.isSafeInteger(index) || index > 2_147_483_647) {
    throw new Error(`${label} is outside the supported object-index range`);
  }
  return index;
}

export default function Functions({ viewMode = 'function', onViewModeChange }: FunctionsProps) {
  const [activeTab, setActiveTab] = useState<FunctionTab>('Call');
  const [flagTab, setFlagTab] = useState<FlagTab>('All');
  const [search, setSearch] = useState('');
  const [packageFilter, setPackageFilter] = useState('');
  const [items, setItems] = useState<FunctionItem[]>([]);
  const [selected, setSelected] = useState<FunctionItem | null>(null);
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState<string | null>(null);

  const [detail, setDetail] = useState<ObjectDetail | null>(null);
  const [functionMeta, setFunctionMeta] = useState<FunctionDetail | null>(null);
  const [detailLoading, setDetailLoading] = useState(false);
  const [detailError, setDetailError] = useState<string | null>(null);

  const [targetIndex, setTargetIndex] = useState('');
  const [paramInputs, setParamInputs] = useState<Record<string, string>>({});
  const [callMode, setCallMode] = useState<CallMode>('instance');
  const [staticClassName, setStaticClassName] = useState('');
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

  const [bytecode, setBytecode] = useState('');
  const [decompiled, setDecompiled] = useState('');
  const [blueprintPath, setBlueprintPath] = useState('');
  const [blueprintProfileId, setBlueprintProfileId] = useState('');
  const [decompileLoading, setDecompileLoading] = useState(false);

  const functionTabs = useMemo(
    () => [
      { id: 'Info' as FunctionTab, icon: Info },
      { id: 'Parameters' as FunctionTab, icon: List },
      { id: 'Call' as FunctionTab, icon: Play },
      { id: 'Disassembly' as FunctionTab, icon: TerminalSquare },
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

  useEffect(() => {
    if (activeTab !== 'Hook' && viewMode !== 'hookManager') {
      return;
    }

    let disposed = false;
    let unsubscribe: (() => Promise<boolean>) | null = null;
    void Promise.all([api.getStatus(), refreshHooks(), refreshHookLog()]).then(async ([status]) => {
      if (disposed || !status.success || !status.data) return;
      const subscription = await api.subscribeSessionEvents(status.data.pid, {
        onEvent: ({ event }) => {
          if (!event.kind.startsWith('hook.') || !isHookPushEventData(event.data)) return;
          const data = event.data;
          setHooks((current) => current.map((hook) => {
            if (hook.id !== data.id || data.source_sequence <= hook.last_event_sequence) return hook;
            return {
              ...hook,
              hit_count: event.kind === 'hook.process_event_enter'
                ? hook.hit_count + 1 : hook.hit_count,
              last_event_sequence: data.source_sequence,
              last_correlation: data.correlation,
              log_count: Math.min(hook.log_count + 1, 128),
              log_drop_count: data.retained_log_dropped_before,
            };
          }));
          if (data.id === activeHookId) {
            const entry: HookLogEntry = {
              sequence: data.source_sequence,
              configuration_generation: data.configuration_generation,
              kind: event.kind.slice('hook.'.length) as HookLogEntry['kind'],
              source: data.source,
              subject: data.id,
              correlation: data.correlation,
              coalesced_before: data.coalesced_before,
              drained_at_monotonic_us: data.drained_at_monotonic_us,
              function_path: data.function_path,
              payload: data.payload ?? { encoding: 'hex', size: 0, data: '' },
              push_payload_omitted: data.payload_omitted,
              push_payload_omission_code: data.payload_omission_code,
            };
            setHookLog((current) => [
              entry,
              ...current.filter((item) => item.sequence !== entry.sequence),
            ].slice(0, 1000));
          }
        },
      });
      if (disposed) {
        void subscription.unsubscribe();
        return;
      }
      unsubscribe = subscription.unsubscribe;
    }).catch(() => undefined);
    return () => {
      disposed = true;
      if (unsubscribe) void unsubscribe();
    };
  }, [activeHookId, activeTab, refreshHookLog, refreshHooks, viewMode]);

  const loadFunctions = useCallback(async () => {
    setListLoading(true);
    setListError(null);
    try {
      const res = await api.searchObjects(search.trim(), {
        kind: 'function',
        packagePath: packageFilter.trim() || undefined,
        cursor: null,
        limit: 128,
      });
      if (!res.success || !res.data) throw new Error(res.error || t('Failed to load functions'));

      const next = res.data.items.map((it: ObjectItem) => ({
        index: it.index,
        name: it.name,
        className: it.class,
        address: it.address,
      }));

      setItems(next);
    } catch (error) {
      setListError(error instanceof Error ? error.message : String(error));
      setItems([]);
    } finally {
      setListLoading(false);
    }
  }, [packageFilter, search]);

  const loadFunctionDetail = useCallback(async (index: number) => {
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
      if (!parts.classPath || !parts.functionPath) {
        throw new Error('Function metadata does not contain an exact owner/function path');
      }
      setBlueprintPath(parts.functionPath);

      const functionRes = await api.getFunctionByPath(parts.functionPath);
      if (!functionRes.success || !functionRes.data) {
        throw new Error(functionRes.error || t('Failed to load function metadata'));
      }
      const found = functionRes.data;
      setFunctionMeta(found);
      setStaticClassName(found.declaring_type.full_path);

      setCallMode(hasFunctionFlag(found.flags, 0x0000000000002000n) ? 'static' : 'instance');
      const inputMap: Record<string, string> = {};
      found.parameters
        .filter((parameter) => parameter.direction === 'input' || parameter.direction === 'inout')
        .forEach((parameter) => {
          inputMap[parameter.name] = '';
        });
      setParamInputs(inputMap);

      // An owning UClass is never a valid implicit instance-call target.
      setTargetIndex('');

    } catch (error) {
      setDetailError(error instanceof Error ? error.message : String(error));
    } finally {
      setDetailLoading(false);
    }
  }, []);

  useEffect(() => {
    const timer = window.setTimeout(() => void loadFunctions(), 150);
    return () => window.clearTimeout(timer);
  }, [loadFunctions]);

  useEffect(() => {
    if (!selected) return;
    const timer = window.setTimeout(() => void loadFunctionDetail(selected.index), 0);
    return () => window.clearTimeout(timer);
  }, [loadFunctionDetail, selected]);

  const executeCall = async () => {
    if (!detail || !functionMeta) return;

    setCalling(true);
    try {
      const argumentsByName: Record<string, FunctionCallArgument> = {};
      for (const parameter of functionMeta.parameters) {
        if (parameter.direction !== 'input' && parameter.direction !== 'inout') continue;
        const raw = (paramInputs[parameter.name] ?? '').trim();
        switch (parameter.kind) {
          case 'bool':
            if (raw !== 'true' && raw !== 'false') {
              throw new Error(`${parameter.name} must be true or false`);
            }
            argumentsByName[parameter.name] = { kind: 'bool', value: raw === 'true' };
            break;
          case 'int8':
          case 'int16':
          case 'int32':
          case 'int64':
            if (!/^(0|-?[1-9][0-9]*)$/.test(raw)) {
              throw new Error(`${parameter.name} must be a canonical signed decimal integer`);
            }
            argumentsByName[parameter.name] = { kind: parameter.kind, value: raw };
            break;
          case 'uint8':
          case 'uint16':
          case 'uint32':
          case 'uint64':
            if (!/^(0|[1-9][0-9]*)$/.test(raw)) {
              throw new Error(`${parameter.name} must be a canonical unsigned decimal integer`);
            }
            argumentsByName[parameter.name] = { kind: parameter.kind, value: raw };
            break;
          case 'float':
          case 'double':
            if (raw === '' || !Number.isFinite(Number(raw))) {
              throw new Error(`${parameter.name} must be a finite floating-point value`);
            }
            argumentsByName[parameter.name] = { kind: parameter.kind, value: raw };
            break;
          case 'object': {
            if (raw === '' || raw === 'null') {
              argumentsByName[parameter.name] = { kind: 'object', value: null };
              break;
            }
            const objectIndex = parseObjectIndex(raw, parameter.name);
            const objectRes = await api.getObjectByIndex(objectIndex);
            if (!objectRes.success || !objectRes.data?.handle) {
              throw new Error(objectRes.error || `Stable handle unavailable for ${parameter.name}`);
            }
            argumentsByName[parameter.name] = { kind: 'object', value: objectRes.data.handle };
            break;
          }
          case 'enum': {
            if (raw.startsWith('name:') && raw.length > 5) {
              argumentsByName[parameter.name] = {
                kind: 'enum',
                type_name: parameter.type_name,
                value: { name: raw.slice(5) },
              };
              break;
            }
            const enumRaw = raw.startsWith('raw:') ? raw.slice(4) : '';
            if (!/^(0|-?[1-9][0-9]*)$/.test(enumRaw)) {
              throw new Error(`${parameter.name} must use name:ExactEnumName or raw:CanonicalInteger`);
            }
            argumentsByName[parameter.name] = {
              kind: 'enum',
              type_name: parameter.type_name,
              value: { raw: enumRaw },
            };
            break;
          }
          case 'struct': {
            const components = raw.split(',').map((component) => component.trim());
            if (components.length !== 3 || components.some((component) => component === '' || !Number.isFinite(Number(component)))) {
              throw new Error(`${parameter.name} must contain three finite comma-separated components`);
            }
            if (parameter.type_name === '/Script/CoreUObject.Vector') {
              argumentsByName[parameter.name] = {
                kind: 'struct',
                type_name: parameter.type_name,
                value: { X: components[0], Y: components[1], Z: components[2] },
              };
              break;
            }
            if (parameter.type_name === '/Script/CoreUObject.Rotator') {
              argumentsByName[parameter.name] = {
                kind: 'struct',
                type_name: parameter.type_name,
                value: { Pitch: components[0], Yaw: components[1], Roll: components[2] },
              };
              break;
            }
            throw new Error(`${parameter.name} uses unsupported struct type ${parameter.type_name}`);
          }
          default:
            throw new Error(`${parameter.name} uses unsupported input kind ${parameter.kind}`);
        }
      }

      let target: StableObjectHandle;
      if (callMode === 'static') {
        if (!staticClassName.trim()) {
          throw new Error('Static call requires a class full path');
        }
        const cdoRes = await api.getClassCDO(staticClassName.trim());
        if (!cdoRes.success || !cdoRes.data?.handle || cdoRes.data.state !== 'present') {
          throw new Error(cdoRes.error || cdoRes.data?.reason || 'Class default object is unavailable');
        }
        target = cdoRes.data.handle;
      } else {
        const objectIndex = parseObjectIndex(targetIndex.trim(), 'Target object index');
        const targetRes = await api.getObjectByIndex(objectIndex);
        if (!targetRes.success || !targetRes.data?.handle) {
          throw new Error(targetRes.error || 'Stable target handle is unavailable');
        }
        target = targetRes.data.handle;
      }

      const res = await api.invokeFunction(target, functionMeta, argumentsByName);
      if (!res.success || !res.data) {
        setCallResult(res.error || 'Call failed');
        return;
      }
      setCallResult(JSON.stringify(res.data, null, 2));
    } catch (error) {
      setCallResult(error instanceof Error ? error.message : String(error));
    } finally {
      setCalling(false);
    }
  };

  const addHook = async () => {
    if (!functionMeta) return;
    setHookBusy(true);
    const res = await api.addHook(functionMeta, { mode: 'fixed_metadata' }, false);
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
    if (!selected || !functionMeta) return;
    setDecompileLoading(true);
    const path = blueprintPath.trim();
    if (!path || path !== functionMeta.full_path) {
      const message = 'Blueprint metadata must use the selected exact FunctionHandle path';
      setBytecode(message);
      setDecompiled(message);
      setDecompileLoading(false);
      return;
    }
    const profileId = blueprintProfileId.trim();
    const bytePromise = api.getBlueprintBytecode(functionMeta);
    const decompilePromise = profileId
      ? api.decompileBlueprint(functionMeta, profileId)
      : Promise.resolve(null);
    const [byteRes, decompileRes] = await Promise.all([bytePromise, decompilePromise]);

    if (byteRes.success && byteRes.data) {
      setBytecode(byteRes.data.bytecode);
    } else {
      setBytecode(byteRes.error || 'No bytecode');
    }
    if (!decompileRes) {
      setDecompiled('BYTECODE_PROFILE_REQUIRED: enter an exact witnessed profile ID');
    } else if (decompileRes.success && decompileRes.data) {
      setDecompiled(decompileRes.data.disassembly.pseudocode);
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
              placeholder={t('ClassName::FunctionName...')}
              className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary transition-all font-mono placeholder:text-text-low/50"
            />
          </div>
          <div className="relative group">
            <Filter className="w-4 h-4 text-text-low absolute left-3 top-2.5" />
            <input
              type="text"
              value={packageFilter}
              onChange={(e) => setPackageFilter(e.target.value)}
              placeholder={t('Package full path filter')}
              className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary transition-all font-mono placeholder:text-text-low/50"
            />
          </div>

          <div className="flex items-center justify-between">
            <div className="flex gap-1 bg-surface-stripe p-1 rounded-lg border border-border-subtle">
              {(['All', 'Native', 'Blueprint'] as FlagTab[]).map((tab) => (
                <button
                  key={tab}
                  onClick={() => setFlagTab(tab)}
                  disabled={tab !== 'All'}
                  title={tab === 'All' ? undefined : 'Backend-indexed implementation filtering is not available yet'}
                  className={`px-2.5 py-1 rounded-md text-[11px] font-semibold tracking-tight transition-colors font-display ${flagTab === tab ? 'bg-background-base text-text-high shadow-sm border border-border-subtle' : 'text-text-low hover:text-text-high border border-transparent'
                    } disabled:opacity-40 disabled:cursor-not-allowed`}
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
          {listLoading && <div className="text-text-low text-xs p-3 font-display">{t('Loading...')}</div>}
          {listError && <div className="text-accent-red text-xs p-3 font-display">{listError}</div>}
          {!listLoading && !listError && items.length === 0 && <div className="text-text-low text-xs p-3 font-display">{t('No functions loaded')}</div>}
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
              {t('Function Workbench')}
            </button>
            <span className="px-3 py-1.5 text-xs text-text-low font-display" title={t('Bounded Hook collector is not active')}>
              {t('Hook Monitoring Unavailable')}
            </span>
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
                    <div className="text-text-low text-xs mb-1">{t('Total Hooks')}</div>
                    <div className="text-text-high text-xl font-mono">{hooks.length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">{t('Enabled')}</div>
                    <div className="text-accent-green text-xl font-mono">{hooks.filter((h) => h.enabled).length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">{t('Filtered')}</div>
                    <div className="text-primary text-xl font-mono">{filteredHooks.length}</div>
                  </div>
                  <div>
                    <div className="text-text-low text-xs mb-1">{t('Selected Hook')}</div>
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
                    {t('Batch Enable')}
                  </button>
                  <button
                    onClick={() => void bulkSetHooksEnabled(false)}
                    disabled={hookBusy || filteredHooks.length === 0}
                    className="px-4 py-2 rounded-lg bg-accent-yellow/10 hover:bg-accent-yellow/20 text-accent-yellow text-xs font-display border border-accent-yellow/20 transition-colors disabled:opacity-50"
                  >
                    {t('Batch Disable')}
                  </button>
                  <button
                    onClick={() => {
                      void refreshHooks();
                      void refreshHookLog();
                    }}
                    className="px-4 py-2 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high text-xs font-display border border-border-subtle transition-colors"
                  >
                    {t('Refresh')}
                  </button>
                </div>

                <div className="border border-border-subtle rounded-xl overflow-hidden bg-background-base">
                  <div className="grid grid-cols-[90px_1fr_120px_140px] bg-surface-dark text-text-low text-[10px] uppercase font-bold tracking-widest px-4 py-2.5 font-display border-b border-border-subtle">
                    <div>{t('ID')}</div>
                    <div>{t('Function Path')}</div>
                    <div>{t('Status')}</div>
                    <div>{t('Hit Count')}</div>
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
                        <div className="font-display">{hook.enabled ? <span className="text-accent-green">{t('Enabled')}</span> : <span className="text-text-low">{t('Disabled')}</span>}</div>
                        <div className="font-mono">{hook.hit_count}</div>
                      </button>
                    ))}
                    {pagedHooks.length === 0 && <div className="p-6 text-text-low text-xs font-display text-center">{t('No matching hooks')}</div>}
                  </div>
                </div>

                <div className="flex items-center justify-between text-xs text-text-low font-display">
                  <div>{t('Page')} {hookPage} {t('of')} {hookTotalPages} ({hookPageSize} {t('per page')})</div>
                  <div className="flex gap-2">
                    <button
                      onClick={() => setHookPage((p) => Math.max(1, p - 1))}
                      disabled={hookPage <= 1}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      {t('Prev Page')}
                    </button>
                    <button
                      onClick={() => setHookPage((p) => Math.min(hookTotalPages, p + 1))}
                      disabled={hookPage >= hookTotalPages}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      {t('Next Page')}
                    </button>
                  </div>
                </div>
              </div>

              <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                <div className="flex items-center justify-between">
                  <h3 className="text-text-high font-semibold font-display">{t('Hook Log')}</h3>
                  <div className="text-xs text-text-low font-mono bg-background-base px-2 py-1 rounded border border-border-subtle">
                    Active Hook ID: {managerSelectedHookId ?? '-'}
                  </div>
                </div>

                <div className="max-h-[320px] overflow-auto border border-border-subtle rounded-xl p-3 bg-background-base space-y-1.5">
                  {pagedHookLogs.map((entry) => (
                    <div key={entry.sequence} className="text-xs font-mono text-text-high border-b border-border-subtle pb-1.5 pt-1">
                      <div className="text-text-low/60">#{entry.sequence} · {entry.drained_at_monotonic_us} us</div>
                      <div className="text-primary mt-0.5">{entry.function_path}</div>
                      {entry.push_payload_omitted && (
                        <div className="text-accent-yellow mt-0.5">
                          {entry.push_payload_omission_code ?? 'HOOK_PUSH_PAYLOAD_OMITTED'}
                        </div>
                      )}
                    </div>
                  ))}
                  {pagedHookLogs.length === 0 && <div className="text-text-low text-xs font-display text-center py-4">{t('No hook logs')}</div>}
                </div>
                <div className="flex items-center justify-between text-xs text-text-low font-display">
                  <div>{t('Page')} {hookLogPage} {t('of')} {hookLogTotalPages} ({hookLogPageSize} {t('per page')})</div>
                  <div className="flex gap-2">
                    <button
                      onClick={() => setHookLogPage((p) => Math.max(1, p - 1))}
                      disabled={hookLogPage <= 1}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      {t('Prev Page')}
                    </button>
                    <button
                      onClick={() => setHookLogPage((p) => Math.min(hookLogTotalPages, p + 1))}
                      disabled={hookLogPage >= hookLogTotalPages}
                      className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle transition-colors disabled:opacity-40"
                    >
                      {t('Next Page')}
                    </button>
                  </div>
                </div>
              </div>
            </div>
          ) : (
            <>
              {!selected && <div className="text-white/40 text-sm">{t('Select a function on the left panel.')}</div>}
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
                            Param Size: {functionMeta?.parameter_size ?? '-'}
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
                      <InfoLine k="Class" v={currentParts.classPath || selected.className} />
                      <InfoLine k="Native Address" v={functionMeta?.native_address || '-'} />
                      <InfoLine k="Param Size" v={String(functionMeta?.parameter_size ?? '-')} />
                      <InfoLine k="Flags" v={functionMeta?.flags || '-'} />
                      <InfoLine k="Implementation" v={functionMeta?.implementation || 'unavailable'} />
                      {functionMeta?.reason && <InfoLine k="Unavailable Reason" v={functionMeta.reason} />}
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
                          {functionMeta?.parameters.map((p) => (
                            <tr key={`${p.name}-${p.offset}`} className="border-b border-border-subtle last:border-0 hover:bg-surface-stripe/30 transition-colors">
                              <td className="py-2.5 text-text-high font-mono">{p.name}</td>
                              <td className="text-text-mid font-mono">{p.type_name}</td>
                              <td className="text-text-low font-display text-[11px]">{p.flags}</td>
                              <td className="text-text-low font-mono">{p.offset}</td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                      {!functionMeta && <div className="text-text-low text-sm font-display text-center py-4">{t('No metadata')}</div>}
                    </div>
                  )}

                  {activeTab === 'Call' && (
                    <div className="grid grid-cols-2 gap-6">
                      <div className="space-y-6">
                        <div className="bg-surface-dark border border-border-subtle rounded-xl overflow-hidden p-6 shadow-sm">
                          <h3 className="text-sm font-semibold text-text-high mb-4 tracking-tight font-display">{t('Invoke Parameters')}</h3>
                          <div className="space-y-4">
                            <div className="space-y-1.5">
                              <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('Call Mode')}</label>
                              <div className="flex gap-2 bg-background-base p-1 rounded-lg border border-border-subtle inline-flex">
                                {([
                                  { id: 'instance' as CallMode, label: 'Instance' },
                                  { id: 'static' as CallMode, label: 'Static' },
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
                                  placeholder={t('Object index')}
                                  className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                />
                              </div>
                            )}

                            {callMode === 'static' && (
                              <div className="space-y-1.5">
                                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">Target Class Full Path</label>
                                <input
                                  type="text"
                                  value={staticClassName}
                                  onChange={(e) => setStaticClassName(e.target.value)}
                                  placeholder={t('e.g. /Game/UI/BP_ItemGridWDT_C')}
                                  className="w-full bg-background-base border border-border-subtle text-text-high font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                                />
                              </div>
                            )}

                            {functionMeta?.parameters
                              .filter((p) => p.direction === 'input' || p.direction === 'inout')
                              .map((p) => (
                                <div key={p.name} className="space-y-1.5">
                                  <label className="text-[10px] font-bold text-text-low uppercase tracking-widest flex items-center justify-between font-display">
                                    <span>{p.name}</span>
                                    <span className="text-primary lowercase font-mono">{p.type_name}</span>
                                  </label>
                                  <input
                                    type="text"
                                    value={paramInputs[p.name] ?? ''}
                                    placeholder={
                                      p.type_name === '/Script/CoreUObject.Vector'
                                        ? 'X, Y, Z'
                                        : p.type_name === '/Script/CoreUObject.Rotator'
                                          ? 'Pitch, Yaw, Roll'
                                          : p.kind === 'enum'
                                            ? 'name:ExactEnumName or raw:0'
                                            : undefined
                                    }
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
                              Scalar and enum values use exact reflected kinds. Enum inputs require name: or raw:; object inputs accept null or an object index.
                            </div>

                            <button
                              onClick={() => void executeCall()}
                              disabled={calling || !functionMeta}
                              className="w-full py-2.5 rounded-lg bg-primary hover:bg-primary/90 text-white font-semibold text-[13px] font-display tracking-tight shadow-sm active:scale-[0.98] transition-all flex items-center justify-center gap-2 mt-4 disabled:opacity-50"
                            >
                              <Play className="w-4 h-4 fill-current" />
                              {calling ? 'Invoking...' : 'Invoke on game thread'}
                            </button>
                          </div>
                        </div>
                      </div>

                      <div className="space-y-6">
                        <div className="bg-surface-dark border border-border-subtle rounded-xl overflow-hidden p-6 flex flex-col h-full shadow-sm">
                          <div className="flex items-center justify-between mb-4">
                            <h3 className="text-sm font-semibold text-text-high tracking-tight font-display">{t('Execution Result')}</h3>
                            <History className="w-4 h-4 text-text-low" />
                          </div>
                          <pre className="flex-1 bg-background-base border border-border-subtle rounded-xl p-4 font-mono text-[12px] text-accent-green overflow-y-auto whitespace-pre-wrap custom-scrollbar">
                            {callResult || 'No invocation result'}
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
                        {hookLog.map((entry) => (
                          <div key={entry.sequence} className="text-xs font-mono text-text-high border-b border-border-subtle pb-1.5 pt-1">
                            <div className="text-text-low/60">#{entry.sequence} · {entry.drained_at_monotonic_us} us</div>
                            <div className="text-primary mt-0.5">{entry.function_path}</div>
                            {entry.push_payload_omitted && (
                              <div className="text-accent-yellow mt-0.5">
                                {entry.push_payload_omission_code ?? 'HOOK_PUSH_PAYLOAD_OMITTED'}
                              </div>
                            )}
                          </div>
                        ))}
                        {hookLog.length === 0 && <div className="text-text-low text-xs font-display text-center py-4">{t('No hook logs')}</div>}
                      </div>
                    </div>
                  )}

                  {activeTab === 'Disassembly' && (
                    <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 space-y-4 shadow-sm">
                      <div className="space-y-1.5">
                        <div className="text-text-low text-[11px] font-bold uppercase tracking-widest font-display">{t('Function Path (e.g. Class.Function)')}</div>
                        <input
                          type="text"
                          value={blueprintPath}
                          onChange={(e) => setBlueprintPath(e.target.value)}
                          placeholder={t('e.g. BP_ItemGridWDT_C.ExecuteUbergraph_BP_ItemGridWDT')}
                          className="w-full bg-background-base border border-border-subtle text-text-high text-[13px] font-mono rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                        />
                      </div>
                      <div className="space-y-1.5">
                        <div className="text-text-low text-[11px] font-bold uppercase tracking-widest font-display">{t('Witnessed Bytecode Profile ID')}</div>
                        <input
                          type="text"
                          value={blueprintProfileId}
                          onChange={(e) => setBlueprintProfileId(e.target.value)}
                          placeholder={t('Required only for strict disassembly')}
                          className="w-full bg-background-base border border-border-subtle text-text-high text-[13px] font-mono rounded-lg px-3 py-2 outline-none focus:border-primary transition-colors placeholder:text-text-low/50"
                        />
                      </div>
                      <button
                        onClick={() => void loadDecompile()}
                        disabled={decompileLoading}
                        className="px-4 py-2.5 rounded-lg bg-primary hover:bg-primary/90 text-white text-xs font-display font-medium shadow-sm active:scale-[0.98] transition-all disabled:opacity-50 flex items-center justify-center gap-2"
                      >
                        <Cpu className="w-4 h-4" />
                        {decompileLoading ? 'Loading...' : 'Load Bytecode + Disassemble'}
                      </button>

                      <div className="grid grid-cols-2 gap-4 pt-2">
                        <div>
                          <div className="text-text-low text-[10px] uppercase font-bold tracking-widest font-display mb-1.5">{t('Bytecode')}</div>
                          <textarea
                            readOnly
                            value={bytecode}
                            className="w-full h-[320px] bg-background-base border border-border-subtle rounded-lg p-3 text-xs font-mono text-text-high custom-scrollbar outline-none shadow-inner"
                          />
                        </div>
                        <div>
                          <div className="text-text-low text-[10px] uppercase font-bold tracking-widest font-display mb-1.5">{t('Bounded Disassembly')}</div>
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
