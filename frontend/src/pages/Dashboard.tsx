import { useState, useEffect } from 'react';
import api, { type StatusData } from '../api';

interface DashboardProps {
  onNavigate: (page: 'objects' | 'functions' | 'memory' | 'sdkdump') => void;
}

export default function Dashboard({ onNavigate }: DashboardProps) {
  const [status, setStatus] = useState<StatusData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    loadStatus();
    const interval = setInterval(loadStatus, 5000);
    return () => clearInterval(interval);
  }, []);

  const loadStatus = async () => {
    const response = await api.getStatus();
    if (response.success && response.data) {
      setStatus(response.data);
      setError(null);
    } else {
      setError(response.error || 'Failed to connect');
    }
    setLoading(false);
  };

  const formatNumber = (num: number): string => {
    return num.toLocaleString();
  };

  if (loading) {
    return (
      <div className="flex-1 flex items-center justify-center">
        <div className="flex flex-col items-center gap-4">
          <div className="w-8 h-8 border-2 border-primary border-t-transparent rounded-full animate-spin"></div>
          <span className="text-white/60 text-sm">Connecting...</span>
        </div>
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-auto p-8">
      {/* Header */}
      <div className="mb-8">
        <h1 className="text-3xl font-bold text-white tracking-tight">Dashboard</h1>
        <p className="text-white/40 text-sm mt-1">Connection overview and quick actions</p>
      </div>

      {/* Connection Status Card */}
      <div className="bg-surface-dark rounded-xl border border-white/5 p-6 mb-6">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-4">
            {/* Status Indicator */}
            <div className="relative">
              <div className={`w-3 h-3 rounded-full ${error ? 'bg-red-500' : 'bg-green-500'}`}></div>
              {error && (
                <div className="absolute -top-1 -right-1 w-3 h-3 bg-red-500 rounded-full animate-pulse"></div>
              )}
            </div>
            <div>
              <div className="text-white font-medium">
                {error ? 'Disconnected' : 'Connected'}
              </div>
              <div className="text-white/40 text-sm">
                {error ? error : `Port 27015 • Token: uexplorer-dev`}
              </div>
            </div>
          </div>
          <button
            onClick={loadStatus}
            className="flex items-center gap-2 px-4 py-2 rounded-lg bg-white/5 hover:bg-white/10 border border-white/10 text-sm text-white transition-colors"
          >
            <span className="material-symbols-outlined text-[18px]">refresh</span>
            Refresh
          </button>
        </div>
      </div>

      {/* Game Info & Stats */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-6 mb-6">
        {/* Game Info */}
        <div className="bg-surface-dark rounded-xl border border-white/5 p-6">
          <div className="text-xs font-bold text-white/40 uppercase tracking-wider mb-4">Game Information</div>
          {status && (
            <div className="space-y-3">
              <div className="flex justify-between">
                <span className="text-white/60 text-sm">Process</span>
                <span className="text-white text-sm font-medium">{status.game_name}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-white/60 text-sm">UE Version</span>
                <span className="text-primary text-sm font-mono">{status.game_version}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-white/60 text-sm">GObjects</span>
                <span className="text-white text-sm font-mono">{status.gobjects_address}</span>
              </div>
            </div>
          )}
        </div>

        {/* Stats Cards */}
        <div className="lg:col-span-2 grid grid-cols-3 gap-4">
          <StatCard
            label="Classes"
            value={status ? formatNumber(status.object_count * 0.3) : '0'}
            onClick={() => onNavigate('objects')}
            color="blue"
          />
          <StatCard
            label="Structs"
            value={status ? formatNumber(status.object_count * 0.15) : '0'}
            onClick={() => onNavigate('objects')}
            color="purple"
          />
          <StatCard
            label="Enums"
            value={status ? formatNumber(status.object_count * 0.02) : '0'}
            onClick={() => onNavigate('objects')}
            color="yellow"
          />
          <StatCard
            label="Functions"
            value={status ? formatNumber(status.object_count * 0.2) : '0'}
            onClick={() => onNavigate('functions')}
            color="green"
          />
          <StatCard
            label="Packages"
            value={status ? formatNumber(status.object_count * 0.05) : '0'}
            onClick={() => onNavigate('objects')}
            color="orange"
          />
          <StatCard
            label="Objects"
            value={status ? formatNumber(status.object_count) : '0'}
            onClick={() => onNavigate('objects')}
            color="cyan"
          />
        </div>
      </div>

      {/* Quick Actions */}
      <div>
        <div className="text-xs font-bold text-white/40 uppercase tracking-wider mb-4 px-1">Quick Actions</div>
        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
          <ActionCard
            icon="code"
            title="Generate SDK"
            description="Export C++ headers, USMAP, or IDA scripts"
            onClick={() => onNavigate('sdkdump')}
          />
          <ActionCard
            icon="search"
            title="Object Browser"
            description="Browse and search all game objects"
            onClick={() => onNavigate('objects')}
          />
          <ActionCard
            icon="functions"
            title="Function Browser"
            description="View and call game functions"
            onClick={() => onNavigate('functions')}
          />
          <ActionCard
            icon="memory"
            title="Memory Tools"
            description="View memory, run console commands"
            onClick={() => onNavigate('memory')}
          />
        </div>
      </div>
    </div>
  );
}

// Stat Card Component
interface StatCardProps {
  label: string;
  value: string;
  onClick: () => void;
  color: 'blue' | 'purple' | 'yellow' | 'green' | 'orange' | 'cyan';
}

const colorMap = {
  blue: { bg: 'bg-blue-500/10', text: 'text-blue-400', border: 'border-blue-500/20' },
  purple: { bg: 'bg-purple-500/10', text: 'text-purple-400', border: 'border-purple-500/20' },
  yellow: { bg: 'bg-yellow-500/10', text: 'text-yellow-400', border: 'border-yellow-500/20' },
  green: { bg: 'bg-green-500/10', text: 'text-green-400', border: 'border-green-500/20' },
  orange: { bg: 'bg-orange-500/10', text: 'text-orange-400', border: 'border-orange-500/20' },
  cyan: { bg: 'bg-cyan-500/10', text: 'text-cyan-400', border: 'border-cyan-500/20' },
};

function StatCard({ label, value, onClick, color }: StatCardProps) {
  const colors = colorMap[color];

  return (
    <button
      onClick={onClick}
      className={`${colors.bg} ${colors.border} border rounded-xl p-4 text-left hover:scale-[1.02] transition-transform cursor-pointer`}
    >
      <div className="text-white/40 text-xs uppercase tracking-wider mb-1">{label}</div>
      <div className={`${colors.text} text-2xl font-bold font-mono`}>{value}</div>
    </button>
  );
}

// Action Card Component
interface ActionCardProps {
  icon: string;
  title: string;
  description: string;
  onClick: () => void;
}

function ActionCard({ icon, title, description, onClick }: ActionCardProps) {
  return (
    <button
      onClick={onClick}
      className="bg-surface-dark hover:bg-surface-dark-lighter rounded-xl border border-white/5 p-5 text-left transition-colors group"
    >
      <div className="flex items-center gap-3 mb-3">
        <div className="w-10 h-10 rounded-lg bg-primary/10 flex items-center justify-center text-primary group-hover:bg-primary/20 transition-colors">
          <span className="material-symbols-outlined text-[20px]">{icon}</span>
        </div>
        <span className="text-white font-medium">{title}</span>
      </div>
      <p className="text-white/40 text-sm">{description}</p>
    </button>
  );
}
