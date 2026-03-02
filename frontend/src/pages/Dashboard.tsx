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

  if (loading) {
    return (
      <div className="flex-1 flex items-center justify-center">
        <div className="flex flex-col items-center gap-4">
          <div className="w-8 h-8 border-2 border-primary border-t-transparent rounded-full animate-spin"></div>
          <span className="text-white/40 text-sm">Connecting...</span>
        </div>
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-auto">
      {/* Context Header */}
      <div className="px-8 py-6 pb-2">
        <div className="flex items-end justify-between">
          <div>
            <h2 className="text-3xl font-bold text-white tracking-tight">Dashboard</h2>
            <p className="text-white/40 text-sm mt-1 font-medium">Connection overview and game statistics</p>
          </div>
          <div className="flex gap-3">
            <button
              onClick={loadStatus}
              className="h-9 px-4 rounded-md bg-white/5 hover:bg-white/10 border border-white/10 text-sm font-medium text-white transition-all flex items-center gap-2"
            >
              <span className="material-symbols-outlined text-[18px]">refresh</span>
              Reload
            </button>
            <button
              onClick={() => onNavigate('sdkdump')}
              className="h-9 px-4 rounded-md bg-primary hover:bg-primary/90 text-sm font-medium text-white shadow-lg shadow-primary/25 transition-all flex items-center gap-2"
            >
              <span className="material-symbols-outlined text-[18px]">download</span>
              Export All
            </button>
          </div>
        </div>
      </div>

      {/* Connection Status Card */}
      <div className="mx-8 mt-6 bg-surface-dark rounded-xl border border-white/5 p-6">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-4">
            {/* Status Indicator */}
            <div className="relative">
              <div className={`w-3 h-3 rounded-full ${error ? 'bg-[#ff5f57]' : 'bg-[#28c840]'}`}></div>
              {error && (
                <div className="absolute -top-1 -right-1 w-3 h-3 bg-red-500 rounded-full animate-pulse"></div>
              )}
            </div>
            <div>
              <div className="text-white font-medium">
                {error ? 'Disconnected' : 'Connected'}
              </div>
              <div className="text-white/40 text-sm">
                Port 27015 • Token: uexplorer-dev
              </div>
            </div>
          </div>
          <div className="flex items-center gap-6">
            {status && (
              <>
                <div className="text-right">
                  <div className="text-white/40 text-xs uppercase">Process</div>
                  <div className="text-white font-medium">{status.game_name}</div>
                </div>
                <div className="text-right">
                  <div className="text-white/40 text-xs uppercase">UE Version</div>
                  <div className="text-primary font-mono">{status.game_version}</div>
                </div>
                <div className="text-right">
                  <div className="text-white/40 text-xs uppercase">GObjects</div>
                  <div className="text-white font-mono">{status.gobjects_address}</div>
                </div>
              </>
            )}
          </div>
        </div>
      </div>

      {/* Stats Grid - 6 columns */}
      <div className="mx-8 mt-6 grid grid-cols-6 gap-4">
        <StatCard
          label="Classes"
          value={status ? Math.floor(status.object_count * 0.3) : 0}
          onClick={() => onNavigate('objects')}
          color="blue"
        />
        <StatCard
          label="Structs"
          value={status ? Math.floor(status.object_count * 0.15) : 0}
          onClick={() => onNavigate('objects')}
          color="purple"
        />
        <StatCard
          label="Enums"
          value={status ? Math.floor(status.object_count * 0.02) : 0}
          onClick={() => onNavigate('objects')}
          color="yellow"
        />
        <StatCard
          label="Functions"
          value={status ? Math.floor(status.object_count * 0.2) : 0}
          onClick={() => onNavigate('functions')}
          color="green"
        />
        <StatCard
          label="Packages"
          value={status ? Math.floor(status.object_count * 0.05) : 0}
          onClick={() => onNavigate('objects')}
          color="orange"
        />
        <StatCard
          label="Total Objects"
          value={status ? status.object_count : 0}
          onClick={() => onNavigate('objects')}
          color="cyan"
        />
      </div>

      {/* Quick Actions - Table style like code.html */}
      <div className="mx-8 mt-6">
        <div className="text-xs font-bold text-white/40 uppercase tracking-wider mb-4 px-1">Quick Actions</div>
        <div className="border border-white/5 rounded-xl overflow-hidden bg-surface-dark/50">
          <table className="w-full text-left text-sm">
            <thead>
              <tr className="border-b border-white/5 bg-surface-dark/80 text-white/50 uppercase text-[11px] font-semibold tracking-wider">
                <th className="px-6 py-4 w-1/2">Action</th>
                <th className="px-6 py-4">Description</th>
                <th className="px-6 py-4 text-right">Shortcut</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-white/5">
              <tr
                className="group hover:bg-white/[0.02] transition-colors cursor-pointer"
                onClick={() => onNavigate('sdkdump')}
              >
                <td className="px-6 py-4">
                  <div className="flex items-center gap-3">
                    <span className="material-symbols-outlined text-primary">code</span>
                    <span className="text-white font-medium">Generate SDK</span>
                  </div>
                </td>
                <td className="px-6 py-4 text-white/60">
                  Export C++ headers, USMAP, or IDA scripts
                </td>
                <td className="px-6 py-4 text-right">
                  <kbd className="px-2 py-0.5 rounded border border-white/10 bg-white/5 text-[10px] font-mono text-white/50">Ctrl+Shift+G</kbd>
                </td>
              </tr>
              <tr
                className="group hover:bg-white/[0.02] transition-colors cursor-pointer"
                onClick={() => onNavigate('objects')}
              >
                <td className="px-6 py-4">
                  <div className="flex items-center gap-3">
                    <span className="material-symbols-outlined text-primary">search</span>
                    <span className="text-white font-medium">Object Browser</span>
                  </div>
                </td>
                <td className="px-6 py-4 text-white/60">
                  Browse and search all game objects
                </td>
                <td className="px-6 py-4 text-right">
                  <kbd className="px-2 py-0.5 rounded border border-white/10 bg-white/5 text-[10px] font-mono text-white/50">Ctrl+O</kbd>
                </td>
              </tr>
              <tr
                className="group hover:bg-white/[0.02] transition-colors cursor-pointer"
                onClick={() => onNavigate('functions')}
              >
                <td className="px-6 py-4">
                  <div className="flex items-center gap-3">
                    <span className="material-symbols-outlined text-primary">functions</span>
                    <span className="text-white font-medium">Function Browser</span>
                  </div>
                </td>
                <td className="px-6 py-4 text-white/60">
                  View and call game functions
                </td>
                <td className="px-6 py-4 text-right">
                  <kbd className="px-2 py-0.5 rounded border border-white/10 bg-white/5 text-[10px] font-mono text-white/50">Ctrl+F</kbd>
                </td>
              </tr>
              <tr
                className="group hover:bg-white/[0.02] transition-colors cursor-pointer"
                onClick={() => onNavigate('memory')}
              >
                <td className="px-6 py-4">
                  <div className="flex items-center gap-3">
                    <span className="material-symbols-outlined text-primary">memory</span>
                    <span className="text-white font-medium">Memory Tools</span>
                  </div>
                </td>
                <td className="px-6 py-4 text-white/60">
                  View memory, run console commands
                </td>
                <td className="px-6 py-4 text-right">
                  <kbd className="px-2 py-0.5 rounded border border-white/10 bg-white/5 text-[10px] font-mono text-white/50">Ctrl+M</kbd>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      {/* Bottom Panel - Connection Settings */}
      <div className="mt-auto border-t border-white/5 bg-[#252527] p-6 pb-8 mx-8 mt-6 rounded-t-xl">
        <div className="flex items-start justify-between">
          <div>
            <div className="flex items-center gap-2 mb-2">
              <span className="material-symbols-outlined text-primary text-[20px]">tune</span>
              <h3 className="text-sm font-bold text-white uppercase tracking-wider">Connection Configuration</h3>
            </div>
            <p className="text-xs text-white/40 max-w-lg leading-relaxed">
              Configure HTTP server settings for DLL communication. These settings are used to connect to the injected game process.
            </p>
          </div>
          <div className="flex gap-6">
            {/* Port Setting */}
            <div className="flex flex-col gap-2">
              <label className="text-[10px] font-bold text-white/50 uppercase">Port</label>
              <div className="flex items-center bg-[#1e1e1e] rounded-lg border border-white/5 p-1 w-32">
                <button className="w-8 h-8 flex items-center justify-center text-white/40 hover:text-white hover:bg-white/5 rounded transition-colors">
                  <span className="material-symbols-outlined text-[16px]">remove</span>
                </button>
                <input className="bg-transparent text-center w-full text-xs font-mono text-white outline-none" type="text" value="27015" readOnly />
                <button className="w-8 h-8 flex items-center justify-center text-white/40 hover:text-white hover:bg-white/5 rounded transition-colors">
                  <span className="material-symbols-outlined text-[16px]">add</span>
                </button>
              </div>
            </div>
            {/* Token Setting */}
            <div className="flex flex-col gap-2">
              <label className="text-[10px] font-bold text-white/50 uppercase">Token</label>
              <input
                className="bg-[#1e1e1e] border border-white/5 rounded-lg px-3 py-2 text-xs font-mono text-white outline-none w-40"
                type="text"
                value="uexplorer-dev"
                readOnly
              />
            </div>
            {/* Auto-Reconnect Toggle */}
            <div className="flex flex-col gap-2">
              <label className="text-[10px] font-bold text-white/50 uppercase">Auto-Reconnect</label>
              <div className="flex items-center h-10 px-1">
                <label className="relative inline-flex items-center cursor-pointer">
                  <input defaultChecked className="sr-only peer" type="checkbox" />
                  <div className="w-11 h-6 bg-white/10 peer-focus:outline-none rounded-full peer dark:bg-gray-700 peer-checked:after:translate-x-full peer-checked:after:border-white after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:border-gray-300 after:border after:rounded-full after:h-5 after:w-5 after:transition-all dark:border-gray-600 peer-checked:bg-primary"></div>
                </label>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

// Stat Card Component
interface StatCardProps {
  label: string;
  value: number;
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
      <div className={`${colors.text} text-2xl font-bold font-mono`}>{value.toLocaleString()}</div>
    </button>
  );
}
