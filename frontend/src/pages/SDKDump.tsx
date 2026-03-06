import { useCallback, useEffect, useMemo, useState, type ComponentType } from 'react';
import { Download, Code2, Database, LayoutTemplate, Coffee, CheckCircle2, RotateCcw, PlayCircle, RefreshCw } from 'lucide-react';
import { t } from '../i18n';
import api, { type DumpJob, type DumpType } from '../api';

interface DumpFormat {
  id: DumpType;
  name: string;
  icon: ComponentType<{ className?: string }>;
  desc: string;
}

const formats: DumpFormat[] = [
  { id: 'sdk', name: 'C++ Headers', icon: Code2, desc: 'Ready-to-use C++ pointers and structs' },
  { id: 'usmap', name: 'USMAP', icon: Database, desc: '.usmap format for FModel/CUE4Parse' },
  { id: 'dumpspace', name: 'Dumpspace JSON', icon: LayoutTemplate, desc: 'Web format for dumpspace.net' },
  { id: 'ida-script', name: 'IDA Script', icon: Coffee, desc: 'Python script for Ghidra / IDA Pro' },
];

export default function SDKDump() {
  const [activeFormat, setActiveFormat] = useState<DumpType>('sdk');
  const [includePackages, setIncludePackages] = useState('');
  const [excludePackages, setExcludePackages] = useState('');
  const [includeBlueprint, setIncludeBlueprint] = useState(true);
  const [paddingStyle, setPaddingStyle] = useState('char pad_01[0xN]');
  const [staticAssert, setStaticAssert] = useState(true);

  const [jobs, setJobs] = useState<DumpJob[]>([]);
  const [runningJobId, setRunningJobId] = useState<string | null>(null);
  const [selectedJobId, setSelectedJobId] = useState('');
  const [selectedJobDetail, setSelectedJobDetail] = useState<DumpJob | null>(null);
  const [detailLoading, setDetailLoading] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const loadJobs = useCallback(async () => {
    const res = await api.getDumpJobs();
    if (!res.success || !res.data) return;
    setJobs(res.data);
  }, []);

  const loadJobDetail = useCallback(async (id: string) => {
    const target = id.trim();
    if (!target) return;
    setDetailLoading(true);
    const res = await api.getDumpJob(target);
    setDetailLoading(false);
    if (!res.success || !res.data) {
      setError(res.error || t('Failed to load job detail'));
      return;
    }
    setSelectedJobDetail(res.data);
  }, []);

  useEffect(() => {
    void loadJobs();
    const timer = setInterval(() => {
      void loadJobs();
    }, 1200);
    return () => clearInterval(timer);
  }, [loadJobs]);

  const sortedJobs = useMemo(
    () => [...jobs].sort((a, b) => (b.start_time || 0) - (a.start_time || 0)),
    [jobs]
  );

  const runningJob = sortedJobs.find((j) => j.id === runningJobId) || sortedJobs.find((j) => j.status === 'running') || null;

  const buildOptions = () => ({
    include_packages: includePackages,
    exclude_packages: excludePackages,
    include_blueprint: includeBlueprint,
    padding_style: paddingStyle,
    static_assert: staticAssert,
  });

  const startGenerate = async (format: DumpType = activeFormat) => {
    setBusy(true);
    setError(null);
    const res = await api.startDump(format, buildOptions());
    setBusy(false);

    if (!res.success || !res.data) {
      setError(res.error || t('Failed to create dump job'));
      return;
    }

    setRunningJobId(res.data.job_id);
    setSelectedJobId(res.data.job_id);
    void loadJobDetail(res.data.job_id);
    await loadJobs();
  };

  const openFolderText = (job: DumpJob) => {
    return job.output_path || '(waiting output path)';
  };

  return (
    <div className="flex-1 overflow-auto bg-background-base">
      <div className="max-w-5xl mx-auto p-10">
        <div className="text-center mb-10">
          <div className="w-16 h-16 mx-auto rounded-[20px] bg-surface-dark border border-border-subtle flex items-center justify-center mb-5 shadow-xl">
            <Download className="w-8 h-8 text-primary stroke-[1.5]" />
          </div>
          <h1 className="text-2xl font-semibold text-text-high tracking-tight mb-2 font-display">Export Center</h1>
          <p className="text-text-low text-[13px] max-w-lg mx-auto font-medium">Generate game structures into standard formats for reverse engineering workflows.</p>
        </div>

        <div className="grid grid-cols-2 gap-5 mb-10">
          {formats.map((f) => (
            <button
              key={f.id}
              onClick={() => setActiveFormat(f.id)}
              className={`text-left p-5 rounded-2xl transition-all duration-200 relative overflow-hidden group border ${activeFormat === f.id ? 'bg-primary/10 border-primary/30' : 'bg-surface-dark border-border-subtle hover:bg-surface-stripe'
                }`}
            >
              <div className="flex gap-4 relative z-10">
                <div className={`w-10 h-10 rounded-xl flex items-center justify-center flex-none mt-0.5 ${activeFormat === f.id ? 'bg-primary text-white' : 'bg-background-base border border-border-subtle text-text-mid group-hover:text-text-high transition-colors'}`}>
                  <f.icon className="w-5 h-5 stroke-[1.5]" />
                </div>
                <div>
                  <h3 className={`text-[15px] font-semibold tracking-tight mb-0.5 font-display ${activeFormat === f.id ? 'text-primary' : 'text-text-high'}`}>{f.name}</h3>
                  <p className={`text-[12px] leading-relaxed font-medium ${activeFormat === f.id ? 'text-primary/70' : 'text-text-low'}`}>{f.desc}</p>
                </div>
              </div>
              {activeFormat === f.id && (
                <div className="absolute right-5 top-1/2 -translate-y-1/2">
                  <CheckCircle2 className="w-5 h-5 text-primary" />
                </div>
              )}
            </button>
          ))}
        </div>

        <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 mb-8 space-y-5">
          <h3 className="text-sm font-semibold text-text-high font-display">Export Configuration</h3>

          <div className="grid grid-cols-2 gap-8">
            <div className="space-y-4">
              <div>
                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest block mb-1.5 font-display">Include Packages</label>
                <input
                  type="text"
                  value={includePackages}
                  onChange={(e) => setIncludePackages(e.target.value)}
                  placeholder="Engine, CoreUObject"
                  className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-2 outline-none focus:border-primary font-mono placeholder:text-text-low/50"
                />
              </div>
              <div>
                <label className="text-[10px] font-bold text-text-low uppercase tracking-widest block mb-1.5 font-display">Exclude Packages</label>
                <input
                  type="text"
                  value={excludePackages}
                  onChange={(e) => setExcludePackages(e.target.value)}
                  placeholder="Temp, Dev"
                  className="w-full bg-background-base border border-border-subtle text-text-high text-xs rounded-lg px-3 py-2 outline-none focus:border-primary font-mono placeholder:text-text-low/50"
                />
              </div>
            </div>

            <div className="space-y-4">
              <ToggleRow
                title="Include Blueprint Classes"
                subtitle="BlueprintGeneratedClass support"
                checked={includeBlueprint}
                onChange={setIncludeBlueprint}
              />
              <ToggleRow
                title="Static Asserts"
                subtitle="Generate offset/size assertions"
                checked={staticAssert}
                onChange={setStaticAssert}
              />
              <div className="flex items-center justify-between">
                <div>
                  <div className="text-xs font-medium text-text-high mb-0.5 font-display">Padding Style</div>
                  <div className="text-[11px] text-text-low">C++ unknown bytes style</div>
                </div>
                <select
                  value={paddingStyle}
                  onChange={(e) => setPaddingStyle(e.target.value)}
                  className="bg-background-base border border-border-subtle text-text-high text-[11px] font-mono rounded-lg px-2 py-1 outline-none"
                >
                  <option>char pad_01[0xN]</option>
                  <option>uint8 UnknownData_01[0xN]</option>
                </select>
              </div>
            </div>
          </div>
        </div>

        <div className="grid grid-cols-[1fr_320px] gap-5 mb-8">
          <button
            disabled={busy}
            onClick={() => void startGenerate(activeFormat)}
            className="h-12 rounded-xl bg-primary hover:bg-primary/90 text-white font-semibold text-sm tracking-tight transition-all active:scale-[0.98] disabled:opacity-50 font-display"
          >
            {busy ? 'Creating Task...' : `Generate ${formats.find((f) => f.id === activeFormat)?.name}`}
          </button>

          <div className="bg-surface-dark border border-border-subtle rounded-xl px-4 py-2 flex items-center justify-between">
            <div>
              <div className="text-text-low text-[10px] font-bold uppercase tracking-widest font-display">Current Task</div>
              <div className="text-text-high text-[11px] font-mono truncate max-w-[190px]">{runningJob?.id || 'none'}</div>
            </div>
            <button
              onClick={() => void loadJobs()}
              className="w-7 h-7 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 flex items-center justify-center border border-border-subtle"
            >
              <RefreshCw className="w-3.5 h-3.5 text-text-low" />
            </button>
          </div>
        </div>

        {runningJob && (
          <div className="bg-surface-dark border border-border-subtle rounded-xl p-4 mb-8">
            <div className="flex items-center justify-between mb-3">
              <div className="text-sm text-text-high font-medium font-display">Running: {runningJob.format}</div>
              <div className="text-xs font-mono text-text-low">{runningJob.status}</div>
            </div>
            <div className="h-2 rounded bg-surface-stripe overflow-hidden">
              <div
                className={`h-full ${runningJob.status === 'running' ? 'bg-primary animate-pulse' : runningJob.status === 'completed' ? 'bg-accent-green' : 'bg-accent-red'}`}
                style={{ width: runningJob.status === 'running' ? '60%' : '100%' }}
              />
            </div>
            <div className="mt-2 text-[11px] text-text-low font-mono">Duration: {runningJob.duration_ms} ms</div>
          </div>
        )}

        <div className="bg-surface-dark border border-border-subtle rounded-xl p-4 mb-6">
          <div className="flex items-center gap-3 mb-3">
            <input
              type="text"
              value={selectedJobId}
              onChange={(e) => setSelectedJobId(e.target.value)}
              placeholder="Enter Job ID (e.g. job-3)"
              className="flex-1 bg-background-base border border-border-subtle text-text-high text-xs font-mono rounded-lg px-3 py-1.5 outline-none focus:border-primary placeholder:text-text-low/50"
            />
            <button
              onClick={() => void loadJobDetail(selectedJobId)}
              className="px-3 py-1.5 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 border border-border-subtle text-text-high text-xs font-display"
            >
              {t('View details')}
            </button>
            <button
              onClick={() => {
                if (selectedJobDetail?.id) void loadJobDetail(selectedJobDetail.id);
              }}
              className="w-7 h-7 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 flex items-center justify-center border border-border-subtle"
              title={t('Refresh details')}
            >
              <RefreshCw className={`w-3.5 h-3.5 text-text-low ${detailLoading ? 'animate-spin' : ''}`} />
            </button>
          </div>
          <pre className="bg-background-base border border-border-subtle rounded-lg p-3 text-[11px] text-text-mid font-mono whitespace-pre-wrap min-h-[96px]">
            {selectedJobDetail
              ? JSON.stringify(selectedJobDetail, null, 2)
              : detailLoading
                ? 'Loading...'
                : 'No job selected'}
          </pre>
        </div>

        {error && <div className="text-red-300 text-sm mb-6">{error}</div>}

        <div className="mt-8 space-y-4">
          <h3 className="text-[10px] font-bold text-text-low uppercase tracking-widest ml-1 font-display">Task History</h3>
          <div className="space-y-2">
            {sortedJobs.map((job) => (
              <div key={job.id} className="bg-surface-dark border border-border-subtle rounded-xl p-3.5 flex items-center justify-between group hover:bg-surface-stripe/50 transition-colors">
                <div className="flex items-center gap-4 min-w-0">
                  <div className={`w-8 h-8 rounded-lg flex items-center justify-center ${job.status === 'completed' ? 'bg-accent-green/10 border border-accent-green/20' : job.status === 'failed' ? 'bg-accent-red/10 border border-accent-red/20' : 'bg-primary/10 border border-primary/20'}`}>
                    {job.status === 'running' ? (
                      <PlayCircle className="w-4 h-4 text-primary" />
                    ) : (
                      <CheckCircle2 className={`w-4 h-4 ${job.status === 'completed' ? 'text-accent-green' : 'text-accent-red'}`} />
                    )}
                  </div>
                  <div className="min-w-0">
                    <div className="text-sm font-medium text-text-high truncate font-display">{job.id}</div>
                    <div className="text-[11px] text-text-low font-mono truncate">
                      {job.format} | {job.status} | {job.duration_ms} ms | {openFolderText(job)}
                    </div>
                  </div>
                </div>
                <div className="flex gap-2 opacity-100">
                  <button
                    onClick={() => void startGenerate(job.format)}
                    className="w-7 h-7 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 flex items-center justify-center text-text-mid border border-border-subtle"
                    title="Rerun"
                  >
                    <RotateCcw className="w-3.5 h-3.5" />
                  </button>
                  <button
                    onClick={() => {
                      setSelectedJobId(job.id);
                      void loadJobDetail(job.id);
                    }}
                    className="px-2 h-7 rounded-lg bg-surface-stripe hover:bg-surface-stripe/80 text-text-high text-xs border border-border-subtle font-display"
                    title={t('View details')}
                  >
                    {t('Details')}
                  </button>
                </div>
              </div>
            ))}
            {sortedJobs.length === 0 && <div className="text-text-low text-xs px-2 font-display">No jobs yet</div>}
          </div>
        </div>
      </div>
    </div>
  );
}

function ToggleRow({
  title,
  subtitle,
  checked,
  onChange,
}: {
  title: string;
  subtitle: string;
  checked: boolean;
  onChange: (v: boolean) => void;
}) {
  return (
    <div className="flex items-center justify-between">
      <div>
        <div className="text-xs font-medium text-text-high mb-0.5 font-display">{title}</div>
        <div className="text-[11px] text-text-low">{subtitle}</div>
      </div>
      <label className="relative inline-flex items-center cursor-pointer">
        <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} className="sr-only peer" />
        <div className="w-9 h-5 bg-background-base border border-border-subtle rounded-full peer peer-checked:bg-primary peer-checked:border-primary transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-text-high after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full" />
      </label>
    </div>
  );
}


