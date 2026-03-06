import { useEffect, useState, useRef } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import { t } from '../../i18n';
import { Search, Box, Database, Layers, Hash, ExternalLink } from 'lucide-react';
import api, {
    type ClassFunction,
    type ClassProperty,
    type EnumDetail,
} from '../../api';
import { Panel, HeaderCard, type BrowserPageProps } from './shared';

// ─── Types ─────────────────────────────────────────────────────

type TypeSubTab = 'Class' | 'Struct' | 'Enum';
type TypeDetailTab = 'Fields' | 'Functions' | 'Instances' | 'Values';

interface TypeItem {
    index: number;
    name: string;
    size?: number;
    super?: string;
    valueCount?: number;
}

// ─── Component ─────────────────────────────────────────────────

export default function TypeBrowser({ onNavigate: _onNavigate, onSwitchMode }: BrowserPageProps) {
    // Left panel state
    const [subTab, setSubTab] = useState<TypeSubTab>('Class');
    const [search, setSearch] = useState('');
    const [packageFilter] = useState('');
    const [items, setItems] = useState<TypeItem[]>([]);
    const [total, setTotal] = useState(0);
    const [listLoading, setListLoading] = useState(false);
    const [listError, setListError] = useState<string | null>(null);
    const [selected, setSelected] = useState<TypeItem | null>(null);

    // Right detail state
    const [detailLoading, setDetailLoading] = useState(false);
    const [detailError, setDetailError] = useState<string | null>(null);
    const [detailTab, setDetailTab] = useState<TypeDetailTab>('Fields');
    const [fields, setFields] = useState<ClassProperty[]>([]);
    const [functions, setFunctions] = useState<ClassFunction[]>([]);
    const [instances, setInstances] = useState<Array<{ index: number; name: string; address: string }>>([]);
    const [enumDetail, setEnumDetail] = useState<EnumDetail | null>(null);
    const [superChain, setSuperChain] = useState<string[]>([]);
    const [fullName, setFullName] = useState('');
    const [alignment, setAlignment] = useState(0);
    const [classSchemaError, setClassSchemaError] = useState<string | null>(null);

    // Available detail tabs per subTab type
    const availableTabs: TypeDetailTab[] =
        subTab === 'Enum' ? ['Values'] :
            subTab === 'Struct' ? ['Fields'] :
                ['Fields', 'Functions', 'Instances'];

    // ─── Data Loading ──────────────────────────────────────────

    const PAGE_SIZE = 500;

    const loadList = async (append = false) => {
        setListLoading(true);
        setListError(null);
        const offset = append ? items.length : 0;
        try {
            if (subTab === 'Class') {
                const res = await api.getClasses(offset, PAGE_SIZE, search);
                if (res.success && res.data) {
                    const mapped = res.data.items.map((c) => ({ index: c.index, name: c.name, size: c.size, super: c.super }));
                    setItems(append ? [...items, ...mapped] : mapped);
                    setTotal(res.data.total);
                }
            } else if (subTab === 'Struct') {
                const res = await api.getStructs(offset, PAGE_SIZE, search);
                if (res.success && res.data) {
                    const mapped = res.data.items.map((s) => ({ index: s.index, name: s.name, size: s.size, super: s.super }));
                    setItems(append ? [...items, ...mapped] : mapped);
                    setTotal(res.data.total);
                }
            } else if (subTab === 'Enum') {
                const res = await api.getEnums(offset, PAGE_SIZE, search);
                if (res.success && res.data) {
                    const mapped = res.data.items.map((e) => ({ index: e.index, name: e.name }));
                    setItems(append ? [...items, ...mapped] : mapped);
                    setTotal(res.data.total);
                }
            }
        } catch (error) {
            setListError(error instanceof Error ? error.message : String(error));
        } finally {
            setListLoading(false);
        }
    };

    const loadDetail = async (item: TypeItem) => {
        setDetailLoading(true);
        setDetailError(null);
        setFields([]);
        setFunctions([]);
        setInstances([]);
        setEnumDetail(null);
        setSuperChain([]);
        setFullName('');
        setAlignment(0);
        setClassSchemaError(null);
        // Reset to first available tab
        setDetailTab(subTab === 'Enum' ? 'Values' : 'Fields');

        try {
            if (subTab === 'Class') {
                const [fieldRes, funcRes, instanceRes] = await Promise.all([
                    api.getClassFields(item.name),
                    api.getClassFunctions(item.name),
                    api.getClassInstances(item.name, 0, 100),
                ]);
                const errors: string[] = [];
                if (fieldRes.success && fieldRes.data) setFields(fieldRes.data);
                else errors.push(`Fields: ${fieldRes.error || 'failed'}`);
                if (funcRes.success && funcRes.data) setFunctions(funcRes.data);
                else errors.push(`Functions: ${funcRes.error || 'failed'}`);
                if (instanceRes.success && instanceRes.data) setInstances(instanceRes.data.items);
                else errors.push(`Instances: ${instanceRes.error || 'failed'}`);
                if (errors.length > 0) setClassSchemaError(errors.join(' | '));

                // Build super chain
                const chain: string[] = [];
                let cur = item.super;
                while (cur) {
                    chain.push(cur);
                    const superRes = await api.getClassByName(cur);
                    if (superRes.success && superRes.data) {
                        setFullName((prev) => prev || superRes.data!.full_name);
                        setAlignment((prev) => prev || superRes.data!.alignment);
                        cur = superRes.data.super || '';
                    } else {
                        break;
                    }
                }
                setSuperChain(chain);
            } else if (subTab === 'Struct') {
                const res = await api.getStructByName(item.name);
                if (res.success && res.data) {
                    setFields(res.data.fields);
                    setFullName(res.data.full_name);
                    setAlignment(res.data.alignment);
                    if (res.data.super) setSuperChain([res.data.super]);
                }
            } else if (subTab === 'Enum') {
                const res = await api.getEnumByName(item.name);
                if (res.success && res.data) {
                    setEnumDetail(res.data);
                    setFullName(res.data.full_name);
                }
            }
        } catch (error) {
            setDetailError(error instanceof Error ? error.message : String(error));
        } finally {
            setDetailLoading(false);
        }
    };

    useEffect(() => { void loadList(); }, [subTab, search, packageFilter]);

    // ─── Virtualization ─────────────────────────────────────────

    const parentRef = useRef<HTMLDivElement>(null);

    const rowVirtualizer = useVirtualizer({
        count: items.length + (items.length < total && !listLoading ? 1 : 0),
        getScrollElement: () => parentRef.current,
        estimateSize: () => 64, // Approx height of each item
        overscan: 10,
    });

    // Handle Infinite Scroll
    const virtualItems = rowVirtualizer.getVirtualItems();
    useEffect(() => {
        const lastItem = virtualItems[virtualItems.length - 1];
        if (!lastItem) return;

        // Fetch more items when scrolled to the last 150 items
        if (lastItem.index >= items.length - 150 && !listLoading && items.length < total) {
            void loadList(true);
        }
    }, [virtualItems, items.length, listLoading, total]);

    // ─── Render ────────────────────────────────────────────────

    const SUB_TABS: { id: TypeSubTab; icon: typeof Layers; label: string }[] = [
        { id: 'Class', icon: Box, label: 'Class' },
        { id: 'Struct', icon: Database, label: 'Struct' },
        { id: 'Enum', icon: Layers, label: 'Enum' },
    ];

    return (
        <div className="flex h-full">
            {/* ── Left: Type List ── */}
            <div className="w-80 border-r border-white/5 flex flex-col flex-none bg-black/30">
                {/* Sub-tab bar */}
                <div className="h-10 border-b border-white/5 flex items-center px-3 gap-1">
                    {SUB_TABS.map((st) => {
                        const Icon = st.icon;
                        return (
                            <button
                                key={st.id}
                                onClick={() => { setSubTab(st.id); setSelected(null); }}
                                className={`h-7 px-3 flex items-center gap-1.5 rounded text-[12px] font-medium transition-all ${subTab === st.id ? 'bg-white/10 text-white' : 'text-white/40 hover:text-white/70'}`}
                            >
                                <Icon className="w-3.5 h-3.5" />
                                {st.label}
                            </button>
                        );
                    })}
                </div>

                {/* Search */}
                <div className="p-3">
                    <div className="relative">
                        <Search className="w-3.5 h-3.5 absolute left-3 top-1/2 -translate-y-1/2 text-white/30" />
                        <input
                            type="text"
                            value={search}
                            onChange={(e) => setSearch(e.target.value)}
                            placeholder={t(`Search ${subTab}...`)}
                            className="w-full h-8 bg-black/40 border border-white/10 rounded-lg text-xs text-white px-3 pl-9 focus:outline-none focus:border-white/20 transition-all focus:bg-black/60 shadow-inner"
                        />
                    </div>
                </div>

                {/* List */}
                <div ref={parentRef} className="flex-1 overflow-auto relative px-1 scrollbar-thin scrollbar-thumb-white/10 scrollbar-track-transparent">
                    {listError && <div className="text-red-300 text-xs p-3">{listError}</div>}

                    <div
                        style={{
                            height: `${rowVirtualizer.getTotalSize()}px`,
                            width: '100%',
                            position: 'relative',
                        }}
                    >
                        {virtualItems.map((virtualRow) => {
                            if (virtualRow.index >= items.length) {
                                return (
                                    <div
                                        key="loader"
                                        style={{
                                            position: 'absolute',
                                            top: 0,
                                            left: 0,
                                            width: '100%',
                                            height: `${virtualRow.size}px`,
                                            transform: `translateY(${virtualRow.start}px)`,
                                        }}
                                        className="flex items-center justify-center text-white/40 text-xs"
                                    >
                                        {t('Loading...')}
                                    </div>
                                );
                            }

                            const item = items[virtualRow.index];
                            return (
                                <div
                                    key={virtualRow.key}
                                    style={{
                                        position: 'absolute',
                                        top: 0,
                                        left: 0,
                                        width: '100%',
                                        height: `${virtualRow.size}px`,
                                        transform: `translateY(${virtualRow.start}px)`,
                                        paddingRight: '6px',
                                        paddingLeft: '6px',
                                        paddingTop: '4px'
                                    }}
                                >
                                    <div
                                        onClick={() => { setSelected(item); void loadDetail(item); }}
                                        className={`px-3 py-2.5 rounded-lg cursor-pointer outline-none transition-all duration-200 group flex flex-col gap-1 ${selected?.index === item.index
                                            ? 'bg-blue-500/[0.12] border-l-[3px] border-l-blue-400 border-y border-r border-transparent shadow-sm'
                                            : 'bg-transparent border-l-[3px] border-l-transparent border-y border-r border-transparent hover:bg-white/[0.06]'
                                            }`}
                                    >
                                        <div className="flex items-center justify-between gap-3">
                                            <span className={`text-[14px] leading-tight font-mono truncate transition-colors ${selected?.index === item.index ? 'text-blue-200 font-semibold' : 'text-slate-200 group-hover:text-white'}`}>
                                                {item.name}
                                            </span>
                                            {item.size !== undefined && <span className="text-xs text-slate-500 font-mono flex-none mt-0.5">0x{item.size.toString(16).toUpperCase()}</span>}
                                        </div>
                                        {item.super && <div className="text-xs text-slate-400 truncate mt-0.5 opacity-80" title={item.super}>↳ {item.super}</div>}
                                    </div>
                                </div>
                            );
                        })}
                    </div>
                </div>

                {/* Status Footer */}
                <div className="p-2 border-t border-white/5 shrink-0 bg-black/40 text-center text-white/30 text-[10px]">
                    {t('Showing')} {items.length} / {total}
                </div>
            </div>

            {/* ── Right: Detail ── */}
            <div className="flex-1 overflow-auto p-8">
                {!selected && <div className="text-white/40 text-sm">{t('Select a type on the left.')}</div>}
                {selected && (
                    <div className="max-w-5xl space-y-6">
                        {/* Header Card */}
                        <HeaderCard
                            icon={subTab === 'Class' ? Box : subTab === 'Struct' ? Database : Layers}
                            name={selected.name}
                            subtitle={fullName || selected.name}
                            gradient={subTab === 'Class' ? 'from-blue-500/20 to-indigo-500/10' : subTab === 'Struct' ? 'from-orange-500/20 to-red-500/10' : 'from-yellow-500/20 to-amber-500/10'}
                            iconColor={subTab === 'Class' ? 'text-blue-400' : subTab === 'Struct' ? 'text-orange-400' : 'text-yellow-400'}
                            glow={subTab === 'Class' ? 'bg-blue-500/20' : subTab === 'Struct' ? 'bg-orange-500/20' : 'bg-yellow-500/20'}
                            badges={<>
                                {selected.size !== undefined && (
                                    <span className="px-3 py-1.5 rounded-md bg-white/[0.03] border border-white/10 text-xs font-mono text-slate-300 shadow-sm backdrop-blur-md">
                                        Size: <span className="text-white/70">0x{selected.size.toString(16).toUpperCase()}</span> <span className="text-slate-500">({selected.size} B)</span>
                                    </span>
                                )}
                                {alignment > 0 && (
                                    <span className="px-3 py-1.5 rounded-md bg-emerald-500/10 border border-emerald-500/20 text-xs font-mono text-emerald-300 shadow-sm backdrop-blur-md">
                                        Align: <span className="text-emerald-100">{alignment}</span>
                                    </span>
                                )}
                            </>}
                        />

                        {superChain.length > 0 && (
                            <div className="flex items-center flex-wrap gap-2 px-6 py-4 bg-black/20 border border-white/5 shadow-inner rounded-xl backdrop-blur-md">
                                <span className="text-slate-500 text-sm font-medium">{t('Inheritance:')}</span>
                                {superChain.map((sc, i) => (
                                    <div key={i} className="flex items-center gap-2">
                                        {i > 0 && <span className="text-blue-500/40 text-[10px]">▶</span>}
                                        <span className={`font-mono text-[14px] px-2 py-0.5 rounded ${i === superChain.length - 1 ? 'bg-blue-500/10 text-blue-300 border border-blue-500/20' : 'text-slate-300 hover:text-white cursor-pointer transition-colors'}`}>
                                            {sc}
                                        </span>
                                    </div>
                                ))}
                            </div>
                        )}

                        {detailLoading && <div className="text-white/40 text-sm">{t('Loading...')}</div>}
                        {detailError && <div className="text-red-300 text-[14px]">{detailError}</div>}
                        {classSchemaError && <div className="text-yellow-300 text-xs">{classSchemaError}</div>}

                        {/* Detail Tab Bar */}
                        <div className="flex gap-1 border-b border-white/5 pb-2">
                            {availableTabs.map((tab) => (
                                <button
                                    key={tab}
                                    onClick={() => setDetailTab(tab)}
                                    className={`px-3 py-1.5 text-xs font-medium rounded-lg transition-all ${detailTab === tab ? 'bg-white/10 text-white' : 'text-white/40 hover:text-white/70'
                                        }`}
                                >
                                    {t(tab)}
                                </button>
                            ))}
                        </div>

                        {/* Fields */}
                        {detailTab === 'Fields' && (
                            <Panel title={t('Fields')}>
                                <div className="overflow-auto">
                                    <table className="w-full text-xs">
                                        <thead>
                                            <tr className="text-white/40 border-b border-white/5">
                                                <th className="text-left py-2 px-2 font-medium">Offset</th>
                                                <th className="text-left py-2 px-2 font-medium">Name</th>
                                                <th className="text-left py-2 px-2 font-medium">Type</th>
                                                <th className="text-left py-2 px-2 font-medium">Size</th>
                                                <th className="text-left py-2 px-2 font-medium">Flags</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {fields.map((f, i) => (
                                                <tr key={i} className="border-b border-white/[0.03] hover:bg-white/[0.03]">
                                                    <td className="py-1.5 px-2 font-mono text-green-400">+0x{f.offset.toString(16).toUpperCase()}</td>
                                                    <td className="py-1.5 px-2 font-mono text-white/90">{f.name}</td>
                                                    <td className="py-1.5 px-2 font-mono text-blue-400">{f.type}</td>
                                                    <td className="py-1.5 px-2 font-mono text-white/50">{f.size}</td>
                                                    <td className="py-1.5 px-2 text-white/30">{f.flags}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                    {fields.length === 0 && <div className="text-white/40 text-sm py-3">{t('No fields')}</div>}
                                </div>
                            </Panel>
                        )}

                        {/* Functions */}
                        {detailTab === 'Functions' && (
                            <Panel title={t('Functions')}>
                                <div className="space-y-2">
                                    {functions.map((fn, i) => (
                                        <div key={i} className="px-4 py-3 rounded-xl border border-white/[0.03] bg-black/10 hover:bg-white/[0.02] transition-colors">
                                            <div className="flex items-center gap-3">
                                                <Hash className="w-4 h-4 text-emerald-500 opacity-80 flex-none" />
                                                <span className="text-[14px] text-slate-200 font-mono font-medium">{fn.name}</span>
                                                <span className="text-xs text-slate-500 ml-auto font-mono max-w-[200px] truncate" title={fn.flags}>{fn.flags}</span>
                                            </div>
                                            {fn.params.length > 0 && (
                                                <div className="mt-2.5 ml-7 text-xs text-slate-400 font-mono leading-relaxed p-2 bg-black/20 rounded-md border border-white/[0.02]">
                                                    <span className="text-slate-500">参数：</span> ({fn.params.map((p) => `${p.name}: `).map((_, idx) => <span key={idx}><span className="text-slate-300">{fn.params[idx].name}</span><span className="text-emerald-400/70">: {fn.params[idx].type}</span>{idx < fn.params.length - 1 ? ', ' : ''}</span>)})
                                                </div>
                                            )}
                                        </div>
                                    ))}
                                    {functions.length === 0 && <div className="text-slate-500 text-sm py-4 text-center">{t('No functions')}</div>}
                                </div>
                            </Panel>
                        )}

                        {/* Instances */}
                        {detailTab === 'Instances' && (
                            <Panel title={t('Instances')}>
                                <div className="space-y-1.5">
                                    {instances.map((inst) => (
                                        <div
                                            key={inst.index}
                                            className="flex items-center gap-3 px-4 py-2.5 rounded-lg border border-transparent bg-black/10 hover:bg-white/[0.03] hover:border-white/[0.05] cursor-pointer transition-all"
                                            onClick={() => onSwitchMode?.('instances', { className: selected.name, objectIndex: inst.index })}
                                        >
                                            <div className="w-2 h-2 rounded-full bg-emerald-500 shadow-[0_0_8px_rgba(16,185,129,0.5)] flex-none" />
                                            <span className="text-[14px] text-slate-200 font-mono flex-1 truncate">{inst.name}</span>
                                            <span className="text-sm text-slate-500 font-mono">#{inst.index}</span>
                                            <span className="text-xs text-slate-400 font-mono">{inst.address}</span>
                                            <ExternalLink className="w-4 h-4 text-slate-500 ml-2 hover:text-blue-400" />
                                        </div>
                                    ))}
                                    {instances.length === 0 && <div className="text-slate-500 text-sm py-4 text-center">{t('No live instances')}</div>}
                                </div>
                            </Panel>
                        )}

                        {/* Enum Values */}
                        {detailTab === 'Values' && enumDetail && (
                            <Panel title={t('Enum Values')}>
                                <div className="text-xs text-white/50 mb-3">
                                    Underlying Type: <span className="text-blue-400 font-mono">{enumDetail.underlying_type}</span>
                                </div>
                                <table className="w-full text-xs">
                                    <thead>
                                        <tr className="text-white/40 border-b border-white/5">
                                            <th className="text-left py-2 px-2 font-medium">#</th>
                                            <th className="text-left py-2 px-2 font-medium">Name</th>
                                            <th className="text-left py-2 px-2 font-medium">Value</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {enumDetail.values.map((v, i) => (
                                            <tr key={i} className="border-b border-white/[0.03] hover:bg-white/[0.03]">
                                                <td className="py-1.5 px-2 text-white/30">{i}</td>
                                                <td className="py-1.5 px-2 font-mono text-white/90">{v.name}</td>
                                                <td className="py-1.5 px-2 font-mono text-yellow-400">{v.value}</td>
                                            </tr>
                                        ))}
                                    </tbody>
                                </table>
                            </Panel>
                        )}
                    </div>
                )}
            </div>
        </div >
    );
}
