import { useState, useEffect } from 'react';
import api, { type EngineStatusData, type ObjectCountData, type StatusData } from '../api';
import ProcessSelector from '../components/ProcessSelector';
import {
  Activity,
  Cpu,
  Database,
  Box,
  TerminalSquare,
  Layers,
  MemoryStick,
  Wifi,
  WifiOff,
  ChevronRight,
  Download,
  Search,
  RefreshCw,
  Zap,
  Settings2
} from 'lucide-react';

interface DashboardProps {
  onNavigate: (page: 'objects' | 'functions' | 'memory' | 'sdkdump') => void;
}

export default function Dashboard({ onNavigate }: DashboardProps) {
  const [status, setStatus] = useState<StatusData | null>(null);
  const [engineStatus, setEngineStatus] = useState<EngineStatusData | null>(null);
  const [counts, setCounts] = useState<ObjectCountData | null>(null);
  const [actorCount, setActorCount] = useState<number>(0);
  const [eventWsConnected, setEventWsConnected] = useState(false);
  const [eventWsCount, setEventWsCount] = useState(0);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [showProcessSelector, setShowProcessSelector] = useState(false);
  const [port, setPort] = useState<number>(api.getSettings().port);

  useEffect(() => {
    loadStatus();
    const interval = setInterval(loadStatus, 5000);
    return () => clearInterval(interval);
  }, []);

  useEffect(() => {
    const conn = api.connectWebSocket('/ws/events', {
      onOpen: () => setEventWsConnected(true),
      onClose: () => setEventWsConnected(false),
      onError: () => setEventWsConnected(false),
      onMessage: () => {
        setEventWsCount((v) => v + 1);
      },
    });
    return () => conn.close();
  }, []);

  const loadStatus = async () => {
    const [statusResponse, countsResponse, worldResponse, engineResponse] = await Promise.all([
      api.getStatus(),
      api.getObjectCounts(),
      api.getWorld(),
      api.getEngineStatus(),
    ]);

    if (statusResponse.success && statusResponse.data) {
      setStatus(statusResponse.data);
      setError(null);
    } else {
      setStatus(null);
      setError(statusResponse.error || 'Failed to connect');
    }

    if (countsResponse.success && countsResponse.data) {
      setCounts(countsResponse.data);
    } else {
      setCounts(null);
    }

    if (worldResponse.success && worldResponse.data) {
      setActorCount(worldResponse.data.actor_count);
    } else {
      setActorCount(0);
    }

    if (engineResponse.success && engineResponse.data) {
      setEngineStatus(engineResponse.data);
    } else {
      setEngineStatus(null);
    }

    setPort(api.getSettings().port);
    setLoading(false);
  };

  if (loading) {
    return (
      <div className="flex-1 flex items-center justify-center h-full">
        <div className="flex flex-col items-center gap-4">
          <RefreshCw className="w-6 h-6 text-white/40 animate-spin stroke-[1.5]" />
          <span className="text-white/40 text-[13px] font-medium tracking-tight">Connecting to engine...</span>
        </div>
      </div>
    );
  }

  const isConnected = !error && !!status;

  const reconnectEngine = async () => {
    const res = await api.reconnectEngine();
    if (!res.success) {
      setError(res.error || 'Engine reconnect failed');
      return;
    }
    await loadStatus();
  };

  return (
    <div className="flex-1 overflow-y-auto overflow-x-hidden relative scroll-smooth">
      <div className="max-w-5xl mx-auto p-8 space-y-8 pb-16">

        {/* Header Section */}
        <div className="flex items-end justify-between mt-2">
          <div>
            <h2 className="text-[28px] font-semibold text-white tracking-tight mb-1">Overview</h2>
            <p className="text-white/50 text-[13px] font-medium tracking-tight">Real-time statistics from the Unreal Engine runtime environment.</p>
          </div>
          <div className="flex items-center gap-3">
            <button
              onClick={() => setShowProcessSelector(true)}
              className="px-4 py-2 rounded-[10px] bg-white/5 hover:bg-white/10 border border-white/10 text-[13px] font-medium text-white transition-all flex items-center gap-2 cursor-pointer shadow-sm active:scale-95"
            >
              <Zap className="w-4 h-4 text-white/70" />
              Inject DLL
            </button>
            <button
              onClick={loadStatus}
              className="px-4 py-2 rounded-[10px] bg-white/5 hover:bg-white/10 border border-white/10 text-[13px] font-medium text-white transition-all flex items-center gap-2 cursor-pointer shadow-sm active:scale-95"
            >
              <RefreshCw className="w-4 h-4 text-white/70" />
              Refresh
            </button>
            <button
              onClick={() => void reconnectEngine()}
              className="px-4 py-2 rounded-[10px] bg-yellow-500/20 hover:bg-yellow-500/30 border border-yellow-500/30 text-[13px] font-medium text-yellow-100 transition-all flex items-center gap-2 cursor-pointer shadow-sm active:scale-95"
            >
              <Zap className="w-4 h-4" />
              重连引擎
            </button>
            <button
              onClick={() => onNavigate('sdkdump')}
              className="px-4 py-2 rounded-[10px] bg-primary hover:bg-primary-dark text-[13px] font-medium text-white shadow-lg shadow-primary/20 transition-all flex items-center gap-2 cursor-pointer active:scale-95 border border-primary-dark"
            >
              <Download className="w-4 h-4" />
              Export SDK
            </button>
          </div>
        </div>

        {/* Bento Grid layout */}
        <div className="grid grid-cols-1 md:grid-cols-12 gap-5 auto-rows-[minmax(130px,auto)]">

          {/* Main Status Card - Spans 8 cols */}
          <div className="md:col-span-8 apple-glass-panel rounded-[32px] p-7 flex flex-col justify-between group relative overflow-hidden">
            {isConnected && <div className="absolute top-0 right-0 w-64 h-64 bg-primary/10 rounded-full blur-[80px] -mr-20 -mt-20 pointer-events-none"></div>}

            <div className="flex items-start justify-between relative z-10">
              <div className="flex items-center gap-4">
                <div className={`w-12 h-12 rounded-[16px] flex items-center justify-center shadow-lg ${isConnected ? 'bg-[#28C840]/10 border border-[#28C840]/20' : 'bg-[#FF5F56]/10 border border-[#FF5F56]/20'}`}>
                  {isConnected ? <Wifi className="w-5 h-5 text-[#28C840]" /> : <WifiOff className="w-5 h-5 text-[#FF5F56]" />}
                </div>
                <div>
                  <h3 className="text-white/90 font-semibold text-[16px] tracking-tight">{isConnected ? 'Engine Connected' : 'Disconnected'}</h3>
              <p className="text-white/40 text-[13px] font-medium mt-0.5">{isConnected ? 'Dumping service is active and listening.' : error || 'Waiting for game process...'}</p>
                </div>
              </div>
              <div className="px-3 py-1 rounded-[8px] bg-white/5 border border-white/10 text-[11px] font-mono font-medium text-white/60 shadow-sm">
                PORT {port}
              </div>
            </div>

            {status && (
              <div className="grid grid-cols-4 gap-6 mt-8 pt-6 border-t border-white/10 relative z-10">
                <div>
                  <div className="text-white/40 text-[11px] font-bold uppercase tracking-widest mb-1.5">Process</div>
                  <div className="text-white font-semibold text-[14px] truncate tracking-tight shadow-sm" title={status.game_name}>{status.game_name}</div>
                </div>
                <div>
                  <div className="text-white/40 text-[11px] font-bold uppercase tracking-widest mb-1.5">PID</div>
                  <div className="text-white/80 font-mono text-[14px]">{status.pid}</div>
                </div>
                <div>
                  <div className="text-white/40 text-[11px] font-bold uppercase tracking-widest mb-1.5">Version</div>
                  <div className="text-primary font-mono text-[14px]">{status.game_version}</div>
                </div>
                <div>
                  <div className="text-white/40 text-[11px] font-bold uppercase tracking-widest mb-1.5">Arch</div>
                  <div className="text-white/80 font-mono text-[14px]">{status.architecture}</div>
                </div>
              </div>
            )}
          </div>

          {/* Quick Stats - Spans 4 cols */}
          <div className="md:col-span-4 grid grid-rows-2 gap-5">
            <BentoStatItem
              icon={Cpu}
              label="Global Objects"
              value={status ? status.object_count.toLocaleString() : '0'}
              color="text-blue-400"
              bg="bg-blue-400/10 border border-blue-400/20"
            />
            <BentoStatItem
              icon={Activity}
              label="World Actors"
              value={actorCount ? actorCount.toLocaleString() : '0'}
              color="text-purple-400"
              bg="bg-purple-400/10 border border-purple-400/20"
            />
          </div>

          {/* Sub Categories - 3 cols each */}
          <BentoCategoryBox
            icon={Box}
            label="Classes"
            value={counts?.classes ?? 0}
            color="text-indigo-400"
            bg="bg-indigo-400/10 border-indigo-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={Database}
            label="Structs"
            value={counts?.structs ?? 0}
            color="text-orange-400"
            bg="bg-orange-400/10 border-orange-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={Layers}
            label="Enums"
            value={counts?.enums ?? 0}
            color="text-yellow-400"
            bg="bg-yellow-400/10 border-yellow-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={TerminalSquare}
            label="Functions"
            value={counts?.functions ?? 0}
            color="text-green-400"
            bg="bg-green-400/10 border-green-400/20"
            onClick={() => onNavigate('functions')}
          />

          {/* Quick Actions List - Spans 8 cols */}
          <div className="md:col-span-8 apple-glass-panel rounded-[32px] p-7">
            <h3 className="text-white/90 font-semibold tracking-tight text-[16px] mb-5">Quick Actions</h3>
            <div className="space-y-3">
              <ActionListItem
                icon={Search}
                title="Object Browser"
                desc="Search through classes, structs, and enumerations"
                shortcut="Ctrl+O"
                onClick={() => onNavigate('objects')}
              />
              <ActionListItem
                icon={TerminalSquare}
                title="Function Browser"
                desc="Inspect and hook engine functions"
                shortcut="Ctrl+F"
                onClick={() => onNavigate('functions')}
              />
              <ActionListItem
                icon={Download}
                title="Export Center"
                desc="Generate C++ SDK, USMAP, or IDA mapping scripts"
                shortcut="Ctrl+Shift+G"
                onClick={() => onNavigate('sdkdump')}
              />
              <ActionListItem
                icon={MemoryStick}
                title="Memory Tool"
                desc="Inspect and edit memory, pointer chains, and watches"
                shortcut="Ctrl+M"
                onClick={() => onNavigate('memory')}
              />
            </div>
          </div>

          {/* Internal Metrics - 4 cols */}
          <div className="md:col-span-4 apple-glass-panel rounded-[32px] p-7 flex flex-col justify-between">
            <div className="flex items-center gap-3 mb-6">
              <div className="w-8 h-8 rounded-[10px] bg-white/5 border border-white/10 flex items-center justify-center">
                <Settings2 className="w-4 h-4 text-white/60" />
              </div>
              <h3 className="text-white/90 font-semibold tracking-tight text-[16px]">Core Offsets</h3>
            </div>

            <div className="space-y-4 flex-1 flex flex-col justify-end">
              <OffsetRow label="GObjects" value={status?.gobjects_address || '0x0000000'} />
              <OffsetRow label="PID" value={status ? String(status.pid) : '-'} />
              <OffsetRow label="UWorld" value={engineStatus?.addresses?.gworld_ptr ? String(engineStatus.addresses.gworld_ptr) : 'Not Resolving'} dimmed={!engineStatus?.addresses?.gworld_ptr} />
              <OffsetRow label="ScriptOff" value={engineStatus?.script_offset_diagnostics ? `0x${engineStatus.script_offset_diagnostics.selected_offset.toString(16)}` : '-'} />
              <OffsetRow label="ScriptConf" value={engineStatus?.script_offset_diagnostics?.confidence || '-'} />
              <OffsetRow label="WS Events" value={eventWsConnected ? `CONNECTED (${eventWsCount})` : 'DISCONNECTED'} dimmed={!eventWsConnected} />
            </div>
          </div>

        </div>
      </div>

      {/* Modals */}
      <ProcessSelector
        isOpen={showProcessSelector}
        onClose={() => setShowProcessSelector(false)}
        onInjectSuccess={(pid) => {
          console.log('DLL injected successfully, PID:', pid);
          loadStatus();
        }}
      />
    </div>
  );
}

function BentoStatItem({ icon: Icon, label, value, color, bg }: any) {
  return (
    <div className="apple-glass-panel rounded-[24px] p-6 flex flex-col justify-between group h-full">
      <div className="flex items-center justify-between mb-2">
        <div className="text-white/50 text-[13px] font-semibold tracking-tight">{label}</div>
        <div className={`w-9 h-9 rounded-full ${bg} flex items-center justify-center`}>
          <Icon className={`w-4 h-4 ${color} stroke-[2.5]`} />
        </div>
      </div>
      <div>
        <div className="text-[28px] font-semibold font-mono tracking-tight text-white/90">{value}</div>
      </div>
    </div>
  );
}

function BentoCategoryBox({ icon: Icon, label, value, color, bg, onClick }: any) {
  return (
    <div onClick={onClick} className="md:col-span-3 apple-glass-panel rounded-[24px] p-6 cursor-pointer hover:bg-white/[0.04] transition-all duration-300">
      <div className="flex items-start justify-between mb-5">
        <div className={`w-10 h-10 rounded-[12px] ${bg} flex items-center justify-center`}>
          <Icon className={`w-5 h-5 ${color} stroke-[2]`} />
        </div>
        <ChevronRight className="w-4 h-4 text-white/20" />
      </div>
      <div className="text-white/50 text-[12px] font-semibold tracking-tight mb-1">{label}</div>
      <div className="text-[22px] font-semibold font-mono text-white/90">{value.toLocaleString()}</div>
    </div>
  );
}

function ActionListItem({ icon: Icon, title, desc, shortcut, onClick }: any) {
  return (
    <div onClick={onClick} className="flex items-center justify-between p-4 rounded-[16px] apple-glass-panel hover:bg-white/[0.04] cursor-pointer transition-colors group">
      <div className="flex items-center gap-4">
        <div className="w-10 h-10 rounded-full bg-white/5 border border-white/10 flex items-center justify-center group-hover:bg-primary group-hover:border-primary transition-colors">
          <Icon className="w-5 h-5 text-white/60 group-hover:text-white transition-colors" />
        </div>
        <div>
          <div className="text-white/90 font-medium text-[14px] tracking-tight">{title}</div>
          <div className="text-white/40 text-[12px] font-medium mt-0.5">{desc}</div>
        </div>
      </div>
      <div>
        <kbd className="px-2 py-1 rounded-[6px] bg-black/40 border border-white/10 text-[10px] font-mono font-medium text-white/40 shadow-inner tracking-wide">
          {shortcut}
        </kbd>
      </div>
    </div>
  );
}

function OffsetRow({ label, value, dimmed }: { label: string, value: string, dimmed?: boolean }) {
  return (
    <div className="flex items-center justify-between">
      <span className="text-white/50 font-semibold text-[11px] uppercase tracking-widest">{label}</span>
      <span className={`font-mono text-[12px] font-medium bg-black/40 px-2 py-0.5 rounded-[6px] border border-white/5 ${dimmed ? 'text-white/30' : 'text-white/80'}`}>{value}</span>
    </div>
  );
}
