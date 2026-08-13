import { useState, type ComponentType } from 'react';
import { Download, Code2, Database, LayoutTemplate, Coffee, CheckCircle2 } from 'lucide-react';
import { t } from '../i18n';
import { type DumpType } from '../api';

interface DumpFormat {
  id: DumpType;
  name: string;
  icon: ComponentType<{ className?: string }>;
  desc: string;
}

function getFormats(): DumpFormat[] {
  return [
    { id: 'sdk', name: t('C++ Headers'), icon: Code2, desc: t('C++ SDK headers for use with game modding') },
    { id: 'usmap', name: t('USMAP'), icon: Database, desc: t('USMAP mappings for FModel/CUE4Parse') },
    { id: 'dumpspace', name: t('Dumpspace JSON'), icon: LayoutTemplate, desc: t('JSON dump for dumpspace.net viewer') },
    { id: 'ida-script', name: t('IDA Script'), icon: Coffee, desc: t('IDA Pro import script with structs/enums') },
  ];
}

export default function SDKDump() {
  const formats = getFormats();
  const [activeFormat, setActiveFormat] = useState<DumpType>('sdk');

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

        <div className="bg-surface-dark border border-border-subtle rounded-xl p-6 mb-8">
          <h3 className="text-sm font-semibold text-text-high font-display">{t('Export Configuration')}</h3>
          <div className="mt-3 rounded-lg border border-accent-yellow/20 bg-accent-yellow/5 p-3 text-xs text-text-mid font-display leading-relaxed">
            {t('Dump execution is unavailable until an owned generator worker is connected. No legacy generator or synthetic job list is used as a fallback.')}
          </div>
        </div>

        <div className="mb-8">
          <button
            disabled
            className="w-full h-12 rounded-xl bg-primary hover:bg-primary/90 text-white font-semibold text-sm tracking-tight transition-all active:scale-[0.98] disabled:opacity-50 font-display"
          >
            {`${t('Generate')} ${formats.find((f) => f.id === activeFormat)?.name} - ${t('Unavailable')}`}
          </button>
        </div>
      </div>
    </div>
  );
}
