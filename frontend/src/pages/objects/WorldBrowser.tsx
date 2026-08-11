import { useCallback, useEffect, useRef, useState } from 'react';
import { t } from '../../i18n';
import { Search, MapPin, Globe, ChevronDown, ChevronRight } from 'lucide-react';
import api, {
    type WorldLevelItem,
    type WorldActorDetail,
    type WorldActorItem,
    type WorldQueryCursor,
    type WorldSnapshotObject,
} from '../../api';
import { Panel, HeaderCard, type BrowserPageProps } from './shared';

// ─── Types ─────────────────────────────────────────────────────

type WorldDetailTab = 'Transform' | 'Components';

type TransformPrecision = 'float32' | 'float64';

interface ParsedMathStruct {
    precision: TransformPrecision;
    values: Record<string, number>;
}

type LiveRelativeTransform =
    | { state: 'idle' | 'loading' }
    | {
        state: 'available';
        precision: TransformPrecision;
        location: { x: number; y: number; z: number };
        rotation: { pitch: number; yaw: number; roll: number };
        scale: { x: number; y: number; z: number };
    }
    | { state: 'unavailable'; reasonCode: string; reason: string };

const SCENE_COMPONENT_PATH = '/Script/Engine.SceneComponent';
const VECTOR_PATH = '/Script/CoreUObject.Vector';
const ROTATOR_PATH = '/Script/CoreUObject.Rotator';

function asRecord(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

function parseMathStruct(
    value: unknown,
    exactTypePath: string,
    fieldNames: readonly string[],
): ParsedMathStruct | null {
    const root = asRecord(value);
    if (!root
        || root.state !== 'ok'
        || root.kind !== 'struct'
        || root.type_name !== exactTypePath
        || root.truncated !== false
        || root.total_count !== fieldNames.length
        || !Array.isArray(root.children)
        || root.children.length !== fieldNames.length) {
        return null;
    }

    let scalarKind: 'float' | 'double' | null = null;
    const values: Record<string, number> = {};
    for (let index = 0; index < fieldNames.length; index += 1) {
        const child = asRecord(root.children[index]);
        const scalar = child ? asRecord(child.scalar) : null;
        const fieldName = fieldNames[index];
        if (!child
            || !scalar
            || typeof fieldName !== 'string'
            || child.state !== 'ok'
            || child.label !== fieldName
            || (child.kind !== 'float' && child.kind !== 'double')
            || scalar.kind !== 'float64'
            || typeof scalar.value !== 'string'
            || scalar.value.trim().length === 0) {
            return null;
        }
        if (scalarKind === null) scalarKind = child.kind;
        if (child.kind !== scalarKind) return null;
        const parsed = Number(scalar.value);
        if (!Number.isFinite(parsed)) return null;
        values[fieldName] = parsed;
    }
    if (scalarKind === null) return null;
    return {
        precision: scalarKind === 'float' ? 'float32' : 'float64',
        values,
    };
}

async function readRootRelativeTransform(
    detail: WorldActorDetail,
): Promise<LiveRelativeTransform> {
    const root = detail.root_component.object;
    if (detail.root_component.state !== 'present' || !root) {
        return {
            state: 'unavailable',
            reasonCode: detail.root_component.reason_code || 'WORLD_ROOT_COMPONENT_NOT_PRESENT',
            reason: detail.root_component.reason || 'The actor has no witnessed root scene component.',
        };
    }

    const requests = [
        { property: 'RelativeLocation', typePath: VECTOR_PATH, fields: ['X', 'Y', 'Z'] },
        { property: 'RelativeRotation', typePath: ROTATOR_PATH, fields: ['Pitch', 'Yaw', 'Roll'] },
        { property: 'RelativeScale3D', typePath: VECTOR_PATH, fields: ['X', 'Y', 'Z'] },
    ] as const;
    const responses = await Promise.all(requests.map((request) => api.readExactObjectProperty(
        root.handle,
        detail.type_snapshot_generation,
        SCENE_COMPONENT_PATH,
        request.property,
    )));
    const failed = responses.find((response) => !response.success || !response.data);
    if (failed) {
        return {
            state: 'unavailable',
            reasonCode: failed.error_code || 'WORLD_TRANSFORM_READ_FAILED',
            reason: failed.error || 'An exact root-component transform property could not be read.',
        };
    }

    const location = responses[0].data
        ? parseMathStruct(responses[0].data.value, requests[0].typePath, requests[0].fields)
        : null;
    const rotation = responses[1].data
        ? parseMathStruct(responses[1].data.value, requests[1].typePath, requests[1].fields)
        : null;
    const scale = responses[2].data
        ? parseMathStruct(responses[2].data.value, requests[2].typePath, requests[2].fields)
        : null;
    if (!location || !rotation || !scale
        || location.precision !== rotation.precision
        || location.precision !== scale.precision) {
        return {
            state: 'unavailable',
            reasonCode: 'WORLD_TRANSFORM_VALUE_INVALID',
            reason: 'The exact FVector/FRotator values or their scalar precision were inconsistent.',
        };
    }
    return {
        state: 'available',
        precision: location.precision,
        location: {
            x: location.values.X,
            y: location.values.Y,
            z: location.values.Z,
        },
        rotation: {
            pitch: rotation.values.Pitch,
            yaw: rotation.values.Yaw,
            roll: rotation.values.Roll,
        },
        scale: {
            x: scale.values.X,
            y: scale.values.Y,
            z: scale.values.Z,
        },
    };
}

// ─── Component ─────────────────────────────────────────────────

export default function WorldBrowser({ onSwitchMode }: BrowserPageProps) {
    const detailRequestEpoch = useRef(0);
    // World state
    const [levels, setLevels] = useState<WorldLevelItem[]>([]);
    const [nextLevelCursor, setNextLevelCursor] = useState<WorldQueryCursor | null>(null);
    const [expandedLevels, setExpandedLevels] = useState<Set<string>>(new Set());
    const [actors, setActors] = useState<WorldActorItem[]>([]);
    const [nextActorCursor, setNextActorCursor] = useState<WorldQueryCursor | null>(null);
    const [actorMatched, setActorMatched] = useState(0);
    const [actorGeneration, setActorGeneration] = useState<number | null>(null);
    const [search, setSearch] = useState('');
    const [classFilter, setClassFilter] = useState('');
    const [listLoading, setListLoading] = useState(false);
    const [actorListLoading, setActorListLoading] = useState(false);
    const [selected, setSelected] = useState<WorldActorItem | null>(null);

    // Detail state
    const [detailTab, setDetailTab] = useState<WorldDetailTab>('Transform');
    const [actorDetail, setActorDetail] = useState<WorldActorDetail | null>(null);
    const [components, setComponents] = useState<WorldSnapshotObject[]>([]);
    const [nextComponentCursor, setNextComponentCursor] = useState<WorldQueryCursor | null>(null);
    const [componentLoading, setComponentLoading] = useState(false);
    const [detailLoading, setDetailLoading] = useState(false);
    const [detailError, setDetailError] = useState<string | null>(null);
    const [relativeTransform, setRelativeTransform] = useState<LiveRelativeTransform>({ state: 'idle' });

    // ─── Data Loading ──────────────────────────────────────────

    const loadWorld = useCallback(async (
        cursor: WorldQueryCursor | null = null,
        append = false,
    ) => {
        setListLoading(true);
        if (!append) {
            setLevels([]);
            setNextLevelCursor(null);
        }
        try {
            const levelsRes = await api.getWorldLevels(cursor);
            if (levelsRes.success && levelsRes.data) {
                setLevels((current) => append
                    ? [...current, ...levelsRes.data!.levels]
                    : levelsRes.data!.levels);
                setNextLevelCursor(levelsRes.data.next_cursor);
                // Auto-expand first level
                if (!append && levelsRes.data.levels.length > 0) {
                    setExpandedLevels(new Set([levelsRes.data.levels[0].source]));
                }
            }
        } catch { /* ignore */ }
        setListLoading(false);
    }, []);

    const loadActors = useCallback(async (
        cursor: WorldQueryCursor | null = null,
        append = false,
    ) => {
        setActorListLoading(true);
        if (!append) {
            detailRequestEpoch.current += 1;
            setActors([]);
            setNextActorCursor(null);
            setActorMatched(0);
            setActorGeneration(null);
            setSelected(null);
            setActorDetail(null);
            setComponents([]);
            setNextComponentCursor(null);
            setRelativeTransform({ state: 'idle' });
        }
        try {
            const res = await api.getWorldActors(cursor, 128, search, classFilter);
            if (res.success && res.data) {
                setActors((current) => append ? [...current, ...res.data!.items] : res.data!.items);
                setNextActorCursor(res.data.next_cursor);
                setActorMatched(res.data.matched);
                setActorGeneration(res.data.generation);
            }
        } catch { /* ignore */ }
        setActorListLoading(false);
    }, [classFilter, search]);

    const loadActorDetail = async (actor: WorldActorItem) => {
        const requestEpoch = ++detailRequestEpoch.current;
        setDetailLoading(true);
        setDetailError(null);
        setActorDetail(null);
        setComponents([]);
        setNextComponentCursor(null);
        setDetailTab('Transform');
        setRelativeTransform({ state: 'loading' });
        const generation = actorGeneration;
        if (generation === null) {
            setDetailError('WORLD_SNAPSHOT_STALE: actor list generation is unavailable');
            setRelativeTransform({
                state: 'unavailable',
                reasonCode: 'WORLD_SNAPSHOT_STALE',
                reason: 'The actor list generation is unavailable.',
            });
            setDetailLoading(false);
            return;
        }
        try {
            const detailRes = await api.getWorldActorDetail(actor.handle, generation);
            if (requestEpoch !== detailRequestEpoch.current) return;
            if (detailRes.success && detailRes.data) {
                setActorDetail(detailRes.data);
                const transform = await readRootRelativeTransform(detailRes.data);
                if (requestEpoch !== detailRequestEpoch.current) return;
                setRelativeTransform(transform);
                if (detailRes.data.components.state === 'available') {
                    const compRes = await api.getWorldActorComponents(actor.handle, generation);
                    if (requestEpoch !== detailRequestEpoch.current) return;
                    if (compRes.success && compRes.data) {
                        setComponents(compRes.data.components);
                        setNextComponentCursor(compRes.data.next_cursor);
                    } else {
                        setDetailError(compRes.error || compRes.error_code || 'World components unavailable');
                    }
                }
            } else {
                setDetailError(detailRes.error || detailRes.error_code || 'World detail unavailable');
                setRelativeTransform({
                    state: 'unavailable',
                    reasonCode: detailRes.error_code || 'WORLD_DETAIL_UNAVAILABLE',
                    reason: detailRes.error || 'World detail is unavailable.',
                });
            }
        } catch (error) {
            if (requestEpoch === detailRequestEpoch.current) {
                const reason = error instanceof Error ? error.message : String(error);
                setDetailError(reason);
                setRelativeTransform({
                    state: 'unavailable',
                    reasonCode: 'WORLD_TRANSFORM_READ_FAILED',
                    reason,
                });
            }
        } finally {
            if (requestEpoch === detailRequestEpoch.current) setDetailLoading(false);
        }
    };

    const loadMoreComponents = async () => {
        if (!selected || !actorDetail || !nextComponentCursor || componentLoading) return;
        setComponentLoading(true);
        try {
            const response = await api.getWorldActorComponents(
                selected.handle,
                actorDetail.generation,
                nextComponentCursor,
            );
            if (response.success && response.data) {
                setComponents((current) => [...current, ...response.data!.components]);
                setNextComponentCursor(response.data.next_cursor);
            } else {
                setDetailError(response.error || response.error_code || 'World components unavailable');
            }
        } catch (error) {
            setDetailError(error instanceof Error ? error.message : String(error));
        } finally {
            setComponentLoading(false);
        }
    };

    useEffect(() => {
        const timer = window.setTimeout(() => void loadWorld(null, false), 0);
        return () => window.clearTimeout(timer);
    }, [loadWorld]);

    useEffect(() => {
        const timer = window.setTimeout(() => void loadActors(null, false), 150);
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
                                {expanded && actors
                                    .filter((actor) => actor.level.full_path === src)
                                    .map((actor) => (
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
                    {nextLevelCursor && (
                        <button type="button"
                            disabled={listLoading}
                            onClick={() => void loadWorld(nextLevelCursor, true)}
                            className="w-full my-1 rounded border border-white/10 px-2 py-1.5 text-[11px] text-white/50 hover:bg-white/5 disabled:opacity-40">
                            {t('Load more levels')}
                        </button>
                    )}
                    {nextActorCursor && (
                        <button type="button"
                            disabled={actorListLoading}
                            onClick={() => void loadActors(nextActorCursor, true)}
                            className="w-full my-1 rounded border border-cyan-400/15 px-2 py-1.5 text-[11px] text-cyan-200/60 hover:bg-cyan-400/5 disabled:opacity-40">
                            {t('Load more actors')} ({actors.length}/{actorMatched})
                        </button>
                    )}
                    {actorListLoading && <div className="text-white/30 text-[11px] p-2">{t('Loading actors...')}</div>}
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
                            subtitle={actorDetail?.actor.full_path || selected.class_path}
                            gradient="from-cyan-500/20 to-blue-500/10"
                            iconColor="text-cyan-400"
                            glow="bg-cyan-500/20"
                            badges={<>
                                <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-mono text-blue-400 cursor-pointer hover:bg-blue-500/20"
                                    onClick={() => onSwitchMode?.('types', { className: actorDetail?.actor.class_path || selected.class_path })}>
                                    {actorDetail?.actor.class_path || selected.class_path}
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
                                {relativeTransform.state === 'loading' && (
                                    <div className="text-xs text-white/40">{t('Loading...')}</div>
                                )}
                                {relativeTransform.state === 'available' && (
                                    <div className="space-y-3">
                                        <div className="flex items-center justify-between text-[11px] text-white/40">
                                            <span>Stored USceneComponent Relative* fields</span>
                                            <span className="rounded border border-cyan-400/20 bg-cyan-400/5 px-2 py-0.5 font-mono text-cyan-200/70">
                                                {relativeTransform.precision}
                                            </span>
                                        </div>
                                        <div className="grid gap-3 xl:grid-cols-3">
                                            {[
                                                { label: 'Location', values: [['X', relativeTransform.location.x], ['Y', relativeTransform.location.y], ['Z', relativeTransform.location.z]] },
                                                { label: 'Rotation', values: [['Pitch', relativeTransform.rotation.pitch], ['Yaw', relativeTransform.rotation.yaw], ['Roll', relativeTransform.rotation.roll]] },
                                                { label: 'Scale', values: [['X', relativeTransform.scale.x], ['Y', relativeTransform.scale.y], ['Z', relativeTransform.scale.z]] },
                                            ].map((group) => (
                                                <div key={group.label} className="rounded-lg border border-white/5 bg-black/20 p-3">
                                                    <div className="mb-2 text-[11px] font-medium uppercase tracking-wide text-white/40">{group.label}</div>
                                                    <div className="space-y-1.5">
                                                        {group.values.map(([axis, value]) => (
                                                            <div key={axis} className="flex items-center justify-between gap-3 font-mono text-xs">
                                                                <span className="text-white/35">{axis}</span>
                                                                <span className="truncate text-white/85">{String(value)}</span>
                                                            </div>
                                                        ))}
                                                    </div>
                                                </div>
                                            ))}
                                        </div>
                                        <div className="text-[11px] text-white/30">
                                            bAbsoluteLocation/Rotation/Scale are not included yet; these are the exact stored Relative* values, not a computed world transform.
                                        </div>
                                    </div>
                                )}
                                {relativeTransform.state === 'unavailable' && (
                                    <div className="rounded-lg border border-amber-500/20 bg-amber-500/10 px-3 py-2 text-xs text-amber-200">
                                        <span className="mr-2 font-mono text-amber-300/70">{relativeTransform.reasonCode}</span>
                                        {relativeTransform.reason}
                                    </div>
                                )}
                                {relativeTransform.state === 'idle' && actorDetail && (
                                    <div className="rounded-lg border border-amber-500/20 bg-amber-500/10 px-3 py-2 text-xs text-amber-200">
                                        {actorDetail.transform.reason}
                                    </div>
                                )}
                            </Panel>
                        )}

                        {/* Components Tab */}
                        {detailTab === 'Components' && (
                            <Panel title={t('Components')}>
                                <div className="space-y-1">
                                    {actorDetail?.root_component.state === 'present' && actorDetail.root_component.object && (
                                        <div className="mb-2 rounded-lg border border-cyan-400/15 bg-cyan-400/5 px-3 py-2 text-xs text-cyan-100/70">
                                            Root: <span className="font-mono">{actorDetail.root_component.object.name}</span>
                                        </div>
                                    )}
                                    {actorDetail?.components.state === 'unavailable' && (
                                        <div className="rounded-lg border border-amber-500/20 bg-amber-500/10 px-3 py-2 text-xs text-amber-200">
                                            {actorDetail.components.reason}
                                        </div>
                                    )}
                                    {components.map((comp) => (
                                        <div key={comp.index}
                                            className="flex items-center gap-3 p-2 rounded-lg border border-white/5 bg-black/20 hover:bg-white/5 cursor-pointer"
                                            onClick={() => onSwitchMode?.('instances', { objectIndex: comp.index })}>
                                            <div className="w-2 h-2 rounded-full bg-cyan-400/60 flex-none" />
                                            <span className="text-[13px] text-white/90 font-mono flex-1 truncate">{comp.name}</span>
                                            <span className="text-[11px] text-blue-400/60 font-mono">{comp.class_path}</span>
                                        </div>
                                    ))}
                                    {nextComponentCursor && (
                                        <button type="button"
                                            disabled={componentLoading}
                                            onClick={() => void loadMoreComponents()}
                                            className="w-full mt-2 rounded border border-cyan-400/15 px-2 py-1.5 text-[11px] text-cyan-200/60 hover:bg-cyan-400/5 disabled:opacity-40">
                                            {componentLoading ? t('Loading...') : t('Load more components')}
                                            {actorDetail?.components.count !== null && ` (${components.length}/${actorDetail?.components.count ?? 0})`}
                                        </button>
                                    )}
                                    {!detailLoading && !detailError
                                        && actorDetail?.components.state === 'available'
                                        && components.length === 0 && (
                                        <div className="text-white/40 text-sm">{t('No components')}</div>
                                    )}
                                </div>
                            </Panel>
                        )}
                    </div>
                )}
            </div>
        </div>
    );
}
