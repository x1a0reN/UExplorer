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
    <div className="flex-1 overflow-auto bg-[#0A0A0C]">
      <div className="max-w-5xl mx-auto p-10">
        <div className="text-center mb-10">
          <div className="w-20 h-20 mx-auto rounded-[24px] bg-white/5 border border-white/10 flex items-center justify-center mb-6 shadow-2xl">
            <Download className="w-10 h-10 text-primary stroke-[1.5]" />
          </div>
          <h1 className="text-[32px] font-semibold text-white tracking-tight mb-2">Export Center</h1>
          <p className="text-white/50 text-[15px] max-w-lg mx-auto">Generate game structures into standard formats for reverse engineering workflows.</p>
        </div>

        <div className="grid grid-cols-2 gap-6 mb-10">
          {formats.map((f) => (
            <button
              key={f.id}
              onClick={() => setActiveFormat(f.id)}
              className={`text-left p-6 rounded-[24px] transition-all duration-300 relative overflow-hidden group border ${
                activeFormat === f.id ? 'bg-primary/10 border-primary/30 shadow-[0_0_40px_rgba(10,132,255,0.1)]' : 'bg-white/5 border-white/5 hover:bg-white/10'
              }`}
            >
              <div className="flex gap-4 relative z-10">
                <div className={`w-12 h-12 rounded-[14px] flex items-center justify-center flex-none mt-1 ${activeFormat === f.id ? 'bg-primary text-white' : 'bg-black/40 text-white/50 group-hover:text-white'}`}>
                  <f.icon className="w-6 h-6 stroke-[1.5]" />
                </div>
                <div>
                  <h3 className={`text-[17px] font-semibold tracking-tight mb-1 ${activeFormat === f.id ? 'text-white' : 'text-white/80'}`}>{f.name}</h3>
                  <p className={`text-[13px] leading-relaxed ${activeFormat === f.id ? 'text-blue-300/80' : 'text-white/40'}`}>{f.desc}</p>
                </div>
              </div>
              {activeFormat === f.id && (
                <div className="absolute right-6 top-1/2 -translate-y-1/2">
                  <CheckCircle2 className="w-6 h-6 text-primary" />
                </div>
              )}
            </button>
          ))}
        </div>

        <div className="apple-glass-panel rounded-[24px] p-8 mb-8 space-y-6">
          <h3 className="text-[15px] font-semibold text-white/90">Export Configuration</h3>

          <div className="grid grid-cols-2 gap-8">
            <div className="space-y-4">
              <div>
                <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest block mb-2">Include Packages</label>
                <input
                  type="text"
                  value={includePackages}
                  onChange={(e) => setIncludePackages(e.target.value)}
                  placeholder="Engine, CoreUObject"
                  className="w-full bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-4 py-2 outline-none focus:border-primary/50"
                />
              </div>
              <div>
                <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest block mb-2">Exclude Packages</label>
                <input
                  type="text"
                  value={excludePackages}
                  onChange={(e) => setExcludePackages(e.target.value)}
                  placeholder="Temp, Dev"
                  className="w-full bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-4 py-2 outline-none focus:border-primary/50"
                />
              </div>
            </div>

            <div className="space-y-5">
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
                  <div className="text-[13px] font-medium text-white/90 mb-0.5">Padding Style</div>
                  <div className="text-[11px] text-white/40">C++ unknown bytes style</div>
                </div>
                <select
                  value={paddingStyle}
                  onChange={(e) => setPaddingStyle(e.target.value)}
                  className="bg-white/5 border border-white/10 text-white text-[12px] rounded-lg px-3 py-1.5 outline-none"
                >
                  <option>char pad_01[0xN]</option>
                  <option>uint8 UnknownData_01[0xN]</option>
                </select>
              </div>
            </div>
          </div>
        </div>

        <div className="grid grid-cols-[1fr_320px] gap-6 mb-10">
          <button
            disabled={busy}
            onClick={() => void startGenerate(activeFormat)}
            className="h-14 rounded-2xl bg-primary hover:bg-primary-dark text-white font-semibold text-[16px] tracking-tight shadow-[0_4px_20px_rgba(10,132,255,0.3)] transition-all active:scale-[0.98] disabled:opacity-60"
          >
            {busy ? 'Creating Task...' : `Generate ${formats.find((f) => f.id === activeFormat)?.name}`}
          </button>

          <div className="apple-glass-panel rounded-2xl px-4 py-3 flex items-center justify-between">
            <div>
              <div className="text-white/40 text-xs">Current Task</div>
              <div className="text-white/90 text-sm font-mono truncate max-w-[190px]">{runningJob?.id || 'none'}</div>
            </div>
            <button
              onClick={() => void loadJobs()}
              className="w-8 h-8 rounded-lg bg-white/10 hover:bg-white/20 flex items-center justify-center"
            >
              <RefreshCw className="w-4 h-4 text-white/70" />
            </button>
          </div>
        </div>

        {runningJob && (
          <div className="apple-glass-panel rounded-[18px] p-4 mb-8">
            <div className="flex items-center justify-between mb-3">
              <div className="text-sm text-white/90 font-medium">Running: {runningJob.format}</div>
              <div className="text-xs font-mono text-white/60">{runningJob.status}</div>
            </div>
            <div className="h-2 rounded bg-white/10 overflow-hidden">
              <div
                className={`h-full ${runningJob.status === 'running' ? 'bg-blue-400 animate-pulse' : runningJob.status === 'completed' ? 'bg-green-400' : 'bg-red-400'}`}
                style={{ width: runningJob.status === 'running' ? '60%' : '100%' }}
              />
            </div>
            <div className="mt-2 text-xs text-white/50">Duration: {runningJob.duration_ms} ms</div>
          </div>
        )}

        <div className="apple-glass-panel rounded-[18px] p-4 mb-8">
          <div className="flex items-center gap-3 mb-3">
            <input
              type="text"
              value={selectedJobId}
              onChange={(e) => setSelectedJobId(e.target.value)}
              placeholder="输入 Job ID（例如 job-3）"
              className="flex-1 bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary/50"
            />
            <button
              onClick={() => void loadJobDetail(selectedJobId)}
              className="px-3 py-2 rounded-lg bg-white/10 hover:bg-white/20 text-white text-sm"
            >
              {t('View details')}
            </button>
            <button
              onClick={() => {
                if (selectedJobDetail?.id) void loadJobDetail(selectedJobDetail.id);
              }}
              className="w-8 h-8 rounded-lg bg-white/10 hover:bg-white/20 flex items-center justify-center"
              title={t('Refresh details')}
            >
              <RefreshCw className={`w-4 h-4 text-white/70 ${detailLoading ? 'animate-spin' : ''}`} />
            </button>
          </div>
          <pre className="bg-black/40 border border-white/10 rounded-lg p-3 text-xs text-white/80 font-mono whitespace-pre-wrap min-h-[96px]">
            {selectedJobDetail
              ? JSON.stringify(selectedJobDetail, null, 2)
              : detailLoading
                ? 'Loading...'
                : '未选择任务'}
          </pre>
        </div>

        {error && <div className="text-red-300 text-sm mb-6">{error}</div>}

        <div className="mt-10 space-y-4">
          <h3 className="text-[13px] font-semibold text-white/50 uppercase tracking-widest ml-2">Task History</h3>
          <div className="space-y-2">
            {sortedJobs.map((job) => (
              <div key={job.id} className="apple-glass-panel rounded-xl p-4 flex items-center justify-between group">
                <div className="flex items-center gap-4 min-w-0">
                  <div className={`w-8 h-8 rounded-full flex items-center justify-center ${job.status === 'completed' ? 'bg-green-500/10' : job.status === 'failed' ? 'bg-red-500/10' : 'bg-blue-500/10'}`}>
                    {job.status === 'running' ? (
                      <PlayCircle className="w-4 h-4 text-blue-400" />
                    ) : (
                      <CheckCircle2 className={`w-4 h-4 ${job.status === 'completed' ? 'text-green-500' : 'text-red-400'}`} />
                    )}
                  </div>
                  <div className="min-w-0">
                    <div className="text-[14px] font-medium text-white/90 truncate">{job.id}</div>
                    <div className="text-[11px] text-white/40 truncate">
                      {job.format} | {job.status} | {job.duration_ms} ms | {openFolderText(job)}
                    </div>
                  </div>
                </div>
                <div className="flex gap-2 opacity-100">
                  <button
                    onClick={() => void startGenerate(job.format)}
                    className="w-8 h-8 rounded-lg bg-white/5 hover:bg-white/10 flex items-center justify-center text-white"
                    title="Rerun"
                  >
                    <RotateCcw className="w-3.5 h-3.5" />
                  </button>
                  <button
                    onClick={() => {
                      setSelectedJobId(job.id);
                      void loadJobDetail(job.id);
                    }}
                    className="px-2 h-8 rounded-lg bg-white/5 hover:bg-white/10 text-white text-[11px]"
                    title={t('View details')}
                  >
                    {t('Details')}
                  </button>
                </div>
              </div>
            ))}
            {sortedJobs.length === 0 && <div className="text-white/40 text-sm px-2">No jobs yet</div>}
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
        <div className="text-[13px] font-medium text-white/90 mb-0.5">{title}</div>
        <div className="text-[11px] text-white/40">{subtitle}</div>
      </div>
      <label className="relative inline-flex items-center cursor-pointer">
        <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} className="sr-only peer" />
        <div className="w-11 h-6 bg-white/10 rounded-full peer peer-checked:bg-primary transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-5 after:w-5 after:transition-all peer-checked:after:translate-x-full" />
      </label>
    </div>
  );
}


