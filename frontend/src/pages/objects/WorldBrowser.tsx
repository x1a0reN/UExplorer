import { useCallback, useEffect, useRef, useState } from 'react';
import { t } from '../../i18n';
import { Search, MapPin, Globe, ChevronDown, ChevronRight, RefreshCw, Save } from 'lucide-react';
import api from '../../services';
import {
    type WorldLevelItem,
    type WorldActorDetail,
    type WorldActorComputedTransform,
    type WorldActorItem,
    type WorldActorStoredTransform,
    type WorldActorTransformResponse,
    type WorldActorTransformUpdate,
    type WorldQueryCursor,
    type WorldSnapshotObject,
    type WorldTransformSpace,
} from '../../contracts';
import { Panel, HeaderCard, type BrowserPageProps } from './shared';
import { isAbortError, useQueryRunner } from '../../features/query/useQueryRunner';
import { useDebouncedValue } from '../../features/query/useDebouncedValue';
import { DomainError } from '../../features/shared/DomainError';

// ─── Types ─────────────────────────────────────────────────────

type WorldDetailTab = 'Transform' | 'Components';

type LiveRelativeTransform =
    | { state: 'idle' | 'loading' }
    | {
        state: 'available';
        value: WorldActorStoredTransform;
        computed: WorldActorComputedTransform;
        scope: WorldActorTransformResponse;
    }
    | { state: 'unavailable'; reasonCode: string; reason: string };

type TransformEditorField = 'location' | 'rotation' | 'scale';

interface TransformEditorState {
    field: TransformEditorField;
    space: WorldTransformSpace;
    components: [string, string, string];
    sweep: boolean;
    teleport: boolean;
    teleportPhysics: boolean;
}

type MutationFeedback =
    | { kind: 'success'; message: string }
    | { kind: 'error'; message: string }
    | null;

const EMPTY_TRANSFORM_EDITOR: TransformEditorState = {
    field: 'location',
    space: 'world',
    components: ['', '', ''],
    sweep: false,
    teleport: false,
    teleportPhysics: false,
};

function currentTransformComponents(
    scope: WorldActorTransformResponse,
    field: TransformEditorField,
    space: WorldTransformSpace,
): [string, string, string] {
    if (space === 'world' && scope.computed_transform.state === 'available') {
        const computed = scope.computed_transform;
        if (field === 'location') {
            return [computed.location.x, computed.location.y, computed.location.z].map(String) as [string, string, string];
        }
        if (field === 'rotation') {
            return [computed.rotation.pitch, computed.rotation.yaw, computed.rotation.roll].map(String) as [string, string, string];
        }
        return [computed.scale.x, computed.scale.y, computed.scale.z].map(String) as [string, string, string];
    }

    const stored = scope.transform;
    if (field === 'location') {
        return [stored.location.x, stored.location.y, stored.location.z].map(String) as [string, string, string];
    }
    if (field === 'rotation') {
        return [stored.rotation.pitch, stored.rotation.yaw, stored.rotation.roll].map(String) as [string, string, string];
    }
    return [stored.scale.x, stored.scale.y, stored.scale.z].map(String) as [string, string, string];
}

function createTransformEditor(
    scope: WorldActorTransformResponse,
    field: TransformEditorField,
    space: WorldTransformSpace,
    previous: TransformEditorState = EMPTY_TRANSFORM_EDITOR,
): TransformEditorState {
    return {
        ...previous,
        field,
        space,
        components: currentTransformComponents(scope, field, space),
    };
}

// ─── Component ─────────────────────────────────────────────────

export default function WorldBrowser({ onSwitchMode }: BrowserPageProps) {
    const { run } = useQueryRunner();
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
    const debouncedSearch = useDebouncedValue(search);
    const debouncedClassFilter = useDebouncedValue(classFilter);
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
    const [transformEditor, setTransformEditor] = useState<TransformEditorState>(EMPTY_TRANSFORM_EDITOR);
    const [mutationBusy, setMutationBusy] = useState(false);
    const [mutationFeedback, setMutationFeedback] = useState<MutationFeedback>(null);

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
            setTransformEditor(EMPTY_TRANSFORM_EDITOR);
            setMutationBusy(false);
            setMutationFeedback(null);
        }
        try {
            const res = await run('world:actors', async () => api.getWorldActors(
                cursor,
                128,
                debouncedSearch,
                debouncedClassFilter,
            ));
            if (res.success && res.data) {
                setActors((current) => append ? [...current, ...res.data!.items] : res.data!.items);
                setNextActorCursor(res.data.next_cursor);
                setActorMatched(res.data.matched);
                setActorGeneration(res.data.generation);
            }
        } catch (error) {
            if (!isAbortError(error)) setDetailError(error instanceof Error ? error.message : String(error));
        }
        setActorListLoading(false);
    }, [debouncedClassFilter, debouncedSearch, run]);

    const loadActorDetail = async (actor: WorldActorItem) => {
        const requestEpoch = ++detailRequestEpoch.current;
        setDetailLoading(true);
        setDetailError(null);
        setActorDetail(null);
        setComponents([]);
        setNextComponentCursor(null);
        setDetailTab('Transform');
        setRelativeTransform({ state: 'loading' });
        setTransformEditor(EMPTY_TRANSFORM_EDITOR);
        setMutationBusy(false);
        setMutationFeedback(null);
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
                const transformRes = await api.getWorldActorTransform(actor.handle, generation);
                if (requestEpoch !== detailRequestEpoch.current) return;
                if (transformRes.success && transformRes.data) {
                    setRelativeTransform({
                        state: 'available',
                        value: transformRes.data.transform,
                        computed: transformRes.data.computed_transform,
                        scope: transformRes.data,
                    });
                    setTransformEditor(createTransformEditor(
                        transformRes.data,
                        'location',
                        'world',
                    ));
                } else {
                    setRelativeTransform({
                        state: 'unavailable',
                        reasonCode: transformRes.error_code || 'WORLD_TRANSFORM_READ_FAILED',
                        reason: transformRes.error || 'The same-frame root-component transform could not be read.',
                    });
                }
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

    const selectTransformEditor = (
        field: TransformEditorField,
        space: WorldTransformSpace,
    ) => {
        setTransformEditor((current) => relativeTransform.state === 'available'
            ? createTransformEditor(relativeTransform.scope, field, space, current)
            : { ...current, field, space });
        setMutationFeedback(null);
    };

    const reloadTransformEditor = () => {
        if (relativeTransform.state !== 'available') return;
        setTransformEditor((current) => createTransformEditor(
            relativeTransform.scope,
            current.field,
            current.space,
            current,
        ));
        setMutationFeedback(null);
    };

    const applyTransformMutation = async () => {
        if (relativeTransform.state !== 'available' || mutationBusy) return;
        const components = transformEditor.components.map((component) => component.trim());
        if (components.some((component) => component === '' || !Number.isFinite(Number(component)))) {
            setMutationFeedback({ kind: 'error', message: 'All three components must be finite numbers.' });
            return;
        }

        let update: WorldActorTransformUpdate;
        if (transformEditor.field === 'location') {
            update = {
                field: 'location',
                space: transformEditor.space,
                value: { x: components[0], y: components[1], z: components[2] },
                sweep: transformEditor.sweep,
                teleport: transformEditor.teleport,
            };
        } else if (transformEditor.field === 'rotation' && transformEditor.space === 'relative') {
            update = {
                field: 'rotation',
                space: 'relative',
                value: { pitch: components[0], yaw: components[1], roll: components[2] },
                sweep: transformEditor.sweep,
                teleport: transformEditor.teleport,
            };
        } else if (transformEditor.field === 'rotation') {
            update = {
                field: 'rotation',
                space: 'world',
                value: { pitch: components[0], yaw: components[1], roll: components[2] },
                teleport_physics: transformEditor.teleportPhysics,
            };
        } else {
            update = {
                field: 'scale',
                space: transformEditor.space,
                value: { x: components[0], y: components[1], z: components[2] },
            };
        }

        const requestEpoch = detailRequestEpoch.current;
        const scope = relativeTransform.scope;
        setMutationBusy(true);
        setMutationFeedback(null);
        try {
            const response = await api.updateWorldActorTransform(scope, update);
            if (requestEpoch !== detailRequestEpoch.current) return;
            if (!response.success || !response.data) {
                setMutationFeedback({
                    kind: 'error',
                    message: [response.error_code, response.error].filter(Boolean).join(' · ') || 'Transform update failed.',
                });
                return;
            }

            setMutationFeedback({
                kind: 'success',
                message: `${response.data.setter.function_path} · ${response.data.execution.mutation_state}`,
            });
            const refreshed = await api.getWorldActorTransform(scope.actor.handle, scope.generation);
            if (requestEpoch !== detailRequestEpoch.current) return;
            if (refreshed.success && refreshed.data) {
                setRelativeTransform({
                    state: 'available',
                    value: refreshed.data.transform,
                    computed: refreshed.data.computed_transform,
                    scope: refreshed.data,
                });
                setTransformEditor((current) => createTransformEditor(
                    refreshed.data!,
                    current.field,
                    current.space,
                    current,
                ));
            }
        } catch (error) {
            if (requestEpoch === detailRequestEpoch.current) {
                setMutationFeedback({
                    kind: 'error',
                    message: error instanceof Error ? error.message : String(error),
                });
            }
        } finally {
            if (requestEpoch === detailRequestEpoch.current) setMutationBusy(false);
        }
    };

    useEffect(() => {
        const timer = window.setTimeout(() => void loadWorld(null, false), 0);
        return () => window.clearTimeout(timer);
    }, [loadWorld]);

    useEffect(() => {
        void loadActors(null, false);
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

    const editorAxes = transformEditor.field === 'rotation'
        ? ['Pitch', 'Yaw', 'Roll']
        : ['X', 'Y', 'Z'];
    const usesSweepOptions = transformEditor.field === 'location'
        || (transformEditor.field === 'rotation' && transformEditor.space === 'relative');

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
                        <DomainError message={detailError} />

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
                                                {relativeTransform.value.precision}
                                            </span>
                                        </div>
                                        <div className="grid gap-3 xl:grid-cols-3">
                                            {[
                                                {
                                                    label: 'Location',
                                                    space: relativeTransform.value.space.location,
                                                    absolute: relativeTransform.value.absolute.location,
                                                    values: [['X', relativeTransform.value.location.x], ['Y', relativeTransform.value.location.y], ['Z', relativeTransform.value.location.z]],
                                                },
                                                {
                                                    label: 'Rotation',
                                                    space: relativeTransform.value.space.rotation,
                                                    absolute: relativeTransform.value.absolute.rotation,
                                                    values: [['Pitch', relativeTransform.value.rotation.pitch], ['Yaw', relativeTransform.value.rotation.yaw], ['Roll', relativeTransform.value.rotation.roll]],
                                                },
                                                {
                                                    label: 'Scale',
                                                    space: relativeTransform.value.space.scale,
                                                    absolute: relativeTransform.value.absolute.scale,
                                                    values: [['X', relativeTransform.value.scale.x], ['Y', relativeTransform.value.scale.y], ['Z', relativeTransform.value.scale.z]],
                                                },
                                            ].map((group) => (
                                                <div key={group.label} className="rounded-lg border border-white/5 bg-black/20 p-3">
                                                    <div className="mb-2 flex items-center justify-between gap-2 text-[11px] font-medium uppercase tracking-wide text-white/40">
                                                        <span>{group.label}</span>
                                                        <span className="rounded border border-white/10 px-1.5 py-0.5 font-mono normal-case tracking-normal text-white/50">
                                                            {group.space}{group.absolute ? ' (absolute)' : ''}
                                                        </span>
                                                    </div>
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
                                        {relativeTransform.computed.state === 'available' ? (
                                            <div className="space-y-3 rounded-lg border border-primary/20 bg-primary/5 p-3">
                                                <div className="flex items-center justify-between text-[11px] text-text-low">
                                                    <span>Computed actor world transform via reflected getters</span>
                                                    <span className="rounded border border-primary/25 px-2 py-0.5 font-mono text-primary">
                                                        {relativeTransform.computed.precision}
                                                    </span>
                                                </div>
                                                <div className="grid gap-3 xl:grid-cols-3">
                                                    {[
                                                        {
                                                            label: 'Location',
                                                            values: [['X', relativeTransform.computed.location.x], ['Y', relativeTransform.computed.location.y], ['Z', relativeTransform.computed.location.z]],
                                                        },
                                                        {
                                                            label: 'Rotation',
                                                            values: [['Pitch', relativeTransform.computed.rotation.pitch], ['Yaw', relativeTransform.computed.rotation.yaw], ['Roll', relativeTransform.computed.rotation.roll]],
                                                        },
                                                        {
                                                            label: 'Scale',
                                                            values: [['X', relativeTransform.computed.scale.x], ['Y', relativeTransform.computed.scale.y], ['Z', relativeTransform.computed.scale.z]],
                                                        },
                                                    ].map((group) => (
                                                        <div key={group.label} className="rounded-lg border border-white/5 bg-black/20 p-3">
                                                            <div className="mb-2 text-[11px] font-medium uppercase tracking-wide text-white/40">
                                                                {group.label}
                                                            </div>
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
                                            </div>
                                        ) : (
                                            <div className="rounded-lg border border-amber-500/20 bg-amber-500/10 px-3 py-2 text-xs text-amber-200">
                                                <span className="mr-2 font-mono text-amber-300/70">{relativeTransform.computed.reason_code}</span>
                                                {relativeTransform.computed.reason}
                                            </div>
                                        )}
                                        <div className="rounded-xl border border-primary/20 bg-background-base/60 p-4">
                                            <div className="mb-4 flex items-start justify-between gap-4">
                                                <div>
                                                    <div className="text-sm font-semibold text-text-high">Edit transform</div>
                                                    <div className="mt-1 text-[11px] text-text-low">
                                                        Executes one reflected setter and reloads the live transform.
                                                    </div>
                                                </div>
                                                <span className="rounded-full border border-primary/25 bg-primary/10 px-2 py-1 text-[10px] font-semibold text-primary">
                                                    GAME THREAD
                                                </span>
                                            </div>

                                            <div className="grid gap-3 md:grid-cols-2">
                                                <label className="space-y-1.5">
                                                    <span className="text-[10px] font-semibold uppercase tracking-wider text-text-low">Field</span>
                                                    <select
                                                        value={transformEditor.field}
                                                        onChange={(event) => selectTransformEditor(
                                                            event.target.value as TransformEditorField,
                                                            transformEditor.space,
                                                        )}
                                                        className="h-9 w-full rounded-lg border border-border-subtle bg-background-base px-3 text-xs text-text-high outline-none transition-colors focus:border-primary"
                                                    >
                                                        <option value="location">Location</option>
                                                        <option value="rotation">Rotation</option>
                                                        <option value="scale">Scale</option>
                                                    </select>
                                                </label>
                                                <label className="space-y-1.5">
                                                    <span className="text-[10px] font-semibold uppercase tracking-wider text-text-low">Space</span>
                                                    <select
                                                        value={transformEditor.space}
                                                        onChange={(event) => selectTransformEditor(
                                                            transformEditor.field,
                                                            event.target.value as WorldTransformSpace,
                                                        )}
                                                        className="h-9 w-full rounded-lg border border-border-subtle bg-background-base px-3 text-xs text-text-high outline-none transition-colors focus:border-primary"
                                                    >
                                                        <option value="world">World</option>
                                                        <option value="relative">Relative</option>
                                                    </select>
                                                </label>
                                            </div>

                                            <div className="mt-3 grid grid-cols-3 gap-2">
                                                {editorAxes.map((axis, index) => (
                                                    <label key={axis} className="space-y-1.5">
                                                        <span className="text-[10px] font-semibold uppercase tracking-wider text-text-low">{axis}</span>
                                                        <input
                                                            type="text"
                                                            inputMode="decimal"
                                                            value={transformEditor.components[index]}
                                                            onChange={(event) => setTransformEditor((current) => {
                                                                const components: [string, string, string] = [...current.components];
                                                                components[index] = event.target.value;
                                                                return { ...current, components };
                                                            })}
                                                            className="h-9 w-full rounded-lg border border-border-subtle bg-background-base px-3 font-mono text-xs text-text-high outline-none transition-colors placeholder:text-text-low/50 focus:border-primary"
                                                        />
                                                    </label>
                                                ))}
                                            </div>

                                            <div className="mt-3 flex min-h-8 flex-wrap items-center gap-4 text-xs text-text-mid">
                                                {usesSweepOptions && (
                                                    <>
                                                        <label className="flex cursor-pointer items-center gap-2">
                                                            <input
                                                                type="checkbox"
                                                                checked={transformEditor.sweep}
                                                                onChange={(event) => setTransformEditor((current) => ({
                                                                    ...current,
                                                                    sweep: event.target.checked,
                                                                }))}
                                                                className="accent-primary"
                                                            />
                                                            Sweep
                                                        </label>
                                                        <label className="flex cursor-pointer items-center gap-2">
                                                            <input
                                                                type="checkbox"
                                                                checked={transformEditor.teleport}
                                                                onChange={(event) => setTransformEditor((current) => ({
                                                                    ...current,
                                                                    teleport: event.target.checked,
                                                                }))}
                                                                className="accent-primary"
                                                            />
                                                            Teleport
                                                        </label>
                                                    </>
                                                )}
                                                {transformEditor.field === 'rotation' && transformEditor.space === 'world' && (
                                                    <label className="flex cursor-pointer items-center gap-2">
                                                        <input
                                                            type="checkbox"
                                                            checked={transformEditor.teleportPhysics}
                                                            onChange={(event) => setTransformEditor((current) => ({
                                                                ...current,
                                                                teleportPhysics: event.target.checked,
                                                            }))}
                                                            className="accent-primary"
                                                        />
                                                        Teleport physics
                                                    </label>
                                                )}
                                            </div>

                                            <div className="mt-4 flex flex-wrap items-center gap-2">
                                                <button
                                                    type="button"
                                                    onClick={reloadTransformEditor}
                                                    disabled={mutationBusy}
                                                    className="inline-flex h-9 items-center gap-2 rounded-lg border border-border-subtle bg-surface-dark px-3 text-xs font-semibold text-text-mid transition-colors hover:border-border-default hover:text-text-high disabled:opacity-40"
                                                >
                                                    <RefreshCw className="h-3.5 w-3.5" />
                                                    Load current
                                                </button>
                                                <button
                                                    type="button"
                                                    onClick={() => void applyTransformMutation()}
                                                    disabled={mutationBusy}
                                                    className="inline-flex h-9 items-center gap-2 rounded-lg bg-primary px-4 text-xs font-semibold text-white transition-all hover:bg-primary/90 active:scale-[0.98] disabled:opacity-40"
                                                >
                                                    <Save className="h-3.5 w-3.5" />
                                                    {mutationBusy ? 'Applying...' : 'Apply transform'}
                                                </button>
                                            </div>

                                            {mutationFeedback && (
                                                <div className={`mt-3 rounded-lg border px-3 py-2 font-mono text-[11px] ${mutationFeedback.kind === 'success'
                                                    ? 'border-emerald-500/20 bg-emerald-500/10 text-emerald-200'
                                                    : 'border-red-500/20 bg-red-500/10 text-red-200'
                                                    }`}>
                                                    {mutationFeedback.message}
                                                </div>
                                            )}
                                        </div>
                                        <div className="text-[11px] text-white/30">
                                            One game-thread work brackets the three reflected Actor getters with the stored RootComponent witness. Space labels apply bAbsoluteLocation/Rotation/Scale; stored and computed values remain explicitly separate.
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
                                            onClick={() => onSwitchMode?.('instances', { className: comp.class, objectIndex: comp.index })}>
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
