import { useEffect, useState, type ComponentType } from 'react';
import {
  AlertCircle,
  CheckCircle2,
  Code2,
  Coffee,
  Database,
  Download,
  LayoutTemplate,
  Loader2,
  XCircle,
} from 'lucide-react';
import api from '../services';
import {
  type DumpJobEvent,
  type DumpJobRecord,
  type DumpScope,
  type DumpType,
  type StatusData,
} from '../contracts';
import { t } from '../i18n';
import { useSession } from '../session/SessionProvider';
import { DomainError } from '../features/shared/DomainError';

interface DumpFormat {
  id: DumpType;
  capability: string;
  name: string;
  icon: ComponentType<{ className?: string }>;
  desc: string;
}

interface ActiveJobBinding {
  id: string;
  scope: DumpScope;
}

function getFormats(): DumpFormat[] {
  return [
    { id: 'sdk', capability: 'dump.cpp', name: t('C++ Headers'), icon: Code2, desc: t('Typed fields, enums, function parameters, and ProcessEvent helpers') },
    { id: 'usmap', capability: 'dump.usmap', name: t('USMAP'), icon: Database, desc: t('Exact USMAP mapping with fail-closed descriptor checks') },
    { id: 'dumpspace', capability: 'dump.dumpspace', name: t('Dumpspace JSON'), icon: LayoutTemplate, desc: t('Snapshot-derived Dumpspace JSON documents') },
    { id: 'ida-script', capability: 'dump.ida', name: t('IDA Script'), icon: Coffee, desc: t('IDA import script for witnessed native Function RVAs') },
  ];
}

function scopeFromStatus(status: StatusData | null): DumpScope | null {
  const sessionId = status?.runtime?.session_id;
  const contextGeneration = status?.runtime?.context_generation;
  const objectGeneration = status?.object_snapshot?.generation;
  const typeGeneration = status?.type_snapshot?.generation;
  if (!sessionId
    || !contextGeneration
    || !status?.object_snapshot?.published
    || !objectGeneration
    || !status.type_snapshot?.published
    || !typeGeneration
    || status.type_snapshot.object_snapshot_generation !== objectGeneration) {
    return null;
  }
  return {
    session_id: sessionId,
    context_generation: contextGeneration,
    object_snapshot_generation: objectGeneration,
    type_snapshot_generation: typeGeneration,
  };
}

function isTerminal(job: DumpJobRecord | null): boolean {
  return job?.state === 'succeeded' || job?.state === 'failed' || job?.state === 'cancelled';
}

export default function SDKDump() {
  const session = useSession();
  const formats = getFormats();
  const [activeFormat, setActiveFormat] = useState<DumpType>(() => api.getSettings().defaultDumpFormat);
  const status = session.status;
  const statusLoading = session.checking;
  const [submitting, setSubmitting] = useState(false);
  const [activeJob, setActiveJob] = useState<ActiveJobBinding | null>(null);
  const [job, setJob] = useState<DumpJobRecord | null>(null);
  const [events, setEvents] = useState<DumpJobEvent[]>([]);
  const [actionError, setActionError] = useState<string | null>(null);

  useEffect(() => {
    if (!activeJob) return;
    let disposed = false;
    let timer: number | null = null;
    let afterSequence = 0;

    const poll = async () => {
      const response = await api.getScopedDumpJob(
        activeJob.scope,
        activeJob.id,
        afterSequence,
        128,
      );
      if (disposed) return;
      if (!response.success || !response.data) {
        setActionError(response.error || t('Failed to load job detail'));
        timer = window.setTimeout(poll, 1500);
        return;
      }
      const snapshot = response.data;
      setActionError(null);
      setJob(snapshot.job);
      if (snapshot.events.length > 0) {
        afterSequence = snapshot.events.at(-1)?.sequence || afterSequence;
        setEvents((current) => {
          const next = snapshot.event_gap_detected
            ? snapshot.events
            : [...current, ...snapshot.events];
          return next.slice(-128);
        });
      }
      if (!isTerminal(snapshot.job)) {
        timer = window.setTimeout(poll, 750);
      }
    };

    void poll();
    return () => {
      disposed = true;
      if (timer !== null) window.clearTimeout(timer);
    };
  }, [activeJob]);

  const selected = formats.find((format) => format.id === activeFormat) || formats[0];
  const scope = scopeFromStatus(status);
  const capability = status?.capabilities?.[selected.capability];
  const canStart = !!scope && capability?.available === true && !submitting && !activeJob?.id;
  const latestProgress = [...events].reverse().find((event) => event.kind === 'progress');
  const progress = latestProgress?.kind === 'progress' ? latestProgress.payload : null;
  const progressPercent = progress && progress.total > 0
    ? Math.min(100, Math.round((progress.completed / progress.total) * 100))
    : null;

  const start = async () => {
    setSubmitting(true);
    setActionError(null);
    try {
      const statusResponse = await api.getStatus();
      const freshStatus = statusResponse.success ? statusResponse.data : null;
      const freshScope = scopeFromStatus(freshStatus);
      const freshCapability = freshStatus?.capabilities?.[selected.capability];
      if (!freshScope || !freshCapability?.available) {
        setActionError(freshCapability?.reason || freshCapability?.reason_code || t('Unavailable'));
        return;
      }
      const response = await api.startScopedDump({
        ...freshScope,
        format: activeFormat,
        deadline_ms: 10 * 60 * 1000,
        options: {},
      });
      if (!response.success || !response.data) {
        setActionError(response.error || t('Failed to create dump job'));
        return;
      }
      setEvents([]);
      setJob(null);
      setActiveJob({ id: response.data.job_id, scope: freshScope });
    } catch (error) {
      setActionError(error instanceof Error ? error.message : t('Failed to create dump job'));
    } finally {
      setSubmitting(false);
    }
  };

  const cancel = async () => {
    if (!activeJob || !job || isTerminal(job)) return;
    const response = await api.cancelScopedDumpJob(activeJob.scope, activeJob.id);
    if (!response.success) setActionError(response.error || t('Failed'));
  };

  const clearFinished = () => {
    if (!isTerminal(job)) return;
    setActiveJob(null);
    setJob(null);
    setEvents([]);
    setActionError(null);
  };

  return (
    <div className="flex-1 overflow-auto bg-background-base">
      <div className="max-w-5xl mx-auto p-10">
        <div className="text-center mb-10">
          <div className="w-16 h-16 mx-auto rounded-[20px] bg-surface-dark border border-border-subtle flex items-center justify-center mb-5 shadow-xl">
            <Download className="w-8 h-8 text-primary stroke-[1.5]" />
          </div>
          <h1 className="text-2xl font-semibold text-text-high tracking-tight mb-2 font-display">{t('Export Center')}</h1>
          <p className="text-text-low text-[13px] max-w-lg mx-auto font-medium">{t('Generate game structures into standard formats for SDK development, reverse engineering, and tool integration.')}</p>
        </div>

        <div className="grid grid-cols-2 gap-5 mb-8">
          {formats.map((format) => (
            <button
              key={format.id}
              disabled={!!activeJob}
              onClick={() => setActiveFormat(format.id)}
              className={`text-left p-5 rounded-2xl transition-all relative overflow-hidden group border disabled:opacity-60 ${activeFormat === format.id ? 'bg-primary/10 border-primary/30' : 'bg-surface-dark border-border-subtle hover:bg-surface-stripe'}`}
            >
              <div className="flex gap-4 relative z-10">
                <div className={`w-10 h-10 rounded-xl flex items-center justify-center flex-none mt-0.5 ${activeFormat === format.id ? 'bg-primary text-white' : 'bg-background-base border border-border-subtle text-text-mid group-hover:text-text-high'}`}>
                  <format.icon className="w-5 h-5 stroke-[1.5]" />
                </div>
                <div className="pr-7">
                  <h3 className={`text-[15px] font-semibold tracking-tight mb-0.5 font-display ${activeFormat === format.id ? 'text-primary' : 'text-text-high'}`}>{format.name}</h3>
                  <p className={`text-[12px] leading-relaxed font-medium ${activeFormat === format.id ? 'text-primary/70' : 'text-text-low'}`}>{format.desc}</p>
                </div>
              </div>
              {activeFormat === format.id && <CheckCircle2 className="absolute right-5 top-1/2 -translate-y-1/2 w-5 h-5 text-primary" />}
            </button>
          ))}
        </div>

        <div className="bg-surface-dark border border-border-subtle rounded-xl p-5 mb-6">
          <div className="flex items-start justify-between gap-4">
            <div>
              <h3 className="text-sm font-semibold text-text-high font-display">{t('Export Configuration')}</h3>
              <p className="mt-1 text-xs text-text-low font-display">{t('Artifacts are written under LocalAppData/UExplorer/Dumps using a session-scoped immutable snapshot job.')}</p>
            </div>
            <div className={`px-2.5 py-1 rounded-full border text-[11px] font-semibold ${capability?.available ? 'border-accent-green/25 bg-accent-green/10 text-accent-green' : 'border-accent-yellow/25 bg-accent-yellow/10 text-accent-yellow'}`}>
              {statusLoading ? t('Loading...') : capability?.available ? t('Ready') : t('Unavailable')}
            </div>
          </div>
          {!capability?.available && !statusLoading && (
            <div className="mt-3 flex gap-2 rounded-lg border border-accent-yellow/20 bg-accent-yellow/5 p-3 text-xs text-text-mid">
              <AlertCircle className="w-4 h-4 text-accent-yellow flex-none" />
              <span>{capability?.reason || capability?.reason_code || session.lastError || t('Unavailable')}</span>
            </div>
          )}
          <div className="mt-3 text-[11px] text-text-low">{t('Code path is available independently of release support; real target fixture verification is still pending.')}</div>
        </div>

        {activeJob && (
          <div className="bg-surface-dark border border-border-subtle rounded-xl p-5 mb-6">
            <div className="flex items-center justify-between gap-4">
              <div>
                <div className="text-xs text-text-low">{t('Current Task')} · {activeJob.id}</div>
                <div className="mt-1 flex items-center gap-2 text-sm font-semibold text-text-high">
                  {!job || !isTerminal(job) ? <Loader2 className="w-4 h-4 animate-spin text-primary" /> : job.state === 'succeeded' ? <CheckCircle2 className="w-4 h-4 text-accent-green" /> : <XCircle className="w-4 h-4 text-accent-red" />}
                  {job?.state || 'queued'}
                </div>
              </div>
              <button
                onClick={() => void (isTerminal(job) ? clearFinished() : cancel())}
                className="px-3 py-1.5 rounded-lg border border-border-subtle bg-background-base text-xs text-text-mid hover:text-text-high"
              >
                {isTerminal(job) ? t('Clear') : t('Cancel')}
              </button>
            </div>
            <div className="mt-4 h-1.5 overflow-hidden rounded-full bg-background-base">
              <div className={`h-full bg-primary transition-all ${progressPercent === null && !isTerminal(job) ? 'w-1/3 animate-pulse' : ''}`} style={progressPercent === null ? undefined : { width: `${progressPercent}%` }} />
            </div>
            <div className="mt-2 flex justify-between text-[11px] text-text-low">
              <span>{progress?.phase || job?.state || 'queued'} · {progress?.message || ''}</span>
              <span>{progressPercent === null ? '' : `${progressPercent}%`}</span>
            </div>
            {job?.error && <div className="mt-3 rounded-lg border border-accent-red/20 bg-accent-red/5 p-3 text-xs text-accent-red">{job.error.code}: {job.error.message}</div>}
            {job && <div className="mt-3 text-[11px] text-text-low">{t('Output Path')}: %LOCALAPPDATA%\UExplorer\Dumps\{job.scope.session_id}\{job.output_path_identity}</div>}
          </div>
        )}

        {actionError && <div className="mb-5"><DomainError message={actionError} compact /></div>}

        <button
          disabled={!canStart}
          onClick={() => void start()}
          className="w-full h-12 rounded-xl bg-primary hover:bg-primary/90 text-white font-semibold text-sm tracking-tight transition-all active:scale-[0.98] disabled:opacity-50 font-display"
        >
          {submitting ? t('Creating Task...') : `${t('Generate')} ${selected.name}`}
        </button>
      </div>
    </div>
  );
}
