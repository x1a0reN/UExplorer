import { useCallback, useEffect, useState } from 'react';
import { t } from '../../i18n';
import { Search, MapPin, Globe, ChevronDown, ChevronRight } from 'lucide-react';
import api, {
    type WorldLevelItem,
    type WorldActorDetail,
    type ObjectItem,
    type Vec3Data,
} from '../../api';
import { Panel, HeaderCard, type BrowserPageProps } from './shared';

// ─── Types ─────────────────────────────────────────────────────

type WorldDetailTab = 'Transform' | 'Components' | 'Properties';
type VecInput = { x: string; y: string; z: string };

function toVecInput(vec?: Vec3Data): VecInput {
    if (!vec) return { x: '', y: '', z: '' };
    return { x: String(vec.x), y: String(vec.y), z: String(vec.z) };
}

// ─── Component ─────────────────────────────────────────────────

export default function WorldBrowser({ onSwitchMode }: BrowserPageProps) {
    // World state
    const [levels, setLevels] = useState<WorldLevelItem[]>([]);
    const [expandedLevels, setExpandedLevels] = useState<Set<string>>(new Set());
    const [actors, setActors] = useState<ObjectItem[]>([]);
    const [search, setSearch] = useState('');
    const [classFilter, setClassFilter] = useState('');
    const [listLoading, setListLoading] = useState(false);
    const [selected, setSelected] = useState<ObjectItem | null>(null);

    // Detail state
    const [detailTab, setDetailTab] = useState<WorldDetailTab>('Transform');
    const [actorDetail, setActorDetail] = useState<WorldActorDetail | null>(null);
    const [components, setComponents] = useState<ObjectItem[]>([]);
    const [detailLoading, setDetailLoading] = useState(false);
    const [detailError, setDetailError] = useState<string | null>(null);

    // Transform inputs
    const [location, setLocation] = useState<VecInput>({ x: '', y: '', z: '' });
    const [rotation, setRotation] = useState<VecInput>({ x: '', y: '', z: '' });
    const [scale, setScale] = useState<VecInput>({ x: '', y: '', z: '' });

    // ─── Data Loading ──────────────────────────────────────────

    const loadWorld = useCallback(async () => {
        setListLoading(true);
        try {
            const levelsRes = await api.getWorldLevels();
            if (levelsRes.success && levelsRes.data) {
                setLevels(levelsRes.data.levels);
                // Auto-expand first level
                if (levelsRes.data.levels.length > 0 && levelsRes.data.levels[0].source) {
                    setExpandedLevels(new Set([levelsRes.data.levels[0].source!]));
                }
            }
        } catch { /* ignore */ }
        setListLoading(false);
    }, []);

    const loadActors = useCallback(async () => {
        try {
            const res = await api.getWorldActors(0, 128, search, classFilter);
            if (res.success && res.data) setActors(res.data.items);
        } catch { /* ignore */ }
    }, [classFilter, search]);

    const loadActorDetail = async (actor: ObjectItem) => {
        setDetailLoading(true);
        setDetailError(null);
        setActorDetail(null);
        setComponents([]);
        setDetailTab('Transform');
        try {
            const [detailRes, compRes] = await Promise.all([
                api.getWorldActorDetail(actor.index),
                api.getWorldActorComponents(actor.index),
            ]);
            if (detailRes.success && detailRes.data) {
                setActorDetail(detailRes.data);
                setLocation(toVecInput(detailRes.data.transform?.location));
                setRotation(toVecInput(detailRes.data.transform?.rotation));
                setScale(toVecInput(detailRes.data.transform?.scale));
            }
            if (compRes.success && compRes.data) setComponents(compRes.data.components);
        } catch (error) {
            setDetailError(error instanceof Error ? error.message : String(error));
        } finally {
            setDetailLoading(false);
        }
    };

    useEffect(() => {
        const timer = window.setTimeout(() => void loadWorld(), 0);
        return () => window.clearTimeout(timer);
    }, [loadWorld]);

    useEffect(() => {
        const timer = window.setTimeout(() => void loadActors(), 150);
        return () => window.clearTimeout(timer);
    }, [loadActors]);

    // ─── Helpers ───────────────────────────────────────────────

    const toggleLevel = (source: string) => {
        setExpandedLevels((prev) => {
            const next = new Set(prev);
            if (next.has(source)) next.delete(source);
            else next.add(source);
            return next;
        });
    };

    const VecEditor = ({ label, value }: { label: string; value: VecInput }) => (
        <div className="grid grid-cols-[80px_1fr_1fr_1fr] gap-2 items-center">
            <span className="text-white/50 text-xs">{label}</span>
            {(['x', 'y', 'z'] as const).map((axis) => (
                <input key={axis} type="text" value={value[axis]}
                    readOnly
                    placeholder={axis.toUpperCase()}
                    className="h-7 bg-white/[0.03] border border-white/5 rounded text-xs text-white/60 font-mono px-2 text-center cursor-not-allowed" />
            ))}
        </div>
    );

    // ─── Render ────────────────────────────────────────────────

    return (
        <div className="flex h-full">
            {/* ── Left: World Tree ── */}
            <div className="w-80 border-r border-white/5 flex flex-col flex-none bg-black/30">
                {/* Search */}
                <div className="p-3 space-y-2 border-b border-white/5">
                    <div className="relative">
                        <Search className="w-3.5 h-3.5 absolute left-3 top-1/2 -translate-y-1/2 text-white/30" />
                        <input type="text" value={search} onChange={(e) => setSearch(e.target.value)}
                            placeholder={t('Search actors...')}
                            className="w-full h-8 bg-white/5 border border-white/10 rounded-lg text-xs text-white px-3 pl-9 focus:outline-none focus:border-white/20" />
                    </div>
                    <input type="text" value={classFilter} onChange={(e) => setClassFilter(e.target.value)}
                        placeholder={t('Filter by class...')}
                        className="w-full h-7 bg-white/5 border border-white/10 rounded-lg text-xs text-white px-3 focus:outline-none focus:border-white/20" />
                </div>

                {/* Level Tree */}
                <div className="flex-1 overflow-auto px-2 pt-1">
                    {listLoading && <div className="text-white/40 text-xs p-3">{t('Loading...')}</div>}
                    {levels.map((level) => {
                        const src = level.source || 'Unknown';
                        const expanded = expandedLevels.has(src);
                        return (
                            <div key={src}>
                                <div className="flex items-center gap-1.5 py-1.5 px-1 cursor-pointer hover:bg-white/5 rounded"
                                    onClick={() => toggleLevel(src)}>
                                    {expanded ? <ChevronDown className="w-3.5 h-3.5 text-white/30" /> : <ChevronRight className="w-3.5 h-3.5 text-white/30" />}
                                    <Globe className="w-3.5 h-3.5 text-cyan-400" />
                                    <span className="text-[12px] text-white/80 font-mono truncate">{src}</span>
                                    <span className="text-[10px] text-white/30 ml-auto">({level.actor_count ?? 0})</span>
                                </div>
                                {expanded && actors.map((actor) => (
                                    <div key={actor.index}
                                        onClick={() => { setSelected(actor); void loadActorDetail(actor); }}
                                        className={`ml-5 p-1.5 rounded cursor-pointer mb-0.5 flex items-center gap-2 transition-all ${selected?.index === actor.index ? 'bg-white/10 border border-white/10' : 'hover:bg-white/5 border border-transparent'
                                            }`}>
                                        <MapPin className="w-3 h-3 text-cyan-400/60 flex-none" />
                                        <span className="text-[12px] text-white/80 font-mono truncate">{actor.name}</span>
                                    </div>
                                ))}
                            </div>
                        );
                    })}
                </div>
            </div>

            {/* ── Right: Actor Detail ── */}
            <div className="flex-1 overflow-auto p-8">
                {!selected && <div className="text-white/40 text-sm">{t('Select an actor from the world tree.')}</div>}
                {selected && (
                    <div className="max-w-5xl space-y-6">
                        <HeaderCard
                            icon={MapPin}
                            name={selected.name}
                            subtitle={actorDetail?.full_name || selected.class}
                            gradient="from-cyan-500/20 to-blue-500/10"
                            iconColor="text-cyan-400"
                            glow="bg-cyan-500/20"
                            badges={<>
                                <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-mono text-blue-400 cursor-pointer hover:bg-blue-500/20"
                                    onClick={() => onSwitchMode?.('types', { className: actorDetail?.class || selected.class })}>
                                    {actorDetail?.class || selected.class}
                                </span>
                                <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/70">
                                    #{selected.index}
                                </span>
                            </>}
                        />

                        {detailLoading && <div className="text-white/40 text-sm">{t('Loading...')}</div>}
                        {detailError && <div className="text-red-300 text-sm">{detailError}</div>}

                        {/* Tab Bar */}
                        <div className="flex gap-1 border-b border-white/5 pb-2">
                            {(['Transform', 'Components'] as const).map((tab) => (
                                <button key={tab} onClick={() => setDetailTab(tab)}
                                    className={`px-3 py-1.5 text-xs font-medium rounded-lg transition-all ${detailTab === tab ? 'bg-white/10 text-white' : 'text-white/40 hover:text-white/70'}`}>
                                    {t(tab)}
                                </button>
                            ))}
                        </div>

                        {/* Transform Tab */}
                        {detailTab === 'Transform' && (
                            <Panel title={t('Transform')}>
                                <div className="space-y-3">
                                    <VecEditor label="Location" value={location} />
                                    <VecEditor label="Rotation" value={rotation} />
                                    <VecEditor label="Scale" value={scale} />
                                    <div className="mt-4 rounded-lg border border-amber-500/20 bg-amber-500/10 px-3 py-2 text-xs text-amber-200">
                                        Transform editing is disabled until a verified game-thread Unreal setter is available.
                                    </div>
                                </div>
                            </Panel>
                        )}

                        {/* Components Tab */}
                        {detailTab === 'Components' && (
                            <Panel title={t('Components')}>
                                <div className="space-y-1">
                                    {components.map((comp) => (
                                        <div key={comp.index}
                                            className="flex items-center gap-3 p-2 rounded-lg border border-white/5 bg-black/20 hover:bg-white/5 cursor-pointer"
                                            onClick={() => onSwitchMode?.('instances', { objectIndex: comp.index })}>
                                            <div className="w-2 h-2 rounded-full bg-cyan-400/60 flex-none" />
                                            <span className="text-[13px] text-white/90 font-mono flex-1 truncate">{comp.name}</span>
                                            <span className="text-[11px] text-blue-400/60 font-mono">{comp.class}</span>
                                        </div>
                                    ))}
                                    {components.length === 0 && <div className="text-white/40 text-sm">{t('No components')}</div>}
                                </div>
                            </Panel>
                        )}
                    </div>
                )}
            </div>
        </div>
    );
}
