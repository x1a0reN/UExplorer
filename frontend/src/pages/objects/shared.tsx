import { type ReactNode } from 'react';
import { ExternalLink } from 'lucide-react';
import type { Page } from '../../types';

// ─── Shared Types ─────────────────────────────────────────────

export type BrowseMode = 'types' | 'instances' | 'world';

export interface BrowserPageProps {
    onNavigate?: (page: Page) => void;
    onSwitchMode?: (mode: BrowseMode, context?: ModeNavContext) => void;
}

/** Context passed when switching between modes for cross-mode navigation */
export interface ModeNavContext {
    className?: string;
    objectIndex?: number;
    objectName?: string;
}

// ─── Utility Functions ─────────────────────────────────────────

export function parseInputValue(raw: string): unknown {
    const trimmed = raw.trim();
    if (trimmed === '') return '';
    if (trimmed === 'true') return true;
    if (trimmed === 'false') return false;
    if (trimmed === 'null') return null;
    if (!Number.isNaN(Number(trimmed))) return Number(trimmed);
    try {
        return JSON.parse(trimmed);
    } catch {
        return trimmed;
    }
}

export function toEditable(value: unknown): string {
    if (typeof value === 'string') return value;
    if (typeof value === 'number' || typeof value === 'boolean') return String(value);
    if (value === null || value === undefined) return '';
    try {
        return JSON.stringify(value);
    } catch {
        return String(value);
    }
}

// ─── Shared UI Components ──────────────────────────────────────

export function Panel({ title, children }: { title: string; children: ReactNode }) {
    return (
        <div className="apple-glass-panel rounded-[24px] overflow-hidden">
            <div className="px-6 py-4 border-b border-white/5 bg-white/[0.02]">
                <h3 className="text-[14px] font-semibold text-white/90">{title}</h3>
            </div>
            <div className="p-4">{children}</div>
        </div>
    );
}

export function InfoRow({ label, value, isLink, onClick }: { label: string; value: string; isLink?: boolean; onClick?: () => void }) {
    return (
        <div className="grid grid-cols-[180px_1fr] gap-3 py-2 border-b border-white/5 last:border-b-0">
            <div className="text-white/50 text-xs">{label}</div>
            <div
                className={`text-xs font-mono break-all ${isLink ? 'cursor-pointer text-blue-400 hover:underline flex items-center gap-1' : 'text-white/90'}`}
                onClick={isLink ? onClick : undefined}
            >
                {isLink && <ExternalLink className="w-3 h-3" />}
                {value}
            </div>
        </div>
    );
}

/** Colored header card with type-specific icon and glow */
export function HeaderCard({
    icon: Icon,
    name,
    subtitle,
    gradient,
    iconColor,
    glow,
    badges,
}: {
    icon: React.ComponentType<{ className?: string }>;
    name: string;
    subtitle: string;
    gradient: string;
    iconColor: string;
    glow: string;
    badges?: ReactNode;
}) {
    return (
        <div className="apple-glass-panel rounded-[24px] p-6 relative overflow-hidden">
            <div className={`absolute right-0 top-0 w-32 h-32 ${glow} blur-[60px] rounded-full pointer-events-none -mt-10 -mr-10`} />
            <div className="flex gap-6 relative z-10">
                <div className={`w-20 h-20 rounded-[16px] bg-gradient-to-br ${gradient} border border-white/10 flex items-center justify-center shadow-lg flex-none`}>
                    <Icon className={`w-10 h-10 ${iconColor} stroke-[1.5]`} />
                </div>
                <div className="flex-1">
                    <h1 className="text-[24px] font-semibold text-white tracking-tight leading-tight mb-1">{name}</h1>
                    <p className="text-[13px] text-white/50 font-mono mb-4">{subtitle}</p>
                    {badges && <div className="flex flex-wrap gap-2">{badges}</div>}
                </div>
            </div>
        </div>
    );
}
