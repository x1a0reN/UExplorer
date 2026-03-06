import { useEffect, useState, useRef } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import { ArrowDown } from 'lucide-react';
import { t } from '../../i18n';
import api from '../../api';

interface InstanceItem {
    index: number;
    name: string;
    className: string;
    outerName?: string;
    address?: string;
}

interface InstancePaneProps {
    selectedClass: string | null;
    onSelectInstance: (idx: number) => void;
}

export default function InstancePane({ selectedClass, onSelectInstance }: InstancePaneProps) {
    const [search, setSearch] = useState('');
    const [items, setItems] = useState<InstanceItem[]>([]);
    const [total, setTotal] = useState(0);
    const [listLoading, setListLoading] = useState(false);
    const [selectedIndexState, setSelectedIndexState] = useState<number | null>(null);

    const PAGE_SIZE = 500;

    const loadList = async (append = false) => {
        if (!selectedClass) return; // Wait until a class is selected
        setListLoading(true);
        const offset = append ? items.length : 0;
        try {
            const res = await api.getClassInstances(selectedClass, offset, PAGE_SIZE);
            if (res.success && res.data) {
                // Return data format mapping
                const mapped = res.data.items.map((i: any) => ({
                    index: i.index,
                    name: i.name,
                    className: selectedClass, // Passed down from selection
                    outerName: i.outer_name || 'Package',
                    address: i.address
                }));
                setItems(append ? [...items, ...mapped] : mapped);
                setTotal(res.data.matched);
            }
        } catch (error) {
            console.error("Failed to load instances", error);
        } finally {
            setListLoading(false);
        }
    };

    // Reload list when class selection or search changes
    useEffect(() => {
        setItems([]);
        const timer = setTimeout(() => {
            void loadList(false);
        }, 50); // slight debounce
        return () => clearTimeout(timer);
    }, [search, selectedClass]);

    // ─── Virtualization ─────────────────────────────────────────

    const parentRef = useRef<HTMLDivElement>(null);

    const rowVirtualizer = useVirtualizer({
        count: items.length + (items.length < total && !listLoading ? 1 : 0),
        getScrollElement: () => parentRef.current,
        estimateSize: () => 28, // Compact row height from Demo
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

    return (
        <section className="flex-1 flex flex-col min-w-[300px] bg-background-base shrink-0 border-r border-border-subtle">
            {/* Toolbar */}
            <div className="h-8 flex flex-none items-center px-2 border-b border-border-subtle bg-background-base">
                <input
                    type="text"
                    value={search}
                    onChange={(e) => setSearch(e.target.value)}
                    placeholder={t('Search Instances...')}
                    className="w-full max-w-[200px] bg-[#121212] border border-border-subtle rounded px-2 py-0.5 text-xs text-text-high font-mono focus:outline-none focus:border-primary placeholder-text-low"
                />
            </div>

            {/* Table Headers */}
            <div className="h-6 flex-none flex items-center px-0 border-b border-border-subtle bg-surface-dark">
                <div className="grid grid-cols-12 w-full px-2 gap-2">
                    <div className="col-span-3 text-[10px] font-bold text-text-mid uppercase tracking-wider font-display flex items-center gap-1 cursor-pointer hover:text-text-high">
                        {t('Address')}
                        <ArrowDown className="w-3 h-3 text-text-low" />
                    </div>
                    <div className="col-span-1 text-[10px] font-bold text-text-mid uppercase tracking-wider font-display border-l border-border-subtle pl-2">{t('#')}</div>
                    <div className="col-span-4 text-[10px] font-bold text-text-mid uppercase tracking-wider font-display border-l border-border-subtle pl-2">{t('Name')}</div>
                    <div className="col-span-4 text-[10px] font-bold text-text-mid uppercase tracking-wider font-display border-l border-border-subtle pl-2">{t('Outer')}</div>
                </div>
            </div>

            {/* Virtualized Grid List */}
            <div ref={parentRef} className="flex-1 overflow-y-auto font-mono text-xs relative bg-background-base">
                {!selectedClass && items.length === 0 && !listLoading && (
                    <div className="absolute inset-0 flex items-center justify-center text-text-low text-xs p-4 text-center">
                        {t('Select a class on the left.')}
                    </div>
                )}
                {selectedClass && items.length === 0 && !listLoading && (
                    <div className="absolute inset-0 flex items-center justify-center text-text-low text-xs p-4 text-center">
                        {t('No instances found for')} {selectedClass}
                    </div>
                )}
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
                                    {t('Loading...')}
                                </div>
                            );
                        }

                        const item = items[virtualRow.index];
                        const isSelected = selectedIndexState === item.index;
                        const isStripe = virtualRow.index % 2 !== 0;

                        return (
                            <div
                                key={virtualRow.key}
                                onClick={() => {
                                    setSelectedIndexState(item.index);
                                    onSelectInstance(item.index);
                                }}
                                style={{
                                    position: 'absolute',
                                    top: 0,
                                    left: 0,
                                    width: '100%',
                                    height: `${virtualRow.size}px`,
                                    transform: `translateY(${virtualRow.start}px)`,
                                }}
                                className={`grid grid-cols-12 w-full px-2 py-1 gap-2 border-b border-[#2a2a2c] cursor-pointer items-center group transition-colors
                                  ${isSelected
                                        ? 'bg-primary-dim border-l-2 border-l-primary hover:bg-primary-dim/80'
                                        : isStripe
                                            ? 'bg-surface-stripe hover:bg-white/5 border-l-2 border-l-transparent'
                                            : 'bg-transparent hover:bg-white/5 border-l-2 border-l-transparent'
                                    }`}
                            >
                                <div className={`col-span-3 truncate ${isSelected ? 'text-primary font-bold' : 'text-text-low group-hover:text-text-mid'}`}>
                                    {item.address ? `0x${item.address}` : '0x0000000'}
                                </div>
                                <div className={`col-span-1 truncate opacity-50 ${isSelected ? 'text-white' : 'text-text-low'}`}>
                                    {item.index}
                                </div>
                                <div className={`col-span-4 truncate ${isSelected ? 'text-white font-medium' : 'text-text-mid group-hover:text-text-high'}`}>
                                    {item.name}
                                </div>
                                <div className={`col-span-4 truncate ${isSelected ? 'text-white/80' : 'text-text-low group-hover:text-text-mid'}`}>
                                    {item.outerName}
                                </div>
                            </div>
                        );
                    })}
                </div>
            </div>

            {/* Status Footer */}
            <div className="h-6 flex-none border-t border-border-subtle bg-surface-dark flex items-center px-2">
                <span className="text-2xs font-mono text-text-low">
                    {selectedIndexState !== null
                        ? `${t('Selected: Index')} ${selectedIndexState}`
                        : `${total.toLocaleString()} ${t('Instances')}`}
                </span>
            </div>
        </section>
    );
}
