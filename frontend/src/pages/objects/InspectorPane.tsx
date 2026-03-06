import { useEffect, useState } from 'react';
import { MoreHorizontal, Save, RefreshCw } from 'lucide-react';
import { t } from '../../i18n';
import api from '../../api';
import type { ClassFunction, ClassProperty, ClassHierarchy, ObjectDetail, ObjectProperty } from '../../api';

interface InspectorPaneProps {
    selectedClass: string | null;
    selectedIndex: number | null;
}

type TabType = 'Properties' | 'Fields' | 'Functions' | 'CDO';

// Helper to reliably convert various value types to string for inputs
function toEditable(val: unknown): string {
    if (val === null || val === undefined) return '';
    if (typeof val === 'string') return val;
    if (typeof val === 'number') return val.toString();
    if (typeof val === 'boolean') return val ? 'true' : 'false';
    if (typeof val === 'object') return JSON.stringify(val);
    return String(val);
}

// Convert string back to proper typed value
function parseInputValue(valStr: string): any {
    if (valStr.toLowerCase() === 'true') return true;
    if (valStr.toLowerCase() === 'false') return false;
    const num = Number(valStr);
    if (!isNaN(num) && valStr.trim() !== '') return num;
    try {
        return JSON.parse(valStr);
    } catch {
        return valStr;
    }
}

export default function InspectorPane({ selectedClass, selectedIndex }: InspectorPaneProps) {
    // Context state
    const isInstanceMode = selectedIndex !== null;

    const [loading, setLoading] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [activeTab, setActiveTab] = useState<TabType>('Properties');
    const [saveStatus, setSaveStatus] = useState<Record<string, 'ok' | 'err' | null>>({});

    // Instance State
    const [instanceDetail, setInstanceDetail] = useState<ObjectDetail | null>(null);
    const [properties, setProperties] = useState<ObjectProperty[]>([]);
    const [propertyEditMap, setPropertyEditMap] = useState<Record<string, string>>({});
    const [propertyRefreshing, setPropertyRefreshing] = useState<Record<string, boolean>>({});

    const [classFields, setClassFields] = useState<ClassProperty[]>([]);
    const [classFunctions, setClassFunctions] = useState<ClassFunction[]>([]);
    const [classFullName, setClassFullName] = useState<string>('');
    const [hierarchy, setHierarchy] = useState<ClassHierarchy | null>(null);
    const [cdoProperties, setCdoProperties] = useState<ObjectProperty[]>([]);

    const availableTabs: TabType[] = isInstanceMode
        ? ['Properties']
        : ['Fields', 'Functions', 'CDO'];

    useEffect(() => {
        if (!isInstanceMode && !selectedClass) return;

        // Switch to valid tab automatically
        if (isInstanceMode) setActiveTab('Properties');
        else if (!isInstanceMode && activeTab === 'Properties') setActiveTab('Fields');

        const loadData = async () => {
            setLoading(true);
            setError(null);
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
                    const [fieldRes, funcRes, classRes, hierRes, cdoRes] = await Promise.all([
                        api.getClassFields(selectedClass),
                        api.getClassFunctions(selectedClass),
                        api.getClassByName(selectedClass),
                        api.getClassHierarchy(selectedClass),
                        api.getClassCDO(selectedClass),
                    ]);
                    if (fieldRes.success && fieldRes.data) setClassFields(fieldRes.data);
                    if (funcRes.success && funcRes.data) setClassFunctions(funcRes.data);
                    if (classRes.success && classRes.data) setClassFullName(classRes.data.full_name);
                    if (hierRes.success && hierRes.data) setHierarchy(hierRes.data);
                    if (cdoRes.success && cdoRes.data) setCdoProperties(cdoRes.data.properties ?? []);
                }
            } catch (err) {
                setError(err instanceof Error ? err.message : String(err));
            } finally {
                setLoading(false);
            }
        };

        void loadData();
    }, [selectedClass, selectedIndex]);

    const handlePropertySave = async (propName: string) => {
        if (!isInstanceMode || selectedIndex === null) return;
        const raw = propertyEditMap[propName];
        const parsed = parseInputValue(raw);
        const res = await api.setObjectProperty(selectedIndex, propName, parsed);
        setSaveStatus((prev) => ({ ...prev, [propName]: res.success ? 'ok' : 'err' }));
        setTimeout(() => setSaveStatus((prev) => ({ ...prev, [propName]: null })), 2000);
    };

    const handleCopyAddress = () => {
        const addr = instanceDetail?.address;
        if (addr) void navigator.clipboard.writeText(`0x${addr}`);
    };

    const handleWatchObject = async () => {
        if (!isInstanceMode || selectedIndex === null || properties.length === 0) return;
        await api.addWatch(selectedIndex, properties[0].name);
    };

    const handlePropertyRefresh = async (propName: string) => {
        if (!isInstanceMode || selectedIndex === null) return;
        setPropertyRefreshing((prev) => ({ ...prev, [propName]: true }));
        const res = await api.getObjectPropertyValue(selectedIndex, propName);
        if (res.success && res.data) {
            setPropertyEditMap((prev) => ({ ...prev, [propName]: toEditable(res.data!.value) }));
        }
        setPropertyRefreshing((prev) => ({ ...prev, [propName]: false }));
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
                                <span className="text-2xs text-text-low font-mono">0x{instanceDetail.address}</span>
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
                {error && (
                    <div className="p-4 bg-red-500/10 border border-red-500/20 text-red-400 text-xs m-4 rounded font-mono">
                        {error}
                    </div>
                )}

                {/* Tab: Properties (Instance Mode) */}
                {activeTab === 'Properties' && isInstanceMode && (
                    <div className="flex flex-col">
                        {properties.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No properties found.')}</div>
                        )}
                        {properties.map((p, index) => {
                            const isBool = p.type === 'bool';
                            return (
                                <div key={p.name} className={`flex items-center border-b border-border-subtle px-3 py-2 hover:bg-white/5 group/row relative ${saveStatus[p.name] === 'ok' ? 'bg-accent-green/5' : saveStatus[p.name] === 'err' ? 'bg-accent-red/5' : index % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                    <div className="w-[45%] pr-2 flex items-center gap-2">
                                        <span className="w-1 h-1 rounded-full bg-transparent group-hover/row:bg-text-low"></span>
                                        <span className="text-xs text-text-mid font-mono truncate max-w-[120px]" title={p.name}>{p.name}</span>
                                    </div>
                                    <div className="w-[55%] relative flex items-center justify-between group/input">
                                        <div className="text-2xs text-text-low px-1 mr-2 border border-border-subtle rounded font-mono truncate max-w-[70px]" title={p.type}>{p.type}</div>

                                        {isBool ? (
                                            <div className="flex-1 flex items-center justify-start gap-2 h-7">
                                                <input
                                                    type="checkbox"
                                                    checked={propertyEditMap[p.name] === 'true'}
                                                    onChange={(e) => {
                                                        setPropertyEditMap(prev => ({ ...prev, [p.name]: e.target.checked ? 'true' : 'false' }));
                                                    }}
                                                    className="form-checkbox h-3.5 w-3.5 text-primary bg-[#0a0a0a] border-border-subtle rounded focus:ring-0"
                                                />
                                                <span className="text-xs text-text-low font-mono">{propertyEditMap[p.name] === 'true' ? t('True') : t('False')}</span>
                                            </div>
                                        ) : (
                                            <input
                                                type="text"
                                                value={propertyEditMap[p.name] ?? ''}
                                                onChange={(e) => setPropertyEditMap(prev => ({ ...prev, [p.name]: e.target.value }))}
                                                className="flex-1 w-full bg-[#0a0a0a] border border-border-subtle rounded px-2 py-1 text-xs text-white font-mono focus:border-primary focus:ring-1 focus:ring-primary shadow-inner"
                                            />
                                        )}

                                        {/* Action Floaters */}
                                        <div className="absolute right-0 top-1/2 -translate-y-1/2 translate-x-1 opacity-0 group-hover/row:opacity-100 flex items-center gap-0.5 bg-surface-dark border border-border-subtle rounded shadow-lg p-0.5 z-20 transition-opacity">
                                            <button onClick={() => void handlePropertySave(p.name)} className="p-1 hover:bg-emerald-500/20 text-text-low hover:text-emerald-400 rounded" title={t('Save')}>
                                                <Save className="w-3.5 h-3.5" />
                                            </button>
                                            <button onClick={() => void handlePropertyRefresh(p.name)} className={`p-1 hover:bg-blue-500/20 text-text-low hover:text-primary rounded ${propertyRefreshing[p.name] ? 'animate-spin text-primary' : ''}`} title={t('Refresh')}>
                                                <RefreshCw className="w-3.5 h-3.5" />
                                            </button>
                                        </div>
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
                            <span key={p}>{i > 0 && <span className="text-text-low mx-0.5">&rarr;</span>}{p}</span>
                        ))}
                        <span className="text-text-low mx-0.5">&rarr;</span>
                        <span className="text-primary font-bold">{hierarchy.name}</span>
                    </div>
                )}

                {activeTab === 'Fields' && !isInstanceMode && (
                    <div className="flex flex-col">
                        {classFields.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No fields found.')}</div>
                        )}
                        {classFields.map((f, i) => (
                            <div key={f.name} className={`flex items-center border-b border-border-subtle px-3 py-2 hover:bg-white/5 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="w-[15%] text-text-low font-mono text-[10px]">+0x{f.offset.toString(16).toUpperCase().padStart(4, '0')}</div>
                                <div className="w-[45%] text-text-mid font-mono text-xs truncate pr-2" title={f.name}>{f.name}</div>
                                <div className="w-[40%] text-text-low px-1 py-0.5 border border-border-subtle rounded font-mono text-[10px] truncate" title={f.type}>{f.type}</div>
                            </div>
                        ))}
                    </div>
                )}

                {/* Tab: Functions (Class Mode) */}
                {activeTab === 'Functions' && !isInstanceMode && (
                    <div className="flex flex-col">
                        {classFunctions.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No functions found.')}</div>
                        )}
                        {classFunctions.map((f, i) => (
                            <div key={f.name} className={`flex flex-col border-b border-border-subtle px-3 py-2 hover:bg-white/5 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="flex items-center gap-2">
                                    <span className="text-primary text-xs font-mono truncate flex-1" title={f.name}>{f.name}()</span>
                                </div>
                                {f.flags && <div className="text-[10px] text-text-low font-mono mt-1 opacity-60">{t('Flags')}: {f.flags}</div>}
                            </div>
                        ))}
                    </div>
                )}

                {activeTab === 'CDO' && !isInstanceMode && (
                    <div className="flex flex-col">
                        <div className="px-3 py-2 border-b border-border-subtle bg-surface-stripe/20 text-[10px] text-text-low font-display">
                            {t('CDO Readonly Hint')}
                        </div>
                        {cdoProperties.length === 0 && !loading && (
                            <div className="p-4 text-center text-text-low text-xs">{t('No properties')}</div>
                        )}
                        {cdoProperties.map((p, i) => (
                            <div key={p.name} className={`flex items-center border-b border-border-subtle px-3 py-2 ${i % 2 === 0 ? 'bg-transparent' : 'bg-surface-stripe'}`}>
                                <div className="w-[45%] pr-2 text-xs text-text-mid font-mono truncate" title={p.name}>{p.name}</div>
                                <div className="w-[55%] flex items-center gap-2">
                                    <span className="text-2xs text-text-low border border-border-subtle rounded px-1 font-mono truncate max-w-[70px]" title={p.type}>{p.type}</span>
                                    <span className="text-xs text-text-high font-mono truncate flex-1">{typeof p.value === 'object' ? JSON.stringify(p.value) : String(p.value ?? '')}</span>
                                </div>
                            </div>
                        ))}
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
