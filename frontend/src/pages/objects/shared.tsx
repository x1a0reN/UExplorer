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
        <div className="bg-black/20 border border-white/5 shadow-xl rounded-2xl overflow-hidden backdrop-blur-md">
            <div className="px-6 py-4 border-b border-white/5 bg-white/[0.03]">
                <h3 className="text-[14px] font-medium tracking-wide text-slate-200">{title}</h3>
            </div>
            <div className="p-4">{children}</div>
        </div>
    );
}

export function InfoRow({ label, value, isLink, onClick }: { label: string; value: string; isLink?: boolean; onClick?: () => void }) {
    return (
        <div className="grid grid-cols-[180px_1fr] gap-4 py-3 border-b border-white/[0.03] last:border-b-0 hover:bg-white/[0.01] transition-colors rounded-sm px-2 -mx-2">
            <div className="text-slate-400 text-[13px] mb-0.5 break-words">{label}</div>
            <div
                className={`text-[13px] font-mono break-all leading-relaxed ${isLink ? 'cursor-pointer text-blue-400 hover:text-blue-300 hover:underline flex items-start gap-1.5' : 'text-slate-200'}`}
                onClick={isLink ? onClick : undefined}
            >
                {isLink && <ExternalLink className="w-3.5 h-3.5 mt-[3px] shrink-0 opacity-70" />}
                {value || <span className="text-white/10 italic">null</span>}
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
        <div className="bg-black/30 border border-white/10 shadow-2xl rounded-3xl p-6 relative overflow-hidden backdrop-blur-xl">
            <div className={`absolute right-0 top-0 w-48 h-48 ${glow} blur-[80px] rounded-full opacity-60 pointer-events-none -mt-10 -mr-10`} />
            <div className="flex gap-6 relative z-10 items-start">
                <div className={`w-20 h-20 rounded-2xl bg-gradient-to-br ${gradient} border border-white/10 flex items-center justify-center shadow-lg flex-none`}>
                    <Icon className={`w-10 h-10 ${iconColor} stroke-[1.5]`} />
                </div>
                <div className="flex-1 mt-1">
                    <h1 className="text-[26px] font-semibold text-white tracking-tight leading-tight mb-1.5">{name}</h1>
                    <p className="text-[14px] text-slate-400 font-mono mb-4">{subtitle}</p>
                    {badges && <div className="flex flex-wrap gap-2.5">{badges}</div>}
                </div>
            </div>
        </div>
    );
}
