import { useEffect, useState } from 'react';
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

    const loadList = async () => {
        setListLoading(true);
        setListError(null);
        try {
            if (subTab === 'Class') {
                const res = await api.getClasses(0, 100, search);
                if (res.success && res.data) {
                    setItems(res.data.items.map((c) => ({ index: c.index, name: c.name, size: c.size, super: c.super })));
                    setTotal(res.data.total);
                }
            } else if (subTab === 'Struct') {
                const res = await api.getStructs(0, 100, search);
                if (res.success && res.data) {
                    setItems(res.data.items.map((s) => ({ index: s.index, name: s.name, size: s.size, super: s.super })));
                    setTotal(res.data.total);
                }
            } else if (subTab === 'Enum') {
                const res = await api.getEnums(0, 100, search);
                if (res.success && res.data) {
                    setItems(res.data.items.map((e) => ({ index: e.index, name: e.name })));
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
                            className="w-full h-8 bg-white/5 border border-white/10 rounded-lg text-xs text-white px-3 pl-9 focus:outline-none focus:border-white/20"
                        />
                    </div>
                </div>

                {/* List */}
                <div className="flex-1 overflow-auto px-2">
                    {listLoading && <div className="text-white/40 text-xs p-3">{t('Loading...')}</div>}
                    {listError && <div className="text-red-300 text-xs p-3">{listError}</div>}
                    {items.map((item) => (
                        <div
                            key={item.index}
                            onClick={() => { setSelected(item); void loadDetail(item); }}
                            className={`p-2.5 rounded-lg cursor-pointer mb-0.5 transition-all ${selected?.index === item.index
                                ? 'bg-white/10 border border-white/10'
                                : 'hover:bg-white/5 border border-transparent'
                                }`}
                        >
                            <div className="text-[13px] text-white/90 font-mono truncate">{item.name}</div>
                            <div className="flex gap-3 text-[11px] text-white/40 mt-0.5">
                                {item.size !== undefined && <span>0x{item.size.toString(16).toUpperCase()}</span>}
                                {item.super && <span className="text-blue-400/50">{item.super}</span>}
                            </div>
                        </div>
                    ))}
                    <div className="text-white/30 text-[11px] p-3">{t('Showing')} {items.length} / {total}</div>
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
                                    <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/70">
                                        Size: 0x{selected.size.toString(16).toUpperCase()} ({selected.size} B)
                                    </span>
                                )}
                                {superChain.length > 0 && (
                                    <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-mono text-blue-400">
                                        {superChain.join(' → ')}
                                    </span>
                                )}
                                {alignment > 0 && (
                                    <span className="px-2.5 py-1 rounded-[6px] bg-green-500/10 border border-green-500/20 text-[11px] font-mono text-green-400">
                                        Align: {alignment}
                                    </span>
                                )}
                            </>}
                        />

                        {detailLoading && <div className="text-white/40 text-sm">{t('Loading...')}</div>}
                        {detailError && <div className="text-red-300 text-sm">{detailError}</div>}
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
                                <div className="space-y-1">
                                    {functions.map((fn, i) => (
                                        <div key={i} className="p-2 rounded-lg border border-white/5 bg-black/20 hover:bg-white/5">
                                            <div className="flex items-center gap-3">
                                                <Hash className="w-3.5 h-3.5 text-green-400 flex-none" />
                                                <span className="text-[13px] text-white/90 font-mono">{fn.name}</span>
                                                <span className="text-[10px] text-white/30 ml-auto font-mono">{fn.flags}</span>
                                            </div>
                                            {fn.params.length > 0 && (
                                                <div className="mt-2 ml-7 text-[11px] text-white/50 font-mono">
                                                    ({fn.params.map((p) => `${p.name}: ${p.type}`).join(', ')})
                                                </div>
                                            )}
                                        </div>
                                    ))}
                                    {functions.length === 0 && <div className="text-white/40 text-sm">{t('No functions')}</div>}
                                </div>
                            </Panel>
                        )}

                        {/* Instances */}
                        {detailTab === 'Instances' && (
                            <Panel title={t('Instances')}>
                                <div className="space-y-1">
                                    {instances.map((inst) => (
                                        <div
                                            key={inst.index}
                                            className="flex items-center gap-3 p-2 rounded-lg border border-white/5 bg-black/20 hover:bg-white/5 cursor-pointer"
                                            onClick={() => onSwitchMode?.('instances', { className: selected.name, objectIndex: inst.index })}
                                        >
                                            <div className="w-2 h-2 rounded-full bg-green-400 flex-none" />
                                            <span className="text-[13px] text-white/90 font-mono flex-1 truncate">{inst.name}</span>
                                            <span className="text-[11px] text-white/30 font-mono">#{inst.index}</span>
                                            <span className="text-[11px] text-white/30 font-mono">{inst.address}</span>
                                            <ExternalLink className="w-3 h-3 text-white/20" />
                                        </div>
                                    ))}
                                    {instances.length === 0 && <div className="text-white/40 text-sm">{t('No live instances')}</div>}
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
        </div>
    );
}
