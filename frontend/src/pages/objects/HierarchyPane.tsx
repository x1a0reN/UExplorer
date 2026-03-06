import { useEffect, useState, useRef } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import { Box, Database, Layers, ChevronRight } from 'lucide-react';
import { t } from '../../i18n';
import api from '../../api';

type TypeSubTab = 'Class' | 'Struct' | 'Enum';

interface TypeItem {
    index: number;
    name: string;
    size?: number;
    super?: string;
    valueCount?: number;
}

interface HierarchyPaneProps {
    onSelectClass: (className: string) => void;
}

export default function HierarchyPane({ onSelectClass }: HierarchyPaneProps) {
    const [subTab, setSubTab] = useState<TypeSubTab>('Class');
    const [search, setSearch] = useState('');
    const [items, setItems] = useState<TypeItem[]>([]);
    const [total, setTotal] = useState(0);
    const [listLoading, setListLoading] = useState(false);
    const [selectedName, setSelectedName] = useState<string | null>(null);

    const PAGE_SIZE = 500;

    const loadList = async (append = false) => {
        setListLoading(true);
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
            console.error("Failed to load list", error);
        } finally {
            setListLoading(false);
        }
    };

    // Reload when tab or search changes
    useEffect(() => {
        setItems([]);
        void loadList(false);
    }, [subTab, search]);

    // ─── Virtualization ─────────────────────────────────────────
    const parentRef = useRef<HTMLDivElement>(null);

    const rowVirtualizer = useVirtualizer({
        count: items.length + (items.length < total && !listLoading ? 1 : 0),
        getScrollElement: () => parentRef.current,
        estimateSize: () => 26, // Crystal IDE compact size
        overscan: 20,
    });

    const virtualItems = rowVirtualizer.getVirtualItems();

    useEffect(() => {
        const lastItem = virtualItems[virtualItems.length - 1];
        if (!lastItem) return;

        if (lastItem.index >= items.length - 150 && !listLoading && items.length < total) {
            void loadList(true);
        }
    }, [virtualItems, items.length, listLoading, total]);

    // ─── Render ────────────────────────────────────────────────

    const SUB_TABS: { id: TypeSubTab; icon: typeof Layers; color: string }[] = [
        { id: 'Class', icon: Box, color: 'text-blue-400' },
        { id: 'Struct', icon: Database, color: 'text-orange-400' },
        { id: 'Enum', icon: Layers, color: 'text-yellow-400' },
    ];

    return (
        <aside className="w-[280px] flex flex-col border-r border-border-subtle bg-surface-dark/50 backdrop-blur-sm shrink-0">
            {/* Header */}
            <div className="h-8 flex flex-none items-center justify-between px-3 border-b border-border-subtle bg-background-base">
                <span className="text-xs font-bold text-text-mid uppercase tracking-wider font-display">{t('Hierarchy')}</span>
                <span className="text-2xs text-text-low font-mono">{total.toLocaleString()} Loaded</span>
            </div>

            {/* Sub-tabs & Search */}
            <div className="p-2 border-b border-border-subtle flex flex-col gap-2 flex-none">
                <div className="flex bg-background-base rounded border border-border-subtle">
                    {SUB_TABS.map((st) => (
                        <button
                            key={st.id}
                            onClick={() => { setSubTab(st.id); setSelectedName(null); onSelectClass(''); }}
                            className={`flex-1 py-1 px-1 text-[11px] font-medium rounded text-center font-display transition-colors ${subTab === st.id ? 'bg-primary text-white shadow-sm' : 'text-text-mid hover:text-text-high'}`}
                        >
                            {st.id}
                        </button>
                    ))}
                </div>
                <input
                    type="text"
                    value={search}
                    onChange={(e) => setSearch(e.target.value)}
                    placeholder={t('Filter...')}
                    className="w-full bg-[#121212] border border-border-subtle rounded px-2 py-1 text-xs text-text-high font-mono focus:outline-none focus:border-primary placeholder-text-low"
                />
            </div>

            {/* Virtualized List */}
            <div ref={parentRef} className="flex-1 overflow-y-auto p-1 relative">
                <div style={{ height: `${rowVirtualizer.getTotalSize()}px`, width: '100%', position: 'relative' }}>
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
                                    className="flex items-center justify-center text-text-low text-xs"
                                >
                                    Loading...
                                </div>
                            );
                        }

                        const item = items[virtualRow.index];
                        const isSelected = selectedName === item.name;
                        const activeTabColor = SUB_TABS.find(t => t.id === subTab)?.color || 'text-text-mid';

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
                                }}
                            >
                                <div
                                    onClick={() => { setSelectedName(item.name); onSelectClass(item.name); }}
                                    className={`group flex items-center gap-1 px-1 py-0.5 rounded cursor-pointer mx-1 ${isSelected ? 'bg-primary' : 'hover:bg-white/5'}`}
                                >
                                    <ChevronRight className={`w-3.5 h-3.5 flex-none ${isSelected ? 'text-white/60' : 'text-text-low group-hover:text-text-mid'}`} />
                                    {subTab === 'Class' && <Box className={`w-3.5 h-3.5 flex-none ${isSelected ? 'text-white' : activeTabColor}`} />}
                                    {subTab === 'Struct' && <Database className={`w-3.5 h-3.5 flex-none ${isSelected ? 'text-white' : activeTabColor}`} />}
                                    {subTab === 'Enum' && <Layers className={`w-3.5 h-3.5 flex-none ${isSelected ? 'text-white' : activeTabColor}`} />}

                                    <span className={`text-[12px] font-mono truncate ml-1 ${isSelected ? 'text-white font-medium' : 'text-text-mid group-hover:text-text-high'}`}>
                                        {item.name}
                                    </span>
                                    {item.size !== undefined && (
                                        <span className={`ml-auto text-[10px] font-mono flex-none ${isSelected ? 'text-white/80' : 'text-text-low'}`}>
                                            0x{item.size.toString(16).toUpperCase()}
                                        </span>
                                    )}
                                </div>
                            </div>
                        );
                    })}
                </div>
            </div>
        </aside>
    );
}
