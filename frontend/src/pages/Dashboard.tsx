import { useState, useEffect } from 'react';
import api, { type EngineStatusData, type ObjectCountData, type StatusData } from '../api';
import ProcessSelector from '../components/ProcessSelector';
import { t } from '../i18n';
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
      setError(statusResponse.error || t('Failed to connect'));
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
      <div className="flex-1 flex items-center justify-center h-full bg-background-base">
        <div className="flex flex-col items-center gap-3">
          <RefreshCw className="w-5 h-5 text-text-low animate-spin stroke-[1.5]" />
          <span className="text-text-low text-xs font-medium tracking-tight font-display">{t('Connecting to engine...')}</span>
        </div>
      </div>
    );
  }

  const isConnected = !error && !!status;

  const reconnectEngine = async () => {
    const res = await api.reconnectEngine();
    if (!res.success) {
      setError(res.error || t('Engine reconnect failed'));
      return;
    }
    await loadStatus();
  };

  return (
    <div className="flex-1 overflow-y-auto overflow-x-hidden relative scroll-smooth bg-background-base">
      <div className="max-w-5xl mx-auto p-6 space-y-6 pb-12">

        {/* Header Section */}
        <div className="flex items-end justify-between mt-1">
          <div>
            <h2 className="text-xl font-semibold text-text-high tracking-tight mb-0.5 font-display">{t('Overview')}</h2>
            <p className="text-text-low text-xs font-medium tracking-tight">{t('Real-time statistics from the Unreal Engine runtime environment.')}</p>
          </div>
          <div className="flex items-center gap-2">
            <button
              onClick={() => setShowProcessSelector(true)}
              className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle text-xs font-medium text-text-mid transition-all flex items-center gap-1.5 cursor-pointer active:scale-95 font-display"
            >
              <Zap className="w-3.5 h-3.5 text-text-low" />
              {t('Inject DLL')}
            </button>
            <button
              onClick={loadStatus}
              className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle text-xs font-medium text-text-mid transition-all flex items-center gap-1.5 cursor-pointer active:scale-95 font-display"
            >
              <RefreshCw className="w-3.5 h-3.5 text-text-low" />
              {t('Refresh')}
            </button>
            <button
              onClick={() => void reconnectEngine()}
              className="px-3 py-1.5 rounded-lg bg-accent-yellow/10 hover:bg-accent-yellow/20 border border-accent-yellow/20 text-xs font-medium text-accent-yellow transition-all flex items-center gap-1.5 cursor-pointer active:scale-95 font-display"
            >
              <Zap className="w-3.5 h-3.5" />
              {t('重连引擎')}
            </button>
            <button
              onClick={() => onNavigate('sdkdump')}
              className="px-3 py-1.5 rounded-lg bg-primary hover:bg-primary/80 text-xs font-medium text-white transition-all flex items-center gap-1.5 cursor-pointer active:scale-95 font-display"
            >
              <Download className="w-3.5 h-3.5" />
              {t('Export SDK')}
            </button>
          </div>
        </div>

        {/* Bento Grid layout */}
        <div className="grid grid-cols-1 md:grid-cols-12 gap-4 auto-rows-[minmax(110px,auto)]">

          {/* Main Status Card - Spans 8 cols */}
          <div className="md:col-span-8 bg-surface-dark border border-border-subtle rounded-xl p-5 flex flex-col justify-between group relative overflow-hidden">
            {isConnected && <div className="absolute top-0 right-0 w-48 h-48 bg-primary/5 rounded-full blur-[60px] -mr-16 -mt-16 pointer-events-none"></div>}

            <div className="flex items-start justify-between relative z-10">
              <div className="flex items-center gap-3">
                <div className={`w-9 h-9 rounded-lg flex items-center justify-center ${isConnected ? 'bg-accent-green/10 border border-accent-green/20' : 'bg-accent-red/10 border border-accent-red/20'}`}>
                  {isConnected ? <Wifi className="w-4 h-4 text-accent-green" /> : <WifiOff className="w-4 h-4 text-accent-red" />}
                </div>
                <div>
                  <h3 className="text-text-high font-semibold text-sm tracking-tight font-display">{isConnected ? t('Engine Connected') : t('Disconnected')}</h3>
                  <p className="text-text-low text-xs font-medium mt-0.5">{isConnected ? t('Dumping service is active and listening.') : error || t('Waiting for game process...')}</p>
                </div>
              </div>
              <div className="px-2 py-0.5 rounded bg-surface-stripe border border-border-subtle text-[10px] font-mono font-medium text-text-low">
                {t('PORT')} {port}
              </div>
            </div>

            {status && (
              <div className="grid grid-cols-4 gap-4 mt-6 pt-4 border-t border-border-subtle relative z-10">
                <div>
                  <div className="text-text-low text-[10px] font-bold uppercase tracking-widest mb-1 font-display">{t('Process')}</div>
                  <div className="text-text-high font-semibold text-xs truncate tracking-tight font-display" title={status.game_name}>{status.game_name}</div>
                </div>
                <div>
                  <div className="text-text-low text-[10px] font-bold uppercase tracking-widest mb-1 font-display">{t('PID')}</div>
                  <div className="text-text-mid font-mono text-xs">{status.pid}</div>
                </div>
                <div>
                  <div className="text-text-low text-[10px] font-bold uppercase tracking-widest mb-1 font-display">{t('Version')}</div>
                  <div className="text-primary font-mono text-xs">{status.game_version}</div>
                </div>
                <div>
                  <div className="text-text-low text-[10px] font-bold uppercase tracking-widest mb-1 font-display">{t('Arch')}</div>
                  <div className="text-text-mid font-mono text-xs">{status.architecture}</div>
                </div>
              </div>
            )}
          </div>

          {/* Quick Stats - Spans 4 cols */}
          <div className="md:col-span-4 grid grid-rows-2 gap-4">
            <BentoStatItem
              icon={Cpu}
              label={t('Global Objects')}
              value={status ? status.object_count.toLocaleString() : '0'}
              color="text-blue-400"
              bg="bg-blue-400/10 border border-blue-400/20"
            />
            <BentoStatItem
              icon={Activity}
              label={t('World Actors')}
              value={actorCount ? actorCount.toLocaleString() : '0'}
              color="text-purple-400"
              bg="bg-purple-400/10 border border-purple-400/20"
            />
          </div>

          {/* Sub Categories - 3 cols each */}
          <BentoCategoryBox
            icon={Box}
            label={t('Classes')}
            value={counts?.classes ?? 0}
            color="text-indigo-400"
            bg="bg-indigo-400/10 border-indigo-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={Database}
            label={t('Structs')}
            value={counts?.structs ?? 0}
            color="text-orange-400"
            bg="bg-orange-400/10 border-orange-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={Layers}
            label={t('Enums')}
            value={counts?.enums ?? 0}
            color="text-yellow-400"
            bg="bg-yellow-400/10 border-yellow-400/20"
            onClick={() => onNavigate('objects')}
          />
          <BentoCategoryBox
            icon={TerminalSquare}
            label={t('Functions')}
            value={counts?.functions ?? 0}
            color="text-green-400"
            bg="bg-green-400/10 border-green-400/20"
            onClick={() => onNavigate('functions')}
          />

          {/* Quick Actions List - Spans 8 cols */}
          <div className="md:col-span-8 bg-surface-dark border border-border-subtle rounded-xl p-5">
            <h3 className="text-text-high font-semibold tracking-tight text-sm mb-4 font-display">{t('Quick Actions')}</h3>
            <div className="space-y-2">
              <ActionListItem
                icon={Search}
                title={t('Object Browser')}
                desc={t('Search through classes, structs, and enumerations')}
                shortcut="Ctrl+O"
                onClick={() => onNavigate('objects')}
              />
              <ActionListItem
                icon={TerminalSquare}
                title={t('Function Browser')}
                desc={t('Inspect and hook engine functions')}
                shortcut="Ctrl+F"
                onClick={() => onNavigate('functions')}
              />
              <ActionListItem
                icon={Download}
                title={t('Export Center')}
                desc={t('Generate C++ SDK, USMAP, or IDA mapping scripts')}
                shortcut="Ctrl+Shift+G"
                onClick={() => onNavigate('sdkdump')}
              />
              <ActionListItem
                icon={MemoryStick}
                title={t('Memory Tool')}
                desc={t('Inspect and edit memory, pointer chains, and watches')}
                shortcut="Ctrl+M"
                onClick={() => onNavigate('memory')}
              />
            </div>
          </div>

          {/* Internal Metrics - 4 cols */}
          <div className="md:col-span-4 bg-surface-dark border border-border-subtle rounded-xl p-5 flex flex-col justify-between">
            <div className="flex items-center gap-2.5 mb-4">
              <div className="w-7 h-7 rounded-lg bg-surface-stripe border border-border-subtle flex items-center justify-center">
                <Settings2 className="w-3.5 h-3.5 text-text-low" />
              </div>
              <h3 className="text-text-high font-semibold tracking-tight text-sm font-display">{t('Core Offsets')}</h3>
            </div>

            <div className="space-y-3 flex-1 flex flex-col justify-end">
              <OffsetRow label={t('GObjects')} value={status?.gobjects_address || '0x0000000'} />
              <OffsetRow label={t('PID')} value={status ? String(status.pid) : '-'} />
              <OffsetRow label={t('UWorld')} value={engineStatus?.addresses?.gworld_ptr ? String(engineStatus.addresses.gworld_ptr) : t('Not Resolving')} dimmed={!engineStatus?.addresses?.gworld_ptr} />
              <OffsetRow label={t('ScriptOff')} value={engineStatus?.script_offset_diagnostics ? `0x${engineStatus.script_offset_diagnostics.selected_offset.toString(16)}` : '-'} />
              <OffsetRow label={t('ScriptConf')} value={engineStatus?.script_offset_diagnostics?.confidence || '-'} />
              <OffsetRow label={t('WS Events')} value={eventWsConnected ? `${t('CONNECTED')} (${eventWsCount})` : t('DISCONNECTED')} dimmed={!eventWsConnected} />
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
    <div className="bg-surface-dark border border-border-subtle rounded-xl p-4 flex flex-col justify-between group h-full">
      <div className="flex items-center justify-between mb-2">
        <div className="text-text-mid text-xs font-semibold tracking-tight font-display">{label}</div>
        <div className={`w-8 h-8 rounded-full ${bg} flex items-center justify-center`}>
          <Icon className={`w-3.5 h-3.5 ${color} stroke-[2.5]`} />
        </div>
      </div>
      <div>
        <div className="text-xl font-semibold font-mono tracking-tight text-text-high">{value}</div>
      </div>
    </div>
  );
}

function BentoCategoryBox({ icon: Icon, label, value, color, bg, onClick }: any) {
  return (
    <div onClick={onClick} className="md:col-span-3 bg-surface-dark border border-border-subtle rounded-xl p-4 cursor-pointer hover:bg-surface-stripe transition-all duration-200">
      <div className="flex items-start justify-between mb-3">
        <div className={`w-8 h-8 rounded-lg ${bg} flex items-center justify-center`}>
          <Icon className={`w-4 h-4 ${color} stroke-[2]`} />
        </div>
        <ChevronRight className="w-3.5 h-3.5 text-text-low" />
      </div>
      <div className="text-text-mid text-[11px] font-semibold tracking-tight mb-0.5 font-display">{label}</div>
      <div className="text-lg font-semibold font-mono text-text-high">{value.toLocaleString()}</div>
    </div>
  );
}

function ActionListItem({ icon: Icon, title, desc, shortcut, onClick }: any) {
  return (
    <div onClick={onClick} className="flex items-center justify-between p-3 rounded-lg bg-surface-stripe/50 border border-border-subtle hover:bg-surface-stripe cursor-pointer transition-colors group">
      <div className="flex items-center gap-3">
        <div className="w-8 h-8 rounded-lg bg-surface-dark border border-border-subtle flex items-center justify-center group-hover:bg-primary group-hover:border-primary transition-colors">
          <Icon className="w-4 h-4 text-text-low group-hover:text-white transition-colors" />
        </div>
        <div>
          <div className="text-text-high font-medium text-xs tracking-tight font-display">{title}</div>
          <div className="text-text-low text-[11px] font-medium mt-0.5">{desc}</div>
        </div>
      </div>
      <div>
        <kbd className="px-1.5 py-0.5 rounded bg-background-base border border-border-subtle text-[10px] font-mono font-medium text-text-low tracking-wide">
          {shortcut}
        </kbd>
      </div>
    </div>
  );
}

function OffsetRow({ label, value, dimmed }: { label: string, value: string, dimmed?: boolean }) {
  return (
    <div className="flex items-center justify-between">
      <span className="text-text-low font-semibold text-[10px] uppercase tracking-widest font-display">{label}</span>
      <span className={`font-mono text-[11px] font-medium bg-background-base px-1.5 py-0.5 rounded border border-border-subtle ${dimmed ? 'text-text-low' : 'text-text-mid'}`}>{value}</span>
    </div>
  );
}
