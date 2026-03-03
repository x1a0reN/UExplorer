import { useEffect, useMemo, useState, type ReactNode } from 'react';
import {
  Search,
  Filter,
  Box,
  Database,
  Layers,
  Hash,
  Info,
  ListTree,
  PackageSearch,
  Globe,
  Save,
  Plus,
  RefreshCw,
} from 'lucide-react';
import api, {
  type ActorTransformData,
  type ClassFunction,
  type ClassProperty,
  type ObjectDetail,
  type ObjectProperty,
  type ObjectItem,
  type Vec3Data,
  type WorldActorDetail,
} from '../api';

type ObjectTypeTab = 'All' | 'Class' | 'Struct' | 'Enum' | 'Function' | 'Package' | 'Actor';
type DetailTab = 'Info' | 'Properties' | 'Fields' | 'Functions' | 'Instances' | 'World';
type VecInput = { x: string; y: string; z: string };
type TransformInputState = { location: VecInput; rotation: VecInput; scale: VecInput };

interface BrowserItem {
  index: number;
  name: string;
  className: string;
  address: string;
  type: ObjectTypeTab;
}

const OBJECT_TABS: ObjectTypeTab[] = ['All', 'Class', 'Struct', 'Enum', 'Function', 'Package', 'Actor'];

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

function toEditable(value: unknown): string {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'boolean') return String(value);
  if (value === null || value === undefined) return '';
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function toVecInput(vec?: { x: number; y: number; z: number }): VecInput {
  if (!vec) return { x: '', y: '', z: '' };
  return {
    x: String(vec.x),
    y: String(vec.y),
    z: String(vec.z),
  };
}

function toTransformInputState(transform?: ActorTransformData): TransformInputState {
  return {
    location: toVecInput(transform?.location),
    rotation: toVecInput(transform?.rotation),
    scale: toVecInput(transform?.scale),
  };
}

function parseVecInput(input: VecInput): Vec3Data | null {
  const x = Number(input.x);
  const y = Number(input.y);
  const z = Number(input.z);
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return null;
  return { x, y, z };
}

export default function Objects() {
  const [activeTab, setActiveTab] = useState<DetailTab>('Info');
  const [typeTab, setTypeTab] = useState<ObjectTypeTab>('All');
  const [search, setSearch] = useState('');
  const [packageFilter, setPackageFilter] = useState('');
  const [items, setItems] = useState<BrowserItem[]>([]);
  const [total, setTotal] = useState(0);
  const [matched, setMatched] = useState(0);
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState<string | null>(null);
  const [selected, setSelected] = useState<BrowserItem | null>(null);

  const [detail, setDetail] = useState<ObjectDetail | null>(null);
  const [properties, setProperties] = useState<ObjectProperty[]>([]);
  const [fields, setFields] = useState<ClassProperty[]>([]);
  const [functions, setFunctions] = useState<ClassFunction[]>([]);
  const [instances, setInstances] = useState<Array<{ index: number; name: string; address: string }>>([]);
  const [worldText, setWorldText] = useState('暂无世界数据');
  const [detailLoading, setDetailLoading] = useState(false);
  const [detailError, setDetailError] = useState<string | null>(null);
  const [propertyEditMap, setPropertyEditMap] = useState<Record<string, string>>({});
  const [cdoDiff, setCdoDiff] = useState<Set<string>>(new Set());
  const [actorDetail, setActorDetail] = useState<WorldActorDetail | null>(null);
  const [transformInput, setTransformInput] = useState<TransformInputState>(toTransformInputState());
  const [worldSaving, setWorldSaving] = useState(false);
  const [worldMessage, setWorldMessage] = useState<string | null>(null);

  const tabs = useMemo(
    () => [
      { id: 'Info' as DetailTab, icon: Info },
      { id: 'Properties' as DetailTab, icon: ListTree },
      { id: 'Fields' as DetailTab, icon: Database },
      { id: 'Functions' as DetailTab, icon: Hash },
      { id: 'Instances' as DetailTab, icon: PackageSearch },
      { id: 'World' as DetailTab, icon: Globe },
    ],
    []
  );

  useEffect(() => {
    void loadList();
  }, [typeTab, search, packageFilter]);

  useEffect(() => {
    if (!selected) return;
    void loadDetail(selected);
  }, [selected]);

  const loadList = async () => {
    setListLoading(true);
    setListError(null);
    setItems([]);
    setSelected(null);
    setDetail(null);
    setProperties([]);
    setFields([]);
    setFunctions([]);
    setInstances([]);
    setMatched(0);
    setTotal(0);
    setActorDetail(null);
    setTransformInput(toTransformInputState());
    setWorldMessage(null);

    try {
      const q = search.trim();
      if (typeTab === 'Class') {
        const res = await api.getClasses(0, 200, q);
        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load classes');
        const mapped: BrowserItem[] = res.data.items.map((it) => ({
          index: it.index,
          name: it.name,
          className: 'Class',
          address: it.address,
          type: 'Class',
        }));
        setItems(mapped);
        setMatched(res.data.total);
        setTotal(res.data.total);
      } else if (typeTab === 'Struct') {
        const res = await api.getStructs(0, 200, q);
        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load structs');
        const mapped: BrowserItem[] = res.data.items.map((it) => ({
          index: it.index,
          name: it.name,
          className: 'Struct',
          address: '-',
          type: 'Struct',
        }));
        setItems(mapped);
        setMatched(res.data.total);
        setTotal(res.data.total);
      } else if (typeTab === 'Enum') {
        const res = await api.getEnums(0, 200, q);
        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load enums');
        const mapped: BrowserItem[] = res.data.items.map((it) => ({
          index: it.index,
          name: it.name,
          className: 'Enum',
          address: '-',
          type: 'Enum',
        }));
        setItems(mapped);
        setMatched(res.data.total);
        setTotal(res.data.total);
      } else if (typeTab === 'Actor') {
        const res = await api.getWorldActors(0, 200, q);
        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load actors');
        const mapped: BrowserItem[] = res.data.items.map((it) => ({
          index: it.index,
          name: it.name,
          className: it.class,
          address: it.address,
          type: 'Actor',
        }));
        setItems(mapped);
        setMatched(res.data.matched);
        setTotal(res.data.matched);
      } else if (typeTab === 'Package') {
        const res = await api.getPackages(0, 200, q);
        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load packages');
        const mapped: BrowserItem[] = res.data.items.map((it) => ({
          index: it.index,
          name: it.name,
          className: 'Package',
          address: it.address,
          type: 'Package',
        }));
        setItems(mapped);
        setMatched(res.data.total);
        setTotal(res.data.total);
      } else {
        const classFilter = typeTab === 'Function' ? 'Function' : undefined;
        const shouldSearch = !!q || !!packageFilter || !!classFilter;
        const res = shouldSearch
          ? await api.searchObjects(q, {
              class: classFilter,
              package: packageFilter.trim() || undefined,
              offset: 0,
              limit: 200,
            })
          : await api.getObjects(0, 200, q);

        if (!res.success || !res.data) throw new Error(res.error || 'Failed to load objects');
        const list = res.data.items.map((it: ObjectItem) => ({
          index: it.index,
          name: it.name,
          className: it.class,
          address: it.address,
          type: typeTab,
        }));
        setItems(list);
        setMatched('matched' in res.data ? res.data.matched : res.data.total);
        setTotal('total' in res.data ? res.data.total : res.data.matched);
      }
    } catch (error) {
      setListError(error instanceof Error ? error.message : String(error));
    } finally {
      setListLoading(false);
    }
  };

  const loadDetail = async (item: BrowserItem) => {
    setDetailLoading(true);
    setDetailError(null);
    setActiveTab('Info');
    setFields([]);
    setFunctions([]);
    setInstances([]);
    setWorldText('暂无世界数据');
    setCdoDiff(new Set());
    setActorDetail(null);
    setTransformInput(toTransformInputState());
    setWorldMessage(null);

    try {
      let currentDetail: ObjectDetail | null = null;
      if (item.type !== 'Struct' && item.type !== 'Enum' && item.type !== 'Package') {
        const detailRes = await api.getObjectByIndex(item.index);
        if (detailRes.success && detailRes.data) {
          currentDetail = detailRes.data;
        }
      }
      setDetail(currentDetail);

      let loadedProperties: ObjectProperty[] = [];
      const canLoadProperties = item.type !== 'Struct' && item.type !== 'Enum' && item.type !== 'Package';
      if (canLoadProperties) {
        const propsRes = await api.getObjectProperties(item.index);
        if (propsRes.success && propsRes.data) {
          loadedProperties = propsRes.data;
          setProperties(loadedProperties);
          const editMap: Record<string, string> = {};
          loadedProperties.forEach((prop) => {
            editMap[prop.name] = toEditable(prop.value);
          });
          setPropertyEditMap(editMap);
        } else {
          setProperties([]);
        }
      } else {
        setProperties([]);
      }

      if (item.type === 'Class' || (currentDetail?.class && item.type !== 'Struct' && item.type !== 'Enum')) {
        const className = item.type === 'Class' ? item.name : currentDetail?.class || '';
        if (className) {
          const [fieldRes, funcRes, instanceRes, cdoRes] = await Promise.all([
            api.getClassFields(className),
            api.getClassFunctions(className),
            api.getClassInstances(className, 0, 100),
            api.getClassCDO(className),
          ]);
          if (fieldRes.success && fieldRes.data) setFields(fieldRes.data);
          if (funcRes.success && funcRes.data) setFunctions(funcRes.data);
          if (instanceRes.success && instanceRes.data) setInstances(instanceRes.data.items);

          if (cdoRes.success && cdoRes.data && loadedProperties.length > 0) {
            const cdoMap = new Map(cdoRes.data.properties.map((p) => [p.name, p.value]));
            const diff = new Set<string>();
            loadedProperties.forEach((p) => {
              if (cdoMap.has(p.name) && cdoMap.get(p.name) !== p.value) {
                diff.add(p.name);
              }
            });
            setCdoDiff(diff);
          }
        }
      }

      if (item.type === 'Struct') {
        const structRes = await api.getStructByName(item.name);
        if (structRes.success && structRes.data) {
          setFields(structRes.data.fields);
        }
      }

      if (item.type === 'Package') {
        const packageRes = await api.getPackageContents(item.name);
        if (packageRes.success && packageRes.data) {
          setWorldText(`Package ${item.name} contains ${packageRes.data.count} entries`);
        } else {
          setWorldText('未能加载包内容');
        }
      } else if (item.type === 'Actor') {
        const [worldRes, actorRes] = await Promise.all([
          api.getWorldShortcuts(),
          api.getWorldActorDetail(item.index),
        ]);

        if (worldRes.success && worldRes.data) {
          const shortcuts = [
            worldRes.data.game_mode?.name,
            worldRes.data.game_state?.name,
            worldRes.data.player_controller?.name,
            worldRes.data.pawn?.name,
          ]
            .filter(Boolean)
            .join(' / ');
          setWorldText(shortcuts ? `World shortcuts: ${shortcuts}` : 'No world shortcuts found');
        } else {
          setWorldText('未能加载 world shortcuts');
        }

        if (actorRes.success && actorRes.data) {
          setActorDetail(actorRes.data);
          setTransformInput(toTransformInputState(actorRes.data.transform));
        } else {
          setActorDetail(null);
          setWorldMessage(actorRes.error || '未能加载 Actor Transform');
        }
      }
    } catch (error) {
      setDetailError(error instanceof Error ? error.message : String(error));
    } finally {
      setDetailLoading(false);
    }
  };

  const saveProperty = async (propertyName: string) => {
    if (!selected) return;
    const raw = propertyEditMap[propertyName] ?? '';
    const parsed = parseInputValue(raw);
    const res = await api.setObjectProperty(selected.index, propertyName, parsed);
    if (res.success) {
      await loadDetail(selected);
    } else {
      setDetailError(res.error || 'Property write failed');
    }
  };

  const addWatch = async (propertyName: string) => {
    if (!selected) return;
    const res = await api.addWatch(selected.index, propertyName);
    if (!res.success) {
      setDetailError(res.error || 'Failed to add watch');
    }
  };

  const updateTransformField = (field: keyof TransformInputState, axis: keyof VecInput, value: string) => {
    setTransformInput((prev) => ({
      ...prev,
      [field]: {
        ...prev[field],
        [axis]: value,
      },
    }));
  };

  const applyActorTransform = async () => {
    if (!selected || selected.type !== 'Actor') return;
    const location = parseVecInput(transformInput.location);
    const rotation = parseVecInput(transformInput.rotation);
    const scale = parseVecInput(transformInput.scale);

    if (!location || !rotation || !scale) {
      setWorldMessage('输入非法：位置/旋转/缩放必须全部是有效数字');
      return;
    }

    setWorldSaving(true);
    setWorldMessage(null);
    const res = await api.updateWorldActorTransform(selected.index, {
      location,
      rotation,
      scale,
    });
    setWorldSaving(false);

    if (!res.success || !res.data) {
      setWorldMessage(`写入失败：${res.error || 'unknown error'}，已回滚到服务器状态`);
      await loadDetail(selected);
      return;
    }

    setActorDetail((prev) => {
      if (!prev) return prev;
      return {
        ...prev,
        transform: res.data?.transform || prev.transform,
      };
    });
    setTransformInput(toTransformInputState(res.data.transform));
    setWorldMessage(res.data.rolled_back ? '服务器提示已回滚' : 'Transform 更新成功');
  };

  return (
    <div className="flex-1 flex overflow-hidden">
      <div className="w-[360px] flex-none border-r border-white/5 flex flex-col bg-black/40 relative z-10 backdrop-blur-md">
        <div className="p-4 border-b border-white/5 space-y-3">
          <div className="relative group">
            <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
            <input
              type="text"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              placeholder="Search name/path..."
              className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-medium placeholder:text-white/30"
            />
          </div>
          <div className="relative group">
            <Filter className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
            <input
              type="text"
              value={packageFilter}
              onChange={(e) => setPackageFilter(e.target.value)}
              placeholder="Package filter"
              className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-medium placeholder:text-white/30"
            />
          </div>
          <div className="flex items-center gap-1 bg-white/5 p-1 rounded-[8px] border border-white/5 overflow-x-auto">
            {OBJECT_TABS.map((tab) => (
              <button
                key={tab}
                onClick={() => setTypeTab(tab)}
                className={`px-2.5 py-1 rounded-[6px] whitespace-nowrap text-[11px] font-semibold tracking-tight transition-colors ${
                  typeTab === tab ? 'bg-white/10 text-white shadow-sm' : 'text-white/50 hover:text-white'
                }`}
              >
                {tab}
              </button>
            ))}
            <button
              onClick={() => void loadList()}
              className="ml-auto w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/50 hover:text-white transition-colors"
              title="Refresh"
            >
              <RefreshCw className="w-3.5 h-3.5" />
            </button>
          </div>
        </div>

        <div className="flex-1 overflow-y-auto p-2 space-y-1 custom-scrollbar">
          {listLoading && <div className="text-white/40 text-[12px] p-3">Loading...</div>}
          {listError && <div className="text-red-300 text-[12px] p-3">{listError}</div>}
          {!listLoading && !listError && items.length === 0 && <div className="text-white/40 text-[12px] p-3">No data</div>}
          {items.map((item) => {
            const active = selected?.index === item.index && selected?.type === item.type;
            return (
              <div
                key={`${item.type}-${item.index}-${item.name}`}
                onClick={() => setSelected(item)}
                className={`flex items-center gap-3 px-3 py-2 rounded-[10px] cursor-pointer transition-colors ${
                  active ? 'bg-primary text-white shadow-sm' : 'hover:bg-white/5 text-white/70'
                }`}
              >
                <div className={`w-8 h-8 rounded-[8px] flex items-center justify-center ${active ? 'bg-white/10' : 'bg-white/5'} flex-none`}>
                  {item.type === 'Enum' ? (
                    <Layers className="w-4 h-4" />
                  ) : item.type === 'Struct' ? (
                    <Database className="w-4 h-4" />
                  ) : (
                    <Box className="w-4 h-4" />
                  )}
                </div>
                <div className="flex-1 min-w-0">
                  <div className={`text-[13px] font-semibold truncate ${active ? 'text-white' : 'text-white/90'}`}>{item.name}</div>
                  <div className={`text-[11px] font-mono truncate ${active ? 'text-white/70' : 'text-white/40'}`}>{item.className}</div>
                  <div className={`text-[10px] font-mono truncate ${active ? 'text-white/60' : 'text-white/30'}`}>{item.address}</div>
                </div>
              </div>
            );
          })}
        </div>

        <div className="p-3 border-t border-white/5 bg-black/40 flex justify-between items-center text-[10px] font-medium text-white/40">
          <span>Total {total.toLocaleString()}</span>
          <span>Matched {matched.toLocaleString()}</span>
        </div>
      </div>

      <div className="flex-1 flex flex-col min-w-0 bg-[#0A0A0C] relative">
        <div className="h-14 border-b border-white/5 bg-white/[0.02] backdrop-blur-3xl flex items-center px-6 gap-6 z-20">
          <nav className="flex items-center gap-6">
            {tabs.map((tab) => (
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
          {!selected && <div className="text-white/40 text-sm">Select an item on the left panel.</div>}
          {selected && (
            <div className="max-w-5xl space-y-6">
              <div className="apple-glass-panel rounded-[24px] p-6 relative overflow-hidden">
                <div className="absolute right-0 top-0 w-32 h-32 bg-primary/20 blur-[60px] rounded-full pointer-events-none -mt-10 -mr-10" />
                <div className="flex gap-6 relative z-10">
                  <div className="w-20 h-20 rounded-[16px] bg-gradient-to-br from-blue-500/20 to-indigo-500/10 border border-white/10 flex items-center justify-center shadow-lg flex-none">
                    <Box className="w-10 h-10 text-blue-400 stroke-[1.5]" />
                  </div>
                  <div className="flex-1">
                    <h1 className="text-[24px] font-semibold text-white tracking-tight leading-tight mb-1">{selected.name}</h1>
                    <p className="text-[13px] text-white/50 font-mono mb-4">{detail?.full_name || selected.className}</p>
                    <div className="flex flex-wrap gap-2">
                      <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">
                        Class: {detail?.class || selected.className}
                      </span>
                      <span className="px-2.5 py-1 rounded-[6px] bg-green-500/10 border border-green-500/20 text-[11px] font-mono text-green-400">
                        Index: {selected.index}
                      </span>
                      <span className="px-2.5 py-1 rounded-[6px] bg-purple-500/10 border border-purple-500/20 text-[11px] font-mono text-purple-400">
                        Address: {selected.address}
                      </span>
                    </div>
                  </div>
                </div>
              </div>

              {detailLoading && <div className="text-white/40 text-sm">Loading detail...</div>}
              {detailError && <div className="text-red-300 text-sm">{detailError}</div>}

              {activeTab === 'Info' && (
                <Panel title="Detailed Information">
                  <InfoRow label="Name" value={detail?.name || selected.name} />
                  <InfoRow label="Full Name" value={detail?.full_name || '-'} />
                  <InfoRow label="Class" value={detail?.class || selected.className} />
                  <InfoRow label="Address" value={detail?.address || selected.address} />
                  <InfoRow label="Outer Chain" value={detail?.outer_chain?.join(' -> ') || '-'} />
                </Panel>
              )}

              {activeTab === 'Properties' && (
                <Panel title="Properties">
                  <div className="space-y-2">
                    {properties.length === 0 && <div className="text-white/40 text-sm">No properties</div>}
                    {properties.map((prop) => (
                      <div
                        key={prop.name}
                        className={`grid grid-cols-[160px_140px_1fr_auto_auto] gap-3 items-center p-3 rounded-lg border ${
                          cdoDiff.has(prop.name) ? 'border-yellow-500/30 bg-yellow-500/5' : 'border-white/5 bg-black/20'
                        }`}
                      >
                        <div className="text-white/80 text-xs font-mono truncate" title={prop.name}>
                          {prop.name}
                        </div>
                        <div className="text-white/50 text-xs font-mono">{prop.type}</div>
                        <input
                          className="bg-black/40 border border-white/10 rounded-md px-2 py-1 text-white text-xs font-mono outline-none focus:border-primary/50"
                          value={propertyEditMap[prop.name] ?? ''}
                          onChange={(e) =>
                            setPropertyEditMap((prev) => ({
                              ...prev,
                              [prop.name]: e.target.value,
                            }))
                          }
                        />
                        <button
                          onClick={() => void saveProperty(prop.name)}
                          className="px-2 py-1 rounded-md bg-primary/80 hover:bg-primary text-white text-xs flex items-center gap-1"
                        >
                          <Save className="w-3 h-3" />
                          Save
                        </button>
                        <button
                          onClick={() => void addWatch(prop.name)}
                          className="px-2 py-1 rounded-md bg-white/10 hover:bg-white/20 text-white text-xs flex items-center gap-1"
                        >
                          <Plus className="w-3 h-3" />
                          Watch
                        </button>
                      </div>
                    ))}
                  </div>
                </Panel>
              )}

              {activeTab === 'Fields' && (
                <Panel title="Fields">
                  <table className="w-full text-left text-xs">
                    <thead>
                      <tr className="text-white/40 border-b border-white/10">
                        <th className="py-2">Offset</th>
                        <th>Name</th>
                        <th>Type</th>
                        <th>Size</th>
                        <th>Flags</th>
                      </tr>
                    </thead>
                    <tbody>
                      {fields.map((field) => (
                        <tr key={`${field.name}-${field.offset}`} className="border-b border-white/5">
                          <td className="py-2 text-white/60 font-mono">{field.offset}</td>
                          <td className="text-white/90 font-mono">{field.name}</td>
                          <td className="text-white/70">{field.type}</td>
                          <td className="text-white/60">{field.size}</td>
                          <td className="text-white/40">{field.flags}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                  {fields.length === 0 && <div className="text-white/40 text-sm">No fields</div>}
                </Panel>
              )}

              {activeTab === 'Functions' && (
                <Panel title="Functions">
                  <div className="space-y-2">
                    {functions.map((fn) => (
                      <div key={fn.full_name} className="p-3 rounded-lg border border-white/5 bg-black/20">
                        <div className="text-white/90 font-mono text-sm">{fn.full_name}</div>
                        <div className="text-white/50 text-xs mt-1">Flags: {fn.flags}</div>
                        <div className="text-white/40 text-xs mt-1">Params: {fn.params.length}</div>
                      </div>
                    ))}
                    {functions.length === 0 && <div className="text-white/40 text-sm">No functions</div>}
                  </div>
                </Panel>
              )}

              {activeTab === 'Instances' && (
                <Panel title="Instances">
                  <div className="space-y-2">
                    {instances.map((inst) => (
                      <div key={`${inst.index}-${inst.address}`} className="p-3 rounded-lg border border-white/5 bg-black/20">
                        <div className="text-white/90 font-mono text-sm">{inst.name}</div>
                        <div className="text-white/40 text-xs mt-1">
                          #{inst.index} / {inst.address}
                        </div>
                      </div>
                    ))}
                    {instances.length === 0 && <div className="text-white/40 text-sm">No instances</div>}
                  </div>
                </Panel>
              )}

              {activeTab === 'World' && (
                <Panel title="World">
                  <div className="space-y-4">
                    <div className="text-white/70 text-sm whitespace-pre-wrap">{worldText}</div>

                    {selected.type !== 'Actor' && (
                      <div className="text-white/40 text-sm">当前对象不是 Actor，无法编辑 Transform。</div>
                    )}

                    {selected.type === 'Actor' && (
                      <>
                        <div className="grid grid-cols-1 md:grid-cols-3 gap-4">
                          {([
                            { key: 'location' as const, label: 'Location' },
                            { key: 'rotation' as const, label: 'Rotation' },
                            { key: 'scale' as const, label: 'Scale' },
                          ]).map((section) => (
                            <div key={section.key} className="rounded-lg border border-white/10 bg-black/20 p-3 space-y-2">
                              <div className="text-xs uppercase tracking-wider text-white/50">{section.label}</div>
                              {(['x', 'y', 'z'] as const).map((axis) => (
                                <div key={axis} className="flex items-center gap-2">
                                  <div className="w-5 text-xs text-white/40 uppercase">{axis}</div>
                                  <input
                                    type="text"
                                    value={transformInput[section.key][axis]}
                                    onChange={(e) => updateTransformField(section.key, axis, e.target.value)}
                                    className="flex-1 bg-black/40 border border-white/10 text-white font-mono text-[12px] rounded-md px-2 py-1 outline-none focus:border-primary/50"
                                  />
                                </div>
                              ))}
                            </div>
                          ))}
                        </div>

                        <div className="flex items-center gap-3">
                          <button
                            onClick={() => void applyActorTransform()}
                            disabled={worldSaving}
                            className="px-3 py-2 rounded-lg bg-primary hover:bg-primary-dark text-white text-sm disabled:opacity-50"
                          >
                            {worldSaving ? '写入中...' : '应用 Transform'}
                          </button>
                          <button
                            onClick={() => {
                              if (actorDetail) {
                                setTransformInput(toTransformInputState(actorDetail.transform));
                                setWorldMessage('已恢复为最近一次服务端状态');
                              }
                            }}
                            className="px-3 py-2 rounded-lg bg-white/10 hover:bg-white/20 text-white text-sm"
                          >
                            回滚输入
                          </button>
                        </div>

                        {worldMessage && <div className="text-xs text-white/70">{worldMessage}</div>}

                        <div className="rounded-lg border border-white/10 bg-black/20 p-3">
                          <div className="text-xs uppercase tracking-wider text-white/50 mb-2">
                            Components ({actorDetail?.component_count ?? 0})
                          </div>
                          <div className="max-h-40 overflow-auto space-y-1">
                            {(actorDetail?.components || []).map((comp) => (
                              <div key={`${comp.index}-${comp.address}`} className="text-xs text-white/80 font-mono">
                                {comp.class} :: {comp.name}
                              </div>
                            ))}
                            {(actorDetail?.components || []).length === 0 && (
                              <div className="text-xs text-white/40">No components</div>
                            )}
                          </div>
                        </div>
                      </>
                    )}
                  </div>
                </Panel>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

function Panel({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="apple-glass-panel rounded-[24px] overflow-hidden">
      <div className="px-6 py-4 border-b border-white/5 bg-white/[0.02]">
        <h3 className="text-[14px] font-semibold text-white/90">{title}</h3>
      </div>
      <div className="p-4">{children}</div>
    </div>
  );
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="grid grid-cols-[180px_1fr] gap-3 py-2 border-b border-white/5 last:border-b-0">
      <div className="text-white/50 text-xs">{label}</div>
      <div className="text-white/90 text-xs font-mono break-all">{value}</div>
    </div>
  );
}
