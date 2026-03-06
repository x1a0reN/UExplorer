import { useEffect, useState, useRef } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import { t } from '../../i18n';
import { Search, Box, RefreshCw, Save, ExternalLink } from 'lucide-react';
import api, {
    type ObjectDetail,
    type ObjectProperty,
    type OuterChainItem,
} from '../../api';
import { Panel, InfoRow, HeaderCard, parseInputValue, toEditable, type BrowserPageProps, type ModeNavContext } from './shared';

// ─── Types ─────────────────────────────────────────────────────

interface InstanceItem {
    index: number;
    name: string;
    className: string;
    address: string;
}

interface InstanceBrowserProps extends BrowserPageProps {
    navContext?: ModeNavContext;
}

// ─── Component ─────────────────────────────────────────────────

export default function InstanceBrowser({ onNavigate, onSwitchMode, navContext }: InstanceBrowserProps) {
    // Left panel state
    const [search, setSearch] = useState('');
    const [classFilter, setClassFilter] = useState(navContext?.className || '');
    const [items, setItems] = useState<InstanceItem[]>([]);
    const [total, setTotal] = useState(0);
    const [listLoading, setListLoading] = useState(false);
    const [listError, setListError] = useState<string | null>(null);
    const [selected, setSelected] = useState<InstanceItem | null>(null);

    // Right panel state
    const [detail, setDetail] = useState<ObjectDetail | null>(null);
    const [outerChain, setOuterChain] = useState<OuterChainItem[]>([]);
    const [properties, setProperties] = useState<ObjectProperty[]>([]);
    const [detailLoading, setDetailLoading] = useState(false);
    const [detailError, setDetailError] = useState<string | null>(null);
    const [detailTab, setDetailTab] = useState<'Properties' | 'Info'>('Properties');
    const [propertyEditMap, setPropertyEditMap] = useState<Record<string, string>>({});
    const [propertyRefreshing, setPropertyRefreshing] = useState<Record<string, boolean>>({});

    // ─── Data Loading ──────────────────────────────────────────

    const PAGE_SIZE = 500;

    const loadList = async (append = false) => {
        setListLoading(true);
        setListError(null);
        const offset = append ? items.length : 0;
        try {
            const res = await api.searchObjects(search, { class: classFilter || undefined, offset, limit: PAGE_SIZE });
            if (res.success && res.data) {
                const mapped = res.data.items.map((o) => ({ index: o.index, name: o.name, className: o.class, address: o.address }));
                setItems(append ? [...items, ...mapped] : mapped);
                setTotal(res.data.matched);
            }
        } catch (error) {
            setListError(error instanceof Error ? error.message : String(error));
        } finally {
            setListLoading(false);
        }
    };

    const loadDetail = async (item: InstanceItem) => {
        setDetailLoading(true);
        setDetailError(null);
        setProperties([]);
        setDetail(null);
        setOuterChain([]);
        setPropertyEditMap({});
        setDetailTab('Properties');
        try {
            const [detailRes, propsRes, chainRes] = await Promise.all([
                api.getObjectByIndex(item.index),
                api.getObjectProperties(item.index),
                api.getObjectOuterChain(item.index),
            ]);
            if (detailRes.success && detailRes.data) setDetail(detailRes.data);
            if (propsRes.success && propsRes.data) {
                setProperties(propsRes.data);
                const editMap: Record<string, string> = {};
                propsRes.data.forEach((p) => { editMap[p.name] = toEditable(p.value); });
                setPropertyEditMap(editMap);
            }
            if (chainRes.success && chainRes.data) setOuterChain(chainRes.data.outer_chain);
        } catch (error) {
            setDetailError(error instanceof Error ? error.message : String(error));
        } finally {
            setDetailLoading(false);
        }
    };

    const handlePropertySave = async (propName: string) => {
        if (!selected) return;
        const raw = propertyEditMap[propName];
        const parsed = parseInputValue(raw);
        await api.setObjectProperty(selected.index, propName, parsed);
    };

    const handlePropertyRefresh = async (propName: string) => {
        if (!selected) return;
        setPropertyRefreshing((prev) => ({ ...prev, [propName]: true }));
        const res = await api.getObjectPropertyValue(selected.index, propName);
        if (res.success && res.data) {
            setPropertyEditMap((prev) => ({ ...prev, [propName]: toEditable(res.data!.value) }));
        }
        setPropertyRefreshing((prev) => ({ ...prev, [propName]: false }));
    };

    useEffect(() => { void loadList(); }, [search, classFilter]);

    // ─── Virtualization ─────────────────────────────────────────

    const parentRef = useRef<HTMLDivElement>(null);

    const rowVirtualizer = useVirtualizer({
        count: items.length + (items.length < total && !listLoading ? 1 : 0),
        getScrollElement: () => parentRef.current,
        estimateSize: () => 64, // Approx height of each instance item
        overscan: 10,
    });

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

    return (
        <div className="flex h-full">
            {/* ── Left: Instance List ── */}
            <div className="w-80 border-r border-white/5 flex flex-col flex-none bg-black/30">
                {/* Search + Class Filter */}
                <div className="p-3 space-y-2 border-b border-white/5">
                    <div className="relative">
                        <Search className="w-3.5 h-3.5 absolute left-3 top-1/2 -translate-y-1/2 text-white/30" />
                        <input type="text" value={search} onChange={(e) => setSearch(e.target.value)}
                            placeholder={t('Search objects...')}
                            className="w-full h-8 bg-white/5 border border-white/10 rounded-lg text-xs text-white px-3 pl-9 focus:outline-none focus:border-white/20" />
                    </div>
                    <input type="text" value={classFilter} onChange={(e) => setClassFilter(e.target.value)}
                        placeholder={t('Filter by class...')}
                        className="w-full h-7 bg-white/5 border border-white/10 rounded-lg text-xs text-white px-3 focus:outline-none focus:border-white/20" />
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
                                        <div className="flex items-center gap-3">
                                            <div className={`w-2 h-2 rounded-full flex-none transition-colors ${selected?.index === item.index ? 'bg-emerald-400 shadow-[0_0_8px_rgba(52,211,153,0.6)]' : 'bg-emerald-500/40 group-hover:bg-emerald-400/80'}`} />
                                            <span className={`text-[14px] font-mono truncate flex-1 transition-colors leading-tight ${selected?.index === item.index ? 'text-blue-200 font-semibold' : 'text-slate-200 group-hover:text-white'}`}>
                                                {item.name}
                                            </span>
                                        </div>
                                        <div className="flex gap-3 text-xs mt-0.5 ml-5 justify-between">
                                            <span className="text-emerald-400/70 truncate max-w-[140px] opacity-80" title={item.className}>{item.className}</span>
                                            <span className="text-slate-500 font-mono flex-none opacity-80">#{item.index}</span>
                                        </div>
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
                {!selected && <div className="text-white/40 text-sm">{t('Select an object on the left.')}</div>}
                {selected && (
                    <div className="max-w-5xl space-y-6">
                        <HeaderCard
                            icon={Box}
                            name={selected.name}
                            subtitle={detail?.full_name || selected.className}
                            gradient="from-green-500/20 to-emerald-500/10"
                            iconColor="text-green-400"
                            glow="bg-green-500/20"
                            badges={<>
                                <span className="px-3 py-1.5 rounded-md bg-blue-500/10 border border-blue-500/20 text-[11px] font-mono text-blue-300 cursor-pointer hover:bg-blue-500/20 transition-colors shadow-sm backdrop-blur-md flex items-center gap-1.5"
                                    onClick={() => onSwitchMode?.('types', { className: selected.className })}>
                                    <span className="text-blue-500/50">Class:</span> {selected.className}
                                </span>
                                <span className="px-3 py-1.5 rounded-md bg-white/[0.03] border border-white/10 text-[11px] font-mono text-slate-300 shadow-sm backdrop-blur-md">
                                    <span className="text-white/30">#</span>{selected.index}
                                </span>
                                <span className="px-3 py-1.5 rounded-md bg-purple-500/10 border border-purple-500/20 text-[11px] font-mono text-purple-300 cursor-pointer hover:bg-purple-500/20 transition-colors shadow-sm backdrop-blur-md flex items-center gap-1.5"
                                    onClick={() => onNavigate?.('memory')}>
                                    <ExternalLink className="w-3.5 h-3.5 opacity-70" /> {selected.address}
                                </span>
                            </>}
                        />

                        {detailLoading && <div className="text-white/40 text-sm">{t('Loading...')}</div>}
                        {detailError && <div className="text-red-300 text-sm">{detailError}</div>}

                        {/* Tab Bar */}
                        <div className="flex gap-1 border-b border-white/5 pb-2">
                            {(['Properties', 'Info'] as const).map((tab) => (
                                <button key={tab} onClick={() => setDetailTab(tab)}
                                    className={`px-3 py-1.5 text-xs font-medium rounded-lg transition-all ${detailTab === tab ? 'bg-white/10 text-white' : 'text-white/40 hover:text-white/70'}`}>
                                    {t(tab)}
                                </button>
                            ))}
                        </div>

                        {/* Properties Tab */}
                        {detailTab === 'Properties' && (
                            <Panel title={t('Properties')}>
                                <div className="overflow-x-auto -mx-4 px-4 pb-2">
                                    <table className="w-full text-left border-collapse">
                                        <thead>
                                            <tr className="border-b border-white/10 text-xs text-slate-400 font-medium tracking-wide">
                                                <th className="py-3 px-3 font-normal w-12 uppercase">{t('Offset')}</th>
                                                <th className="py-3 px-3 font-normal w-40 uppercase">{t('Name')}</th>
                                                <th className="py-3 px-3 font-normal w-24 uppercase">{t('Type')}</th>
                                                <th className="py-3 px-3 font-normal uppercase">{t('Value')}</th>
                                                <th className="py-3 px-3 w-16" />
                                            </tr>
                                        </thead>
                                        <tbody className="divide-y divide-white/[0.03]">
                                            {properties.map((p) => (
                                                <tr key={p.name} className="hover:bg-white/[0.02] transition-colors group">
                                                    <td className="py-2.5 px-3 text-sm text-slate-500 font-mono">+0x{p.offset.toString(16).toUpperCase().padStart(4, '0')}</td>
                                                    <td className="py-2.5 px-3 text-[14px] text-slate-200 font-mono font-medium">{p.name}</td>
                                                    <td className="py-2.5 px-3 text-sm text-emerald-400/80 font-mono truncate max-w-[150px]">{p.type}</td>
                                                    <td className="py-2.5 px-3">
                                                        <input type="text"
                                                            value={propertyEditMap[p.name] ?? toEditable(p.value)}
                                                            onChange={(e) => setPropertyEditMap((prev) => ({ ...prev, [p.name]: e.target.value }))}
                                                            className="w-full bg-black/20 border border-white/10 rounded-md px-3 py-1 text-[13px] text-white font-mono focus:outline-none focus:border-blue-500/50 focus:bg-white/5 transition-all shadow-inner" />
                                                    </td>
                                                    <td className="py-2.5 px-3">
                                                        <div className="flex gap-2 justify-end opacity-60 group-hover:opacity-100 transition-opacity">
                                                            <button onClick={() => void handlePropertySave(p.name)} title={t('Save')}
                                                                className="p-1.5 rounded-md hover:bg-emerald-500/20 text-slate-400 hover:text-emerald-400 transition-colors">
                                                                <Save className="w-3.5 h-3.5" />
                                                            </button>
                                                            <button onClick={() => void handlePropertyRefresh(p.name)} title={t('Refresh')}
                                                                className={`p-1.5 rounded-md hover:bg-blue-500/20 text-slate-400 hover:text-blue-400 transition-colors ${propertyRefreshing[p.name] ? 'animate-spin text-blue-400' : ''}`}>
                                                                <RefreshCw className="w-3.5 h-3.5" />
                                                            </button>
                                                        </div>
                                                    </td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                    {properties.length === 0 && <div className="text-slate-500 text-sm py-4 text-center">{t('No properties')}</div>}
                                </div>
                            </Panel>
                        )}

                        {/* Info Tab */}
                        {detailTab === 'Info' && (
                            <Panel title={t('Object Info')}>
                                <InfoRow label={t('Full Name')} value={detail?.full_name || ''} />
                                <InfoRow label={t('Class')} value={selected.className} isLink onClick={() => onSwitchMode?.('types', { className: selected.className })} />
                                <InfoRow label={t('Index')} value={String(selected.index)} />
                                <InfoRow label={t('Address')} value={selected.address} isLink onClick={() => onNavigate?.('memory')} />
                                <InfoRow label={t('Flags')} value={detail?.flags || ''} />
                                {detail?.flags_raw != null && (
                                    <InfoRow label={t('Flags (Raw)')} value={`0x${detail.flags_raw.toString(16).toUpperCase()}`} />
                                )}
                                {outerChain.length > 0 && (
                                    <div className="mt-4">
                                        <div className="text-white/50 text-xs mb-2">{t('Outer Chain')}</div>
                                        <div className="flex flex-wrap gap-1.5">
                                            {outerChain.map((oc, i) => (
                                                <span key={i} className="px-2 py-0.5 rounded bg-white/5 border border-white/10 text-[11px] font-mono text-white/70">
                                                    {oc.name}
                                                </span>
                                            ))}
                                        </div>
                                    </div>
                                )}
                            </Panel>
                        )}
                    </div>
                )}
            </div>
        </div>
    );
}
