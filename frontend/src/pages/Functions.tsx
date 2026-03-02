import { useEffect, useMemo, useState } from 'react';
import { Search, Filter, TerminalSquare, Play, Info, List, History, Power, RefreshCw, Cpu } from 'lucide-react';
import api, { type ClassFunction, type HookItem, type HookLogEntry, type ObjectDetail, type ObjectItem } from '../api';

type FunctionTab = 'Info' | 'Parameters' | 'Call' | 'Hook' | 'Decompile';
type FlagTab = 'All' | 'Native' | 'Blueprint';

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

export default function Functions() {
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
  const [callResult, setCallResult] = useState<string>('');
  const [calling, setCalling] = useState(false);

  const [hooks, setHooks] = useState<HookItem[]>([]);
  const [hookLog, setHookLog] = useState<HookLogEntry[]>([]);
  const [hookBusy, setHookBusy] = useState(false);

  const [bytecode, setBytecode] = useState('');
  const [decompiled, setDecompiled] = useState('');
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

  useEffect(() => {
    void loadFunctions();
  }, [search, classFilter, flagTab]);

  useEffect(() => {
    if (!selected) return;
    void loadFunctionDetail(selected.index);
  }, [selected]);

  useEffect(() => {
    const timer = setInterval(() => {
      void refreshHooks();
    }, 2000);
    return () => clearInterval(timer);
  }, []);

  useEffect(() => {
    if (activeTab !== 'Hook') return;
    const unsubscribe = api.subscribeEventStream(
      '/events/hooks',
      () => {
        void refreshHookLog();
        void refreshHooks();
      },
      () => {
        // ignore stream errors, polling is fallback
      }
    );
    return unsubscribe;
  }, [activeTab, currentHook?.id]);

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
      if (!res.success || !res.data) throw new Error(res.error || 'Failed to load functions');

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
        throw new Error(detailRes.error || 'Failed to load function detail');
      }
      setDetail(detailRes.data);

      const parts = extractFunctionParts(detailRes.data);
      if (!parts.className) return;

      const classFuncRes = await api.getClassFunctions(parts.className);
      if (!classFuncRes.success || !classFuncRes.data) {
        throw new Error(classFuncRes.error || 'Failed to load class functions');
      }
      const found = classFuncRes.data.find((f) => f.name === parts.functionName) || null;
      setFunctionMeta(found);

      if (found) {
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

  const refreshHooks = async () => {
    const res = await api.listHooks();
    if (res.success && res.data) {
      setHooks(res.data.hooks);
    }
  };

  const refreshHookLog = async () => {
    const id = currentHook?.id ?? 0;
    const res = await api.getHookLog(id);
    if (res.success && res.data) {
      setHookLog(res.data.entries.slice(-200).reverse());
    }
  };

  const executeCall = async () => {
    if (!detail) return;
    if (!targetIndex.trim()) {
      setCallResult('Target object index is required');
      return;
    }

    const objectIndex = Number(targetIndex);
    if (Number.isNaN(objectIndex)) {
      setCallResult('Target object index is invalid');
      return;
    }

    const params: Record<string, unknown> = {};
    Object.entries(paramInputs).forEach(([name, value]) => {
      params[name] = parseInputValue(value);
    });

    setCalling(true);
    const res = await api.callFunction(objectIndex, detail.name, params, true);
    setCalling(false);

    if (!res.success || !res.data) {
      setCallResult(res.error || 'Call failed');
      return;
    }
    setCallResult(JSON.stringify(res.data.result, null, 2));
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

  const loadDecompile = async () => {
    if (!selected) return;
    setDecompileLoading(true);
    const [byteRes, decompileRes] = await Promise.all([
      api.getBlueprintBytecode(selected.index),
      api.decompileBlueprint(selected.index),
    ]);
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
    <div className="flex-1 flex overflow-hidden">
      <div className="w-[340px] flex-none border-r border-white/5 flex flex-col bg-black/40 relative z-10 backdrop-blur-md">
        <div className="p-4 border-b border-white/5 space-y-3">
          <div className="relative group">
            <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
            <input
              type="text"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              placeholder="ClassName::FunctionName..."
              className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-mono placeholder:text-white/30"
            />
          </div>
          <div className="relative group">
            <Filter className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
            <input
              type="text"
              value={classFilter}
              onChange={(e) => setClassFilter(e.target.value)}
              placeholder="Package/class filter"
              className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-mono placeholder:text-white/30"
            />
          </div>

          <div className="flex items-center justify-between">
            <div className="flex gap-1 bg-white/5 p-1 rounded-[8px] border border-white/5">
              {(['All', 'Native', 'Blueprint'] as FlagTab[]).map((tab) => (
                <button
                  key={tab}
                  onClick={() => setFlagTab(tab)}
                  className={`px-2.5 py-1 rounded-[6px] text-[11px] font-semibold tracking-tight transition-colors ${
                    flagTab === tab ? 'bg-white/10 text-white' : 'text-white/50 hover:text-white'
                  }`}
                >
                  {tab}
                </button>
              ))}
            </div>
            <button
              onClick={() => void loadFunctions()}
              className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/50 hover:text-white transition-colors"
            >
              <RefreshCw className="w-3.5 h-3.5" />
            </button>
          </div>
        </div>

        <div className="flex-1 overflow-y-auto p-2 space-y-1 custom-scrollbar">
          {listLoading && <div className="text-white/40 text-xs p-3">Loading...</div>}
          {listError && <div className="text-red-300 text-xs p-3">{listError}</div>}
          {!listLoading && !listError && items.length === 0 && <div className="text-white/40 text-xs p-3">No functions</div>}
          {items.map((item) => {
            const active = selected?.index === item.index;
            return (
              <div
                key={item.index}
                onClick={() => setSelected(item)}
                className={`flex items-center gap-3 px-3 py-2 rounded-[10px] cursor-pointer transition-colors ${
                  active ? 'bg-primary text-white shadow-sm' : 'hover:bg-white/5 text-white/70'
                }`}
              >
                <div className={`w-7 h-7 rounded-[8px] flex items-center justify-center ${active ? 'bg-white/10 border-white/20' : 'bg-white/5'} border border-transparent flex-none`}>
                  <TerminalSquare className={`w-3.5 h-3.5 ${active ? 'text-white' : 'text-blue-400'}`} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className={`text-[12px] font-mono truncate ${active ? 'text-white' : 'text-white/90'}`}>{item.name}</div>
                  <div className={`text-[10px] uppercase font-bold tracking-widest truncate ${active ? 'text-white/70' : 'text-white/30'}`}>{item.className}</div>
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className="flex-1 flex flex-col min-w-0 bg-[#0A0A0C] relative">
        <div className="h-14 border-b border-white/5 bg-white/[0.02] backdrop-blur-3xl flex items-center px-6 gap-6 z-20">
          <nav className="flex items-center gap-4">
            {functionTabs.map((tab) => (
              <button
                key={tab.id}
                onClick={() => setActiveTab(tab.id)}
                className={`relative h-14 flex items-center gap-2 text-[13px] font-semibold tracking-tight transition-colors ${
                  activeTab === tab.id ? 'text-white' : 'text-white/40 hover:text-white/70'
                }`}
              >
                <tab.icon className="w-4 h-4" />
                {tab.id}
                {activeTab === tab.id && (
                  <div className="absolute bottom-0 left-0 right-0 h-[3px] bg-primary rounded-t-full shadow-[0_-2px_8px_rgba(10,132,255,0.5)]" />
                )}
              </button>
            ))}
          </nav>
        </div>

        <div className="flex-1 overflow-auto p-8 relative">
          {!selected && <div className="text-white/40 text-sm">Select a function on the left panel.</div>}
          {selected && (
            <div className="max-w-5xl space-y-6">
              <div className="apple-glass-panel rounded-[24px] p-6 relative overflow-hidden">
                <div className="flex gap-5 relative z-10 items-center">
                  <div className="w-16 h-16 rounded-[14px] bg-gradient-to-br from-green-500/20 to-teal-500/10 border border-white/10 flex items-center justify-center shadow-lg flex-none">
                    <TerminalSquare className="w-8 h-8 text-green-400 stroke-[1.5]" />
                  </div>
                  <div className="flex-1">
                    <h1 className="text-[20px] font-mono text-white tracking-tight leading-tight mb-2">{detail?.full_name || `${selected.className}::${selected.name}`}</h1>
                    <div className="flex flex-wrap gap-2">
                      <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-bold tracking-widest uppercase text-blue-400">
                        {selected.className}
                      </span>
                      <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">
                        Param Size: {functionMeta?.param_size ?? '-'}
                      </span>
                      <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">
                        Flags: {functionMeta?.flags || '-'}
                      </span>
                    </div>
                  </div>
                </div>
              </div>

              {detailLoading && <div className="text-white/40 text-sm">Loading function detail...</div>}
              {detailError && <div className="text-red-300 text-sm">{detailError}</div>}

              {activeTab === 'Info' && (
                <div className="apple-glass-panel rounded-[24px] p-6 space-y-2 text-sm">
                  <InfoLine k="Name" v={detail?.name || selected.name} />
                  <InfoLine k="Class" v={currentParts.className || selected.className} />
                  <InfoLine k="Address" v={functionMeta?.address || selected.address} />
                  <InfoLine k="Param Size" v={String(functionMeta?.param_size ?? '-')} />
                  <InfoLine k="Flags" v={functionMeta?.flags || '-'} />
                  <InfoLine k="Has Script" v={String(functionMeta?.has_script ?? false)} />
                </div>
              )}

              {activeTab === 'Parameters' && (
                <div className="apple-glass-panel rounded-[24px] p-6">
                  <table className="w-full text-left text-xs">
                    <thead>
                      <tr className="text-white/40 border-b border-white/10">
                        <th className="py-2">Name</th>
                        <th>Type</th>
                        <th>Flags</th>
                        <th>Offset</th>
                      </tr>
                    </thead>
                    <tbody>
                      {functionMeta?.params.map((p) => (
                        <tr key={`${p.name}-${p.offset}`} className="border-b border-white/5">
                          <td className="py-2 text-white/90 font-mono">{p.name}</td>
                          <td className="text-white/70">{p.type}</td>
                          <td className="text-white/50">{p.flags}</td>
                          <td className="text-white/40">{p.offset}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                  {!functionMeta && <div className="text-white/40 text-sm">No metadata</div>}
                </div>
              )}

              {activeTab === 'Call' && (
                <div className="grid grid-cols-2 gap-6">
                  <div className="space-y-6">
                    <div className="apple-glass-panel rounded-[24px] overflow-hidden p-6">
                      <h3 className="text-[14px] font-semibold text-white/90 mb-4 tracking-tight">Invoke Parameters</h3>
                      <div className="space-y-4">
                        <div className="space-y-1.5">
                          <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest">Target Object Index</label>
                          <input
                            type="text"
                            value={targetIndex}
                            onChange={(e) => setTargetIndex(e.target.value)}
                            placeholder="Object index"
                            className="w-full bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary/50 transition-colors"
                          />
                        </div>

                        {functionMeta?.params
                          .filter((p) => !p.flags.includes('OutParm') && !p.flags.includes('ReturnParm'))
                          .map((p) => (
                            <div key={p.name} className="space-y-1.5">
                              <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest flex items-center justify-between">
                                <span>{p.name}</span>
                                <span className="text-blue-400 lowercase font-mono">{p.type}</span>
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
                                className="w-full bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary/50 transition-colors"
                              />
                            </div>
                          ))}

                        <button
                          onClick={() => void executeCall()}
                          disabled={calling}
                          className="w-full py-2.5 rounded-lg bg-primary hover:bg-primary-dark text-white font-semibold text-[13px] tracking-tight shadow-md active:scale-[0.98] transition-all flex items-center justify-center gap-2 mt-4 disabled:opacity-50"
                        >
                          <Play className="w-4 h-4 fill-current" />
                          {calling ? 'Calling...' : 'Execute ProcessEvent'}
                        </button>
                      </div>
                    </div>
                  </div>

                  <div className="space-y-6">
                    <div className="apple-glass-panel rounded-[24px] overflow-hidden p-6 flex flex-col h-full">
                      <div className="flex items-center justify-between mb-4">
                        <h3 className="text-[14px] font-semibold text-white/90 tracking-tight">Execution Result</h3>
                        <History className="w-4 h-4 text-white/40" />
                      </div>
                      <pre className="flex-1 bg-black/50 border border-white/5 rounded-xl p-4 font-mono text-[12px] text-green-400 overflow-y-auto whitespace-pre-wrap">
                        {callResult || 'No execution yet'}
                      </pre>
                    </div>
                  </div>
                </div>
              )}

              {activeTab === 'Hook' && (
                <div className="apple-glass-panel rounded-[24px] p-6 space-y-4">
                  <div className="flex items-center gap-3">
                    <button
                      onClick={() => void addHook()}
                      disabled={hookBusy || !!currentHook || !currentParts.functionPath}
                      className="px-3 py-2 rounded-lg bg-primary hover:bg-primary-dark text-white text-sm disabled:opacity-50"
                    >
                      Add Hook
                    </button>
                    <button
                      onClick={() => void toggleHook()}
                      disabled={hookBusy || !currentHook}
                      className="px-3 py-2 rounded-lg bg-white/10 hover:bg-white/20 text-white text-sm disabled:opacity-50"
                    >
                      {currentHook?.enabled ? 'Disable' : 'Enable'}
                    </button>
                    <button
                      onClick={() => void removeHook()}
                      disabled={hookBusy || !currentHook}
                      className="px-3 py-2 rounded-lg bg-red-500/20 hover:bg-red-500/30 text-red-200 text-sm disabled:opacity-50"
                    >
                      Remove
                    </button>
                    <button
                      onClick={() => void refreshHookLog()}
                      className="ml-auto w-8 h-8 rounded-lg bg-white/10 hover:bg-white/20 flex items-center justify-center"
                    >
                      <RefreshCw className="w-4 h-4 text-white/70" />
                    </button>
                  </div>

                  <div className="text-xs text-white/60 font-mono">
                    Path: {currentParts.functionPath || '-'} | Current Hook ID: {currentHook?.id ?? '-'} | Hit Count: {currentHook?.hit_count ?? 0}
                  </div>

                  <div className="max-h-[300px] overflow-auto border border-white/5 rounded-xl p-3 bg-black/30 space-y-2">
                    {hookLog.map((entry, idx) => (
                      <div key={`${entry.timestamp}-${idx}`} className="text-xs font-mono text-white/80 border-b border-white/5 pb-2">
                        <div>{new Date(entry.timestamp).toLocaleTimeString()}</div>
                        <div className="text-white/50">{entry.function_name}</div>
                      </div>
                    ))}
                    {hookLog.length === 0 && <div className="text-white/40 text-xs">No hook logs</div>}
                  </div>
                </div>
              )}

              {activeTab === 'Decompile' && (
                <div className="apple-glass-panel rounded-[24px] p-6 space-y-4">
                  <button
                    onClick={() => void loadDecompile()}
                    disabled={decompileLoading}
                    className="px-4 py-2 rounded-lg bg-primary hover:bg-primary-dark text-white text-sm disabled:opacity-50 flex items-center gap-2"
                  >
                    <Cpu className="w-4 h-4" />
                    {decompileLoading ? 'Loading...' : 'Load Bytecode + Decompile'}
                  </button>

                  <div className="grid grid-cols-2 gap-4">
                    <div>
                      <div className="text-white/70 text-xs mb-2">Bytecode</div>
                      <textarea
                        readOnly
                        value={bytecode}
                        className="w-full h-[320px] bg-black/40 border border-white/10 rounded-lg p-3 text-xs font-mono text-white/80"
                      />
                    </div>
                    <div>
                      <div className="text-white/70 text-xs mb-2">Pseudocode</div>
                      <textarea
                        readOnly
                        value={decompiled}
                        className="w-full h-[320px] bg-black/40 border border-white/10 rounded-lg p-3 text-xs font-mono text-green-300"
                      />
                    </div>
                  </div>
                </div>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

function InfoLine({ k, v }: { k: string; v: string }) {
  return (
    <div className="grid grid-cols-[140px_1fr] gap-3 border-b border-white/5 py-2 last:border-b-0">
      <span className="text-white/50">{k}</span>
      <span className="text-white/90 font-mono break-all">{v}</span>
    </div>
  );
}
