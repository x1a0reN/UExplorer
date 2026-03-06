import { useEffect, useState } from 'react';
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
                <div className="flex-1 overflow-auto px-2 pt-1">
                    {listLoading && <div className="text-white/40 text-xs p-3">{t('Loading...')}</div>}
                    {listError && <div className="text-red-300 text-xs p-3">{listError}</div>}
                    {items.map((item) => (
                        <div key={item.index}
                            onClick={() => { setSelected(item); void loadDetail(item); }}
                            className={`p-2.5 rounded-lg cursor-pointer mb-0.5 transition-all ${selected?.index === item.index ? 'bg-white/10 border border-white/10' : 'hover:bg-white/5 border border-transparent'
                                }`}>
                            <div className="flex items-center gap-2">
                                <div className="w-2 h-2 rounded-full bg-green-400 flex-none" />
                                <span className="text-[13px] text-white/90 font-mono truncate flex-1">{item.name}</span>
                            </div>
                            <div className="flex gap-3 text-[11px] text-white/40 mt-0.5 ml-4">
                                <span className="text-blue-400/60">{item.className}</span>
                                <span>#{item.index}</span>
                                <span>{item.address}</span>
                            </div>
                        </div>
                    ))}
                    <div className="text-white/30 text-[11px] p-3">{t('Showing')} {items.length} / {total}</div>
                    {items.length < total && !listLoading && (
                        <button onClick={() => void loadList(true)} className="w-full py-2 text-xs text-blue-300 hover:text-blue-200 hover:bg-white/5 rounded-lg transition-colors">
                            {t('Load More')} ({total - items.length} {t('remaining')})
                        </button>
                    )}
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
                                <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-mono text-blue-400 cursor-pointer hover:bg-blue-500/20"
                                    onClick={() => onSwitchMode?.('types', { className: selected.className })}>
                                    {t('Class')}: {selected.className}
                                </span>
                                <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/70">
                                    #{selected.index}
                                </span>
                                <span className="px-2.5 py-1 rounded-[6px] bg-purple-500/10 border border-purple-500/20 text-[11px] font-mono text-purple-400 cursor-pointer hover:bg-purple-500/20"
                                    onClick={() => onNavigate?.('memory')}>
                                    <ExternalLink className="w-3 h-3 inline mr-1" />{selected.address}
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
                                <div className="overflow-auto">
                                    <table className="w-full text-xs">
                                        <thead>
                                            <tr className="text-white/40 border-b border-white/5">
                                                <th className="text-left py-2 px-2 font-medium w-40">Name</th>
                                                <th className="text-left py-2 px-2 font-medium w-24">Type</th>
                                                <th className="text-left py-2 px-2 font-medium w-16">Offset</th>
                                                <th className="text-left py-2 px-2 font-medium">Value</th>
                                                <th className="py-2 px-2 w-20" />
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {properties.map((p) => (
                                                <tr key={p.name} className="border-b border-white/[0.03] hover:bg-white/[0.03]">
                                                    <td className="py-1.5 px-2 font-mono text-white/90">{p.name}</td>
                                                    <td className="py-1.5 px-2 font-mono text-blue-400">{p.type}</td>
                                                    <td className="py-1.5 px-2 font-mono text-green-400/60">+0x{p.offset.toString(16).toUpperCase()}</td>
                                                    <td className="py-1.5 px-2">
                                                        <input type="text"
                                                            value={propertyEditMap[p.name] ?? toEditable(p.value)}
                                                            onChange={(e) => setPropertyEditMap((prev) => ({ ...prev, [p.name]: e.target.value }))}
                                                            className="w-full bg-transparent border border-white/5 rounded px-2 py-0.5 text-xs text-white font-mono focus:outline-none focus:border-white/20" />
                                                    </td>
                                                    <td className="py-1.5 px-2">
                                                        <div className="flex gap-1">
                                                            <button onClick={() => void handlePropertySave(p.name)} title={t('Save')}
                                                                className="p-1 rounded hover:bg-white/10 text-white/40 hover:text-green-400">
                                                                <Save className="w-3 h-3" />
                                                            </button>
                                                            <button onClick={() => void handlePropertyRefresh(p.name)} title={t('Refresh')}
                                                                className={`p-1 rounded hover:bg-white/10 text-white/40 hover:text-blue-400 ${propertyRefreshing[p.name] ? 'animate-spin' : ''}`}>
                                                                <RefreshCw className="w-3 h-3" />
                                                            </button>
                                                        </div>
                                                    </td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                    {properties.length === 0 && <div className="text-white/40 text-sm py-3">{t('No properties')}</div>}
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
