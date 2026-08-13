import { AlertCircle } from 'lucide-react';

export function DomainError({ message, compact = false }: { message: string | null; compact?: boolean }) {
  if (!message) return null;
  return (
    <div className={`flex gap-2 rounded-lg border border-accent-red/20 bg-accent-red/5 text-accent-red ${compact ? 'px-2 py-1.5 text-xs' : 'p-3 text-sm'}`}>
      <AlertCircle className="w-4 h-4 flex-none mt-0.5" />
      <span className="break-words">{message}</span>
    </div>
  );
}
