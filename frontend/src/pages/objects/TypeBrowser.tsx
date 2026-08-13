import { useCallback, useEffect, useRef, useState } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import { t } from '../../i18n';
import { Search, Box, Database, Layers, Hash, ExternalLink } from 'lucide-react';
import api from '../../services';
import {
    type ClassFunction,
    type ClassProperty,
    type EnumDetail,
    type EnumValue,
    type SnapshotQueryCursor,
    type TypeQueryCursor,
} from '../../contracts';
import { Panel, HeaderCard, type BrowserPageProps, type ModeNavContext } from './shared';
import { isAbortError, useQueryRunner } from '../../features/query/useQueryRunner';
import { useDebouncedValue } from '../../features/query/useDebouncedValue';
import { DomainError } from '../../features/shared/DomainError';
import { LoadMoreButton } from '../../features/shared/Pagination';
import { formatAddress } from '../../features/address/address';

// ─── Types ─────────────────────────────────────────────────────

type TypeSubTab = 'Class' | 'Struct' | 'Enum';
type TypeDetailTab = 'Fields' | 'Functions' | 'Instances' | 'Values';

interface TypeItem {
    index: number;
    name: string;
    fullName: string;
    size?: number;
    super?: string;
    valueCount?: number;
}

// ─── Component ─────────────────────────────────────────────────

interface TypeBrowserProps extends BrowserPageProps {
    navContext?: ModeNavContext;
}

export default function TypeBrowser({ onSwitchMode, navContext }: TypeBrowserProps) {
    const { run } = useQueryRunner();
    const detailRequestEpoch = useRef(0);
    // Left panel state
    const [subTab, setSubTab] = useState<TypeSubTab>('Class');
    const [search, setSearch] = useState(navContext?.className || '');
    const [items, setItems] = useState<TypeItem[]>([]);
    const [total, setTotal] = useState(0);
    const [nextCursor, setNextCursor] = useState<SnapshotQueryCursor | null>(null);
    const [hasMore, setHasMore] = useState(false);
    const [listLoading, setListLoading] = useState(false);
    const [listError, setListError] = useState<string | null>(null);
    const [selected, setSelected] = useState<TypeItem | null>(null);
    const debouncedSearch = useDebouncedValue(search);

    // Right detail state
    const [detailLoading, setDetailLoading] = useState(false);
    const [detailError, setDetailError] = useState<string | null>(null);
    const [detailTab, setDetailTab] = useState<TypeDetailTab>('Fields');
    const [fields, setFields] = useState<ClassProperty[]>([]);
    const [functions, setFunctions] = useState<ClassFunction[]>([]);
    const [instances, setInstances] = useState<Array<{ index: number; name: string; address: string }>>([]);
    const [enumDetail, setEnumDetail] = useState<EnumDetail | null>(null);
    const [enumValues, setEnumValues] = useState<EnumValue[]>([]);
    const [superChain, setSuperChain] = useState<string[]>([]);
    const [fullName, setFullName] = useState('');
    const [typeSize, setTypeSize] = useState<number | null>(null);
    const [alignment, setAlignment] = useState(0);
    const [classSchemaError, setClassSchemaError] = useState<string | null>(null);
    const [fieldCursor, setFieldCursor] = useState<TypeQueryCursor | null>(null);
    const [fieldHasMore, setFieldHasMore] = useState(false);
    const [functionCursor, setFunctionCursor] = useState<TypeQueryCursor | null>(null);
    const [functionHasMore, setFunctionHasMore] = useState(false);
    const [enumCursor, setEnumCursor] = useState<TypeQueryCursor | null>(null);
    const [enumHasMore, setEnumHasMore] = useState(false);

    // Available detail tabs per subTab type
    const availableTabs: TypeDetailTab[] =
        subTab === 'Enum' ? ['Values'] :
            subTab === 'Struct' ? ['Fields'] :
                ['Fields', 'Functions', 'Instances'];

    // ─── Data Loading ──────────────────────────────────────────

    const PAGE_SIZE = 128;

    const loadList = useCallback(async (
        cursor: SnapshotQueryCursor | null = null,
        append = false,
    ) => {
        if (!append) {
            setItems([]);
            setTotal(0);
            setNextCursor(null);
            setHasMore(false);
        }
        setListLoading(true);
        setListError(null);
        try {
            if (subTab === 'Class') {
                const res = await run('type-browser:list', async () => api.getClasses(cursor, PAGE_SIZE, debouncedSearch));
                if (!res.success || !res.data) throw new Error(res.error || 'Class query failed');
                const mapped = res.data.items.map((c) => ({ index: c.index, name: c.name, fullName: c.full_name, size: c.size, super: c.super }));
                setItems((current) => append ? [...current, ...mapped] : mapped);
                setTotal(res.data.total);
                setNextCursor(res.data.next_cursor);
                setHasMore(res.data.has_more);
            } else if (subTab === 'Struct') {
                const res = await run('type-browser:list', async () => api.getStructs(cursor, PAGE_SIZE, debouncedSearch));
                if (!res.success || !res.data) throw new Error(res.error || 'Struct query failed');
                const mapped = res.data.items.map((s) => ({ index: s.index, name: s.name, fullName: s.full_name, size: s.size, super: s.super }));
                setItems((current) => append ? [...current, ...mapped] : mapped);
                setTotal(res.data.total);
                setNextCursor(res.data.next_cursor);
                setHasMore(res.data.has_more);
            } else if (subTab === 'Enum') {
                const res = await run('type-browser:list', async () => api.getEnums(cursor, PAGE_SIZE, debouncedSearch));
                if (!res.success || !res.data) throw new Error(res.error || 'Enum query failed');
                const mapped = res.data.items.map((e) => ({ index: e.index, name: e.name, fullName: e.full_name }));
                setItems((current) => append ? [...current, ...mapped] : mapped);
                setTotal(res.data.total);
                setNextCursor(res.data.next_cursor);
                setHasMore(res.data.has_more);
            }
        } catch (error) {
            if (isAbortError(error)) return;
            setListError(error instanceof Error ? error.message : String(error));
            setNextCursor(null);
            setHasMore(false);
        } finally {
            setListLoading(false);
        }
    }, [debouncedSearch, run, subTab]);

    const loadDetail = async (item: TypeItem) => {
        const requestEpoch = ++detailRequestEpoch.current;
        setDetailLoading(true);
        setDetailError(null);
        setFields([]);
        setFunctions([]);
        setInstances([]);
        setEnumDetail(null);
        setEnumValues([]);
        setSuperChain([]);
        setFullName(item.fullName);
        setTypeSize(null);
        setAlignment(0);
        setClassSchemaError(null);
        setFieldCursor(null);
        setFieldHasMore(false);
        setFunctionCursor(null);
        setFunctionHasMore(false);
        setEnumCursor(null);
        setEnumHasMore(false);
        // Reset to first available tab
        setDetailTab(subTab === 'Enum' ? 'Values' : 'Fields');

        try {
            if (subTab === 'Class') {
                const [detailRes, fieldRes, funcRes, hierarchyRes, instanceRes] = await Promise.all([
                    api.getClassByPath(item.fullName),
                    api.getClassFields(item.fullName),
                    api.getClassFunctions(item.fullName),
                    api.getClassHierarchy(item.fullName),
                    api.getClassInstances(item.fullName, null, 100),
                ]);
                if (requestEpoch !== detailRequestEpoch.current) return;
                const errors: string[] = [];
                if (detailRes.success && detailRes.data) {
                    setFullName(detailRes.data.full_path);
                    setTypeSize(detailRes.data.properties_size);
                    setAlignment(detailRes.data.min_alignment);
                } else errors.push(`Detail: ${detailRes.error || 'failed'}`);
                if (fieldRes.success && fieldRes.data) {
                    setFields(fieldRes.data.items);
                    setFieldCursor(fieldRes.data.next_cursor);
                    setFieldHasMore(fieldRes.data.has_more);
                }
                else errors.push(`Fields: ${fieldRes.error || 'failed'}`);
                if (funcRes.success && funcRes.data) {
                    setFunctions(funcRes.data.items);
                    setFunctionCursor(funcRes.data.next_cursor);
                    setFunctionHasMore(funcRes.data.has_more);
                }
                else errors.push(`Functions: ${funcRes.error || 'failed'}`);
                if (hierarchyRes.success && hierarchyRes.data) {
                    setSuperChain(hierarchyRes.data.parents.map((parent) => parent.full_path));
                } else errors.push(`Hierarchy: ${hierarchyRes.error || 'failed'}`);
                if (instanceRes.success && instanceRes.data) setInstances(instanceRes.data.items);
                else errors.push(`Instances: ${instanceRes.error || 'failed'}`);
                if (errors.length > 0) setClassSchemaError(errors.join(' | '));
            } else if (subTab === 'Struct') {
                const [detailRes, fieldRes] = await Promise.all([
                    api.getStructByPath(item.fullName),
                    api.getStructFields(item.fullName),
                ]);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (!detailRes.success || !detailRes.data) throw new Error(detailRes.error || 'Struct detail failed');
                if (!fieldRes.success || !fieldRes.data) throw new Error(fieldRes.error || 'Struct fields failed');
                setFields(fieldRes.data.items);
                setFieldCursor(fieldRes.data.next_cursor);
                setFieldHasMore(fieldRes.data.has_more);
                setFullName(detailRes.data.full_path);
                setTypeSize(detailRes.data.properties_size);
                setAlignment(detailRes.data.min_alignment);
                if (detailRes.data.super) setSuperChain([detailRes.data.super.full_path]);
            } else if (subTab === 'Enum') {
                const [detailRes, valuesRes] = await Promise.all([
                    api.getEnumByPath(item.fullName),
                    api.getEnumValues(item.fullName),
                ]);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (!detailRes.success || !detailRes.data) throw new Error(detailRes.error || 'Enum detail failed');
                if (!valuesRes.success || !valuesRes.data) throw new Error(valuesRes.error || 'Enum values failed');
                setEnumDetail(detailRes.data);
                setEnumValues(valuesRes.data.items);
                setEnumCursor(valuesRes.data.next_cursor);
                setEnumHasMore(valuesRes.data.has_more);
                setFullName(detailRes.data.full_path);
            }
        } catch (error) {
            if (requestEpoch === detailRequestEpoch.current) {
                setDetailError(error instanceof Error ? error.message : String(error));
            }
        } finally {
            if (requestEpoch === detailRequestEpoch.current) setDetailLoading(false);
        }
    };

    const loadMoreDetail = async (kind: 'fields' | 'functions' | 'values') => {
        if (!selected || detailLoading) return;
        const requestEpoch = ++detailRequestEpoch.current;
        setDetailLoading(true);
        setDetailError(null);
        try {
            if (kind === 'fields' && fieldCursor) {
                const res = subTab === 'Struct'
                    ? await api.getStructFields(selected.fullName, fieldCursor)
                    : await api.getClassFields(selected.fullName, fieldCursor);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (!res.success || !res.data) throw new Error(res.error || 'Field continuation failed');
                setFields((current) => [...current, ...res.data!.items]);
                setFieldCursor(res.data.next_cursor);
                setFieldHasMore(res.data.has_more);
            } else if (kind === 'functions' && functionCursor) {
                const res = await api.getClassFunctions(selected.fullName, functionCursor);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (!res.success || !res.data) throw new Error(res.error || 'Function continuation failed');
                setFunctions((current) => [...current, ...res.data!.items]);
                setFunctionCursor(res.data.next_cursor);
                setFunctionHasMore(res.data.has_more);
            } else if (kind === 'values' && enumCursor) {
                const res = await api.getEnumValues(selected.fullName, enumCursor);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (!res.success || !res.data) throw new Error(res.error || 'Enum continuation failed');
                setEnumValues((current) => [...current, ...res.data!.items]);
                setEnumCursor(res.data.next_cursor);
                setEnumHasMore(res.data.has_more);
            }
        } catch (error) {
            if (requestEpoch === detailRequestEpoch.current) {
                setDetailError(error instanceof Error ? error.message : String(error));
            }
        } finally {
            if (requestEpoch === detailRequestEpoch.current) setDetailLoading(false);
        }
    };

    useEffect(() => {
        setNextCursor(null);
        setHasMore(false);
        void loadList(null, false);
    }, [loadList]);

    // ─── Virtualization ─────────────────────────────────────────

    const parentRef = useRef<HTMLDivElement>(null);

    const rowVirtualizer = useVirtualizer({
        count: items.length + (hasMore && !listLoading ? 1 : 0),
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
        if (lastItem.index >= items.length - 64 && !listLoading && hasMore && nextCursor) {
            void loadList(nextCursor, true);
        }
    }, [hasMore, items.length, listLoading, loadList, nextCursor, virtualItems]);

    // ─── Render ────────────────────────────────────────────────

    const SUB_TABS: { id: TypeSubTab; icon: typeof Layers; label: string }[] = [
        { id: 'Class', icon: Box, label: 'Class' },
        { id: 'Struct', icon: Database, label: 'Struct' },
        { id: 'Enum', icon: Layers, label: 'Enum' },
    ];
    const displaySize = typeSize ?? selected?.size;

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
                    <DomainError message={listError} compact />

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
                                {displaySize !== undefined && (
                                    <span className="px-3 py-1.5 rounded-md bg-white/[0.03] border border-white/10 text-xs font-mono text-slate-300 shadow-sm backdrop-blur-md">
                                        Size: <span className="text-white/70">0x{displaySize.toString(16).toUpperCase()}</span> <span className="text-slate-500">({displaySize} B)</span>
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
                        <DomainError message={detailError} />
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
                                            {fields.map((f) => (
                                                <tr key={`${f.declaring_type.full_path}:${f.name}:${f.offset}`} className="border-b border-white/[0.03] hover:bg-white/[0.03]">
                                                    <td className="py-1.5 px-2 font-mono text-green-400">+0x{f.offset.toString(16).toUpperCase()}</td>
                                                    <td className="py-1.5 px-2 font-mono text-white/90">{f.name}</td>
                                                    <td className="py-1.5 px-2 font-mono text-blue-400" title={f.reason || undefined}>
                                                        {f.type_name}
                                                        {f.state !== 'supported' && <span className="ml-2 text-yellow-400">({f.state})</span>}
                                                    </td>
                                                    <td className="py-1.5 px-2 font-mono text-white/50">{f.size}</td>
                                                    <td className="py-1.5 px-2 text-white/30">{f.flags}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                    {fields.length === 0 && <div className="text-white/40 text-sm py-3">{t('No fields')}</div>}
                                    <LoadMoreButton visible={fieldHasMore && fieldCursor !== null} loading={detailLoading} onClick={() => void loadMoreDetail('fields')} label={t('Load more')} />
                                </div>
                            </Panel>
                        )}

                        {/* Functions */}
                        {detailTab === 'Functions' && (
                            <Panel title={t('Functions')}>
                                <div className="space-y-2">
                                    {functions.map((fn) => (
                                        <div key={fn.full_path} className="px-4 py-3 rounded-xl border border-white/[0.03] bg-black/10 hover:bg-white/[0.02] transition-colors">
                                            <div className="flex items-center gap-3">
                                                <Hash className="w-4 h-4 text-emerald-500 opacity-80 flex-none" />
                                                <span className="text-[14px] text-slate-200 font-mono font-medium">{fn.name}</span>
                                                <span className="text-[10px] text-emerald-400/70 font-mono">{fn.implementation}</span>
                                                <span className="text-xs text-slate-500 ml-auto font-mono max-w-[200px] truncate" title={fn.flags}>{fn.flags}</span>
                                            </div>
                                            {fn.reason && <div className="mt-2 ml-7 text-xs text-yellow-300">{fn.reason_code}: {fn.reason}</div>}
                                            {fn.parameters.length > 0 && (
                                                <div className="mt-2.5 ml-7 text-xs text-slate-400 font-mono leading-relaxed p-2 bg-black/20 rounded-md border border-white/[0.02]">
                                                    <span className="text-slate-500">{t('Params')}:</span> ({fn.parameters.map((parameter, idx) => <span key={`${parameter.name}:${parameter.offset}`}><span className="text-slate-300">{parameter.name}</span><span className="text-emerald-400/70">: {parameter.type_name}</span>{idx < fn.parameters.length - 1 ? ', ' : ''}</span>)})
                                                </div>
                                            )}
                                        </div>
                                    ))}
                                    {functions.length === 0 && <div className="text-slate-500 text-sm py-4 text-center">{t('No functions')}</div>}
                                    <LoadMoreButton visible={functionHasMore && functionCursor !== null} loading={detailLoading} onClick={() => void loadMoreDetail('functions')} label={t('Load more')} />
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
                                            onClick={() => onSwitchMode?.('instances', { className: selected.fullName, objectIndex: inst.index })}
                                        >
                                            <div className="w-2 h-2 rounded-full bg-emerald-500 shadow-[0_0_8px_rgba(16,185,129,0.5)] flex-none" />
                                            <span className="text-[14px] text-slate-200 font-mono flex-1 truncate">{inst.name}</span>
                                            <span className="text-sm text-slate-500 font-mono">#{inst.index}</span>
                                            <span className="text-xs text-slate-400 font-mono">{formatAddress(inst.address)}</span>
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
                                    State: <span className="text-blue-400 font-mono">{enumDetail.enum.state}</span>
                                    {' | '}Underlying Type: <span className="text-blue-400 font-mono">{enumDetail.enum.underlying_kind || 'unavailable'}</span>
                                </div>
                                {enumDetail.enum.reason && <div className="text-xs text-yellow-300 mb-3">{enumDetail.enum.reason_code}: {enumDetail.enum.reason}</div>}
                                <table className="w-full text-xs">
                                    <thead>
                                        <tr className="text-white/40 border-b border-white/5">
                                            <th className="text-left py-2 px-2 font-medium">#</th>
                                            <th className="text-left py-2 px-2 font-medium">Name</th>
                                            <th className="text-left py-2 px-2 font-medium">Value</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {enumValues.map((v, i) => (
                                            <tr key={`${v.name}:${v.value}`} className="border-b border-white/[0.03] hover:bg-white/[0.03]">
                                                <td className="py-1.5 px-2 text-white/30">{i}</td>
                                                <td className="py-1.5 px-2 font-mono text-white/90">{v.name}</td>
                                                <td className="py-1.5 px-2 font-mono text-yellow-400">{v.value}</td>
                                            </tr>
                                        ))}
                                    </tbody>
                                </table>
                                <LoadMoreButton visible={enumHasMore && enumCursor !== null} loading={detailLoading} onClick={() => void loadMoreDetail('values')} label={t('Load more')} />
                            </Panel>
                        )}
                    </div>
                )}
            </div>
        </div >
    );
}
