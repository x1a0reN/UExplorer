import { RefreshCw } from 'lucide-react';

export function PropertyValueEditor({
  value,
  refreshing,
  onRefresh,
}: {
  value: string;
  refreshing: boolean;
  onRefresh: () => void;
}) {
  return (
    <div className="flex items-center gap-2">
      <input
        type="text"
        value={value}
        readOnly
        className="min-w-0 flex-1 bg-background-base border border-border-subtle rounded-md px-3 py-1 text-[13px] text-text-mid font-mono cursor-default"
      />
      <button
        onClick={onRefresh}
        disabled={refreshing}
        title="Refresh"
        className="p-1.5 rounded-md hover:bg-primary/15 text-text-low hover:text-primary disabled:opacity-50"
      >
        <RefreshCw className={`w-3.5 h-3.5 ${refreshing ? 'animate-spin' : ''}`} />
      </button>
    </div>
  );
}
