import { ChevronLeft, ChevronRight, Loader2 } from 'lucide-react';

export function PageControls({
  page,
  totalPages,
  onPage,
}: {
  page: number;
  totalPages: number;
  onPage: (page: number) => void;
}) {
  return (
    <div className="flex items-center gap-2">
      <button disabled={page <= 1} onClick={() => onPage(page - 1)} className="p-1 rounded border border-border-subtle disabled:opacity-30">
        <ChevronLeft className="w-3.5 h-3.5" />
      </button>
      <span className="text-[11px] text-text-low font-mono">{page} / {Math.max(1, totalPages)}</span>
      <button disabled={page >= totalPages} onClick={() => onPage(page + 1)} className="p-1 rounded border border-border-subtle disabled:opacity-30">
        <ChevronRight className="w-3.5 h-3.5" />
      </button>
    </div>
  );
}

export function LoadMoreButton({
  visible,
  loading,
  onClick,
  label = 'Load more',
}: {
  visible: boolean;
  loading: boolean;
  onClick: () => void;
  label?: string;
}) {
  if (!visible) return null;
  return (
    <button disabled={loading} onClick={onClick} className="m-3 py-1.5 border border-border-subtle rounded text-xs text-text-mid hover:text-white disabled:opacity-50 flex items-center justify-center gap-2">
      {loading && <Loader2 className="w-3.5 h-3.5 animate-spin" />}
      {label}
    </button>
  );
}
