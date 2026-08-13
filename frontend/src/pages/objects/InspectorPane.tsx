import { useEffect, useState } from 'react';
import { MoreHorizontal } from 'lucide-react';
import { t } from '../../i18n';
import api from '../../services';
import type { ClassCDOResponse, ClassFunction, ClassProperty, ClassHierarchy, EnumValue, ObjectDetail, ObjectProperty, TypeQueryCursor } from '../../contracts';
import { toEditable } from '../../features/value/valueParser';
import { formatAddress } from '../../features/address/address';
import { PropertyValueEditor } from '../../features/shared/PropertyValueEditor';
import { DomainError } from '../../features/shared/DomainError';
import { LoadMoreButton } from '../../features/shared/Pagination';

interface InspectorPaneProps {
    selectedClass: string | null;
    selectedType: 'Class' | 'Struct' | 'Enum' | 'Package';
    selectedIndex: number | null;
}

type TabType = 'Properties' | 'Fields' | 'Functions' | 'CDO' | 'Values';

export default function InspectorPane({ selectedClass, selectedType, selectedIndex }: InspectorPaneProps) {
    // Context state
    const isInstanceMode = selectedIndex !== null;

    const [loading, setLoading] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [activeTab, setActiveTab] = useState<TabType>('Properties');

    // Instance State
    const [instanceDetail, setInstanceDetail] = useState<ObjectDetail | null>(null);
    const [properties, setProperties] = useState<ObjectProperty[]>([]);
    const [propertyEditMap, setPropertyEditMap] = useState<Record<string, string>>({});
    const [propertyRefreshing, setPropertyRefreshing] = useState<Record<string, boolean>>({});

    const [classFields, setClassFields] = useState<ClassProperty[]>([]);
    const [classFunctions, setClassFunctions] = useState<ClassFunction[]>([]);
    const [classFullName, setClassFullName] = useState<string>('');
    const [hierarchy, setHierarchy] = useState<ClassHierarchy | null>(null);
    const [cdoMetadata, setCdoMetadata] = useState<ClassCDOResponse | null>(null);
    const [enumValues, setEnumValues] = useState<EnumValue[]>([]);
    const [fieldCursor, setFieldCursor] = useState<TypeQueryCursor | null>(null);
    const [fieldHasMore, setFieldHasMore] = useState(false);
    const [functionCursor, setFunctionCursor] = useState<TypeQueryCursor | null>(null);
    const [functionHasMore, setFunctionHasMore] = useState(false);
    const [enumCursor, setEnumCursor] = useState<TypeQueryCursor | null>(null);
    const [enumHasMore, setEnumHasMore] = useState(false);

    const availableTabs: TabType[] = isInstanceMode
        ? ['Properties']
        : selectedType === 'Class'
            ? ['Fields', 'Functions', 'CDO']
            : selectedType === 'Struct'
                ? ['Fields']
                : selectedType === 'Enum'
                    ? ['Values']
                    : [];

    useEffect(() => {
        if (!isInstanceMode && !selectedClass) return;

        // Switch to valid tab automatically
        if (isInstanceMode) setActiveTab('Properties');
        else if (!isInstanceMode) {
            if (selectedType === 'Enum') setActiveTab('Values');
            else if (selectedType === 'Struct' || selectedType === 'Class') setActiveTab('Fields');
        }

        const loadData = async () => {
            setLoading(true);
            setError(null);
            setClassFields([]);
            setClassFunctions([]);
            setHierarchy(null);
            setCdoMetadata(null);
            setEnumValues([]);
            setFieldCursor(null);
            setFieldHasMore(false);
            setFunctionCursor(null);
            setFunctionHasMore(false);
            setEnumCursor(null);
            setEnumHasMore(false);
            try {
                if (isInstanceMode) {
                    const [detailRes, propsRes] = await Promise.all([
                        api.getObjectByIndex(selectedIndex),
                        api.getObjectProperties(selectedIndex),
                    ]);
                    if (detailRes.success && detailRes.data) setInstanceDetail(detailRes.data);
                    if (propsRes.success && propsRes.data) {
                        setProperties(propsRes.data);
                        const editMap: Record<string, string> = {};
                        propsRes.data.forEach((p) => { editMap[p.name] = toEditable(p.value); });
                        setPropertyEditMap(editMap);
                    }
                } else if (selectedClass) {
                    if (selectedType === 'Class') {
                        const [fieldRes, funcRes, classRes, hierRes, cdoRes] = await Promise.all([
                            api.getClassFields(selectedClass),
                            api.getClassFunctions(selectedClass),
                            api.getClassByPath(selectedClass),
                            api.getClassHierarchy(selectedClass),
                            api.getClassCDO(selectedClass),
                        ]);
                        const failures: string[] = [];
                        if (fieldRes.success && fieldRes.data) {
                            setClassFields(fieldRes.data.items);
                            setFieldCursor(fieldRes.data.next_cursor);
                            setFieldHasMore(fieldRes.data.has_more);
                        } else failures.push(fieldRes.error || 'Class fields failed');
                        if (funcRes.success && funcRes.data) {
                            setClassFunctions(funcRes.data.items);
                            setFunctionCursor(funcRes.data.next_cursor);
                            setFunctionHasMore(funcRes.data.has_more);
                        } else failures.push(funcRes.error || 'Class functions failed');
                        if (classRes.success && classRes.data) setClassFullName(classRes.data.full_path);
                        else failures.push(classRes.error || 'Class detail failed');
                        if (hierRes.success && hierRes.data) setHierarchy(hierRes.data);
                        else failures.push(hierRes.error || 'Class hierarchy failed');
                        if (cdoRes.success && cdoRes.data) setCdoMetadata(cdoRes.data);
                        else failures.push(cdoRes.error || 'Class default object failed');
                        if (failures.length > 0) throw new Error(failures.join(' | '));
                    } else if (selectedType === 'Struct') {
                        const [detailRes, fieldsRes] = await Promise.all([
                            api.getStructByPath(selectedClass),
                            api.getStructFields(selectedClass),
                        ]);
                        if (!detailRes.success || !detailRes.data) throw new Error(detailRes.error || 'Struct detail failed');
                        if (!fieldsRes.success || !fieldsRes.data) throw new Error(fieldsRes.error || 'Struct fields failed');
                        setClassFields(fieldsRes.data.items);
                        setFieldCursor(fieldsRes.data.next_cursor);
                        setFieldHasMore(fieldsRes.data.has_more);
                        setClassFullName(detailRes.data.full_path);
                    } else if (selectedType === 'Enum') {
                        const [detailRes, valuesRes] = await Promise.all([
                            api.getEnumByPath(selectedClass),
                            api.getEnumValues(selectedClass),
                        ]);
                        if (!detailRes.success || !detailRes.data) throw new Error(detailRes.error || 'Enum detail failed');
                        if (!valuesRes.success || !valuesRes.data) throw new Error(valuesRes.error || 'Enum values failed');
                        setClassFullName(detailRes.data.full_path);
                        setEnumValues(valuesRes.data.items);
                        setEnumCursor(valuesRes.data.next_cursor);
                        setEnumHasMore(valuesRes.data.has_more);
                    }
                }
            } catch (err) {
                setError(err instanceof Error ? err.message : String(err));
            } finally {
                setLoading(false);
            }
        };

        void loadData();
    }, [isInstanceMode, selectedClass, selectedIndex, selectedType]);

    const loadMoreMetadata = async (kind: 'fields' | 'functions' | 'values') => {
        if (!selectedClass || loading) return;
        setLoading(true);
        setError(null);
        try {
            if (kind === 'fields' && fieldCursor) {
                const response = selectedType === 'Struct'
                    ? await api.getStructFields(selectedClass, fieldCursor)
                    : await api.getClassFields(selectedClass, fieldCursor);
                if (!response.success || !response.data) throw new Error(response.error || 'Field continuation failed');
                setClassFields((current) => [...current, ...response.data!.items]);
                setFieldCursor(response.data.next_cursor);
                setFieldHasMore(response.data.has_more);
            } else if (kind === 'functions' && functionCursor) {
                const response = await api.getClassFunctions(selectedClass, functionCursor);
                if (!response.success || !response.data) throw new Error(response.error || 'Function continuation failed');
                setClassFunctions((current) => [...current, ...response.data!.items]);
                setFunctionCursor(response.data.next_cursor);
                setFunctionHasMore(response.data.has_more);
            } else if (kind === 'values' && enumCursor) {
                const response = await api.getEnumValues(selectedClass, enumCursor);
                if (!response.success || !response.data) throw new Error(response.error || 'Enum continuation failed');
                setEnumValues((current) => [...current, ...response.data!.items]);
                setEnumCursor(response.data.next_cursor);
                setEnumHasMore(response.data.has_more);
            }
        } catch (metadataError) {
            setError(metadataError instanceof Error ? metadataError.message : String(metadataError));
        } finally {
            setLoading(false);
        }
    };

    const handleCopyAddress = () => {
        const addr = instanceDetail?.address;
        if (addr) void navigator.clipboard.writeText(`0x${addr}`);
    };

    const handleWatchObject = async () => {
        if (!isInstanceMode || selectedIndex === null || properties.length === 0) return;
        const response = await api.addWatch(properties[0]);
        if (!response.success) setError(response.error || 'Watch creation failed');
    };

    const handlePropertyRefresh = async (property: ObjectProperty) => {
        if (!isInstanceMode || selectedIndex === null) return;
        setPropertyRefreshing((prev) => ({ ...prev, [property.name]: true }));
        const res = await api.getObjectPropertyValue(property);
        if (res.success && res.data) {
            setPropertyEditMap((prev) => ({ ...prev, [property.name]: toEditable(res.data!.value) }));
        }
        setPropertyRefreshing((prev) => ({ ...prev, [property.name]: false }));
    };

    if (!selectedClass && !isInstanceMode) {
        return (
            <aside className="w-[400px] flex flex-col bg-surface-dark/30 backdrop-blur-sm shrink-0 border-l border-border-subtle">
                <div className="flex-1 flex flex-col items-center justify-center p-8 text-center text-text-low">
                    <span className="text-sm font-display mb-2">{t('No Item Selected')}</span>
                    <span className="text-xs">{t('Select a class or instance to view...')}</span>
                </div>
            </aside>
        )
    }

    return (
        <aside className="w-[400px] flex flex-col bg-surface-dark/30 backdrop-blur-sm shrink-0 border-l border-border-subtle">
            {/* Header */}
            <div className="p-4 border-b border-border-subtle bg-surface-dark/50 flex-none">
                <div className="flex items-start justify-between">
                    <div>
                        <h2 className="text-sm font-bold text-white font-display mb-1 truncate max-w-[300px]" title={instanceDetail?.name || selectedClass || ''}>
                            {isInstanceMode ? instanceDetail?.name : selectedClass}
                        </h2>
                        <div className="flex items-center gap-2 mt-1">
                            <span className="px-1.5 py-0.5 rounded bg-blue-500/10 text-blue-400 text-2xs font-mono border border-blue-500/20 truncate max-w-[200px]" title={isInstanceMode ? instanceDetail?.class : classFullName}>
                                {isInstanceMode ? instanceDetail?.class : classFullName || 'Type'}
                            </span>
                            {isInstanceMode && instanceDetail?.address && (
                                <span className="text-2xs text-text-low font-mono">{formatAddress(instanceDetail.address)}</span>
                            )}
                            {!isInstanceMode && (
                                <span className="text-2xs text-text-low font-mono">{t('Definition')}</span>
                            )}
                        </div>
                    </div>
                    <button className="text-text-low hover:text-white transition-colors">
                        <MoreHorizontal className="w-4.5 h-4.5" />
                    </button>
                </div>

                {/* Tabs */}
                <div className="flex bg-background-base rounded p-1 mt-4 border border-border-subtle">
                    {availableTabs.map((tab) => (
                        <button
                            key={tab}
                            onClick={() => setActiveTab(tab)}
                            className={`flex-1 py-1 px-2 text-xs font-medium rounded text-center font-display transition-colors ${activeTab === tab
                                ? 'text-white bg-primary shadow-sm'
                                : 'text-text-mid hover:text-text-high'
                                }`}
                        >
                            {t(tab)}
                        </button>
                    ))}
                </div>
            </div>

            {/* Content Area */}
            <div className="flex-1 overflow-y-auto relative">
                {loading && (
                    <div className="absolute inset-0 bg-background-base/50 flex flex-col items-center justify-center text-text-low text-xs z-10">
                        <span>{t('Loading structure...')}</span>
                    </div>
                )}
                <div className="m-4"><DomainError message={error} compact /></div>

                {/* Tab: Properties (Instance Mode) */}
                {activeTab === 'Properties' && isInstanceMode && (
                    <div className="flex flex-col">
                        {properties.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No properties found.')}</div>
                        )}
                        {properties.map((p, index) => {
                            return (
                                <div key={p.name} className={`flex items-center border-b border-border-subtle px-3 py-2 hover:bg-white/5 group/row relative ${index % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                    <div className="w-[45%] pr-2 flex items-center gap-2">
                                        <span className="w-1 h-1 rounded-full bg-transparent group-hover/row:bg-text-low"></span>
                                        <span className="text-xs text-text-mid font-mono truncate max-w-[120px]" title={p.name}>{p.name}</span>
                                    </div>
                                    <div className="w-[55%] relative flex items-center justify-between group/input">
                                        <div className="text-2xs text-text-low px-1 mr-2 border border-border-subtle rounded font-mono truncate max-w-[70px]" title={p.type}>{p.type}</div>

                                        <PropertyValueEditor
                                            value={propertyEditMap[p.name] ?? ''}
                                            refreshing={propertyRefreshing[p.name] === true}
                                            onRefresh={() => void handlePropertyRefresh(p)}
                                        />
                                    </div>
                                </div>
                            );
                        })}
                    </div>
                )}

                {/* Hierarchy breadcrumb */}
                {!isInstanceMode && hierarchy && hierarchy.parents.length > 0 && (
                    <div className="px-3 py-2 border-b border-border-subtle bg-surface-stripe/30 flex items-center gap-1 text-[10px] text-text-low font-mono overflow-x-auto flex-none">
                        <span className="text-text-mid font-display font-bold mr-1">{t('Inheritance:')}</span>
                        {hierarchy.parents.map((p, i) => (
                            <span key={p.full_path}>{i > 0 && <span className="text-text-low mx-0.5">&rarr;</span>}{p.full_path}</span>
                        ))}
                        <span className="text-text-low mx-0.5">&rarr;</span>
                        <span className="text-primary font-bold">{hierarchy.path}</span>
                    </div>
                )}

                {activeTab === 'Fields' && !isInstanceMode && (
                    <div className="flex flex-col">
                        {classFields.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No fields found.')}</div>
                        )}
                        {classFields.map((f, i) => (
                            <div key={`${f.declaring_type.full_path}:${f.name}:${f.offset}`} className={`flex items-center border-b border-border-subtle px-3 py-2 hover:bg-white/5 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="w-[15%] text-text-low font-mono text-[10px]">+0x{f.offset.toString(16).toUpperCase().padStart(4, '0')}</div>
                                <div className="w-[45%] text-text-mid font-mono text-xs truncate pr-2" title={f.name}>{f.name}</div>
                                <div className="w-[40%] text-text-low px-1 py-0.5 border border-border-subtle rounded font-mono text-[10px] truncate" title={f.reason || f.type_name}>
                                    {f.type_name}{f.state !== 'supported' ? ` (${f.state})` : ''}
                                </div>
                            </div>
                        ))}
                        <LoadMoreButton visible={fieldHasMore && fieldCursor !== null} loading={loading} onClick={() => void loadMoreMetadata('fields')} label={t('Load more')} />
                    </div>
                )}

                {/* Tab: Functions (Class Mode) */}
                {activeTab === 'Functions' && !isInstanceMode && (
                    <div className="flex flex-col">
                        {classFunctions.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No functions found.')}</div>
                        )}
                        {classFunctions.map((f, i) => (
                            <div key={f.full_path} className={`flex flex-col border-b border-border-subtle px-3 py-2 hover:bg-white/5 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="flex items-center gap-2">
                                    <span className="text-primary text-xs font-mono truncate flex-1" title={f.name}>{f.name}()</span>
                                    <span className="text-[10px] text-text-low font-mono">{f.implementation}</span>
                                </div>
                                {f.flags && <div className="text-[10px] text-text-low font-mono mt-1 opacity-60">{t('Flags')}: {f.flags}</div>}
                                {f.reason && <div className="text-[10px] text-yellow-400 font-mono mt-1">{f.reason_code}: {f.reason}</div>}
                            </div>
                        ))}
                        <LoadMoreButton visible={functionHasMore && functionCursor !== null} loading={loading} onClick={() => void loadMoreMetadata('functions')} label={t('Load more')} />
                    </div>
                )}

                {activeTab === 'CDO' && !isInstanceMode && selectedType === 'Class' && (
                    <div className="flex flex-col">
                        <div className="px-3 py-2 border-b border-border-subtle bg-surface-stripe/20 text-[10px] text-text-low font-display">
                            {t('CDO Readonly Hint')}
                        </div>
                        {!cdoMetadata && !loading && <div className="p-4 text-center text-text-low text-xs">{t('No metadata')}</div>}
                        {cdoMetadata && (
                            <div className="p-4 space-y-2 text-xs font-mono">
                                <div className="text-text-mid">State: <span className="text-primary">{cdoMetadata.state}</span></div>
                                <div className="text-text-mid break-all">Address: <span className="text-text-high">{cdoMetadata.handle?.address || 'unavailable'}</span></div>
                                {cdoMetadata.reason && <div className="text-yellow-400">{cdoMetadata.reason_code}: {cdoMetadata.reason}</div>}
                                <div className="text-text-low">Property values are not fabricated before the property codec command is available.</div>
                            </div>
                        )}
                    </div>
                )}

                {/* Tab: Values (Enum Mode) */}
                {activeTab === 'Values' && !isInstanceMode && selectedType === 'Enum' && (
                    <div className="flex flex-col">
                        {enumValues.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No values found.')}</div>
                        )}
                        {enumValues.map((v, i) => (
                            <div key={v.name} className={`flex items-center justify-between border-b border-border-subtle px-3 py-2 hover:bg-white/5 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="text-xs text-text-mid font-mono truncate flex-1 pr-2" title={v.name}>{v.name}</div>
                                <div className="text-xs text-primary font-mono font-medium">{v.value}</div>
                            </div>
                        ))}
                        <LoadMoreButton visible={enumHasMore && enumCursor !== null} loading={loading} onClick={() => void loadMoreMetadata('values')} label={t('Load more')} />
                    </div>
                )}
            </div>

            {/* Quick Actions Footer */}
            <div className="p-3 border-t border-border-subtle bg-surface-dark flex gap-2 flex-none">
                {isInstanceMode && (
                    <button onClick={handleCopyAddress} className="flex-1 py-1.5 bg-surface-stripe hover:bg-white/10 border border-border-subtle rounded text-xs text-text-mid font-medium transition-colors">
                        {t('Copy Address')}
                    </button>
                )}
                <button onClick={isInstanceMode ? () => void handleWatchObject() : undefined} className="flex-1 py-1.5 bg-surface-stripe hover:bg-white/10 border border-border-subtle rounded text-xs text-text-mid font-medium transition-colors">
                    {isInstanceMode ? t('Watch Object') : t('Find References')}
                </button>
            </div>
        </aside>
    );
}
