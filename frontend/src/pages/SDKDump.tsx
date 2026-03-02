import { useState } from 'react';
import { Download, Code2, Database, LayoutTemplate, Coffee, CheckCircle2, RotateCcw } from 'lucide-react';

export default function SDKDump() {
    const [activeFormat, setActiveFormat] = useState('cpp');

    const formats = [
        { id: 'cpp', name: 'C++ Headers', icon: Code2, desc: 'Ready-to-use C++ pointers and structs' },
        { id: 'usmap', name: 'USMAP', icon: Database, desc: '.usmap format for FModel/CUE4Parse' },
        { id: 'json', name: 'Dumpspace JSON', icon: LayoutTemplate, desc: 'Web format for dumpspace.net' },
        { id: 'ida', name: 'IDA Script', icon: Coffee, desc: 'Python script for Ghidra / IDA Pro' },
    ];

    return (
        <div className="flex-1 overflow-auto bg-[#0A0A0C]">
            <div className="max-w-4xl mx-auto p-12">

                <div className="text-center mb-12">
                    <div className="w-20 h-20 mx-auto rounded-[24px] bg-white/5 border border-white/10 flex items-center justify-center mb-6 shadow-2xl">
                        <Download className="w-10 h-10 text-primary stroke-[1.5]" />
                    </div>
                    <h1 className="text-[32px] font-semibold text-white tracking-tight mb-2">Export Center</h1>
                    <p className="text-white/50 text-[15px] max-w-lg mx-auto">Generate game structures into standard formats for modding and reverse engineering tools.</p>
                </div>

                {/* Formats Grid */}
                <div className="grid grid-cols-2 gap-6 mb-12">
                    {formats.map(f => (
                        <button
                            key={f.id}
                            onClick={() => setActiveFormat(f.id)}
                            className={`text-left p-6 rounded-[24px] transition-all duration-300 relative overflow-hidden group border ${activeFormat === f.id ? 'bg-primary/10 border-primary/30 shadow-[0_0_40px_rgba(10,132,255,0.1)]' : 'bg-white/5 border-white/5 hover:bg-white/10'}`}
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

                {/* Configuration specific to format */}
                {activeFormat === 'cpp' && (
                    <div className="apple-glass-panel rounded-[24px] p-8 mb-8 space-y-6 animate-in fade-in slide-in-from-bottom-4 duration-500">
                        <h3 className="text-[15px] font-semibold text-white/90">C++ Export Configuration</h3>

                        <div className="grid grid-cols-2 gap-8">
                            <div className="space-y-5">
                                <div className="flex items-center justify-between">
                                    <div>
                                        <div className="text-[13px] font-medium text-white/90 mb-0.5">Static Asserts</div>
                                        <div className="text-[11px] text-white/40">Verify offsets during compilation</div>
                                    </div>
                                    <label className="relative inline-flex items-center cursor-pointer">
                                        <input type="checkbox" defaultChecked className="sr-only peer" />
                                        <div className="w-11 h-6 bg-white/10 rounded-full peer peer-checked:bg-primary transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-5 after:w-5 after:transition-all peer-checked:after:translate-x-full"></div>
                                    </label>
                                </div>

                                <div className="flex items-center justify-between">
                                    <div>
                                        <div className="text-[13px] font-medium text-white/90 mb-0.5">Padding Style</div>
                                        <div className="text-[11px] text-white/40">Character array padding</div>
                                    </div>
                                    <select className="bg-white/5 border border-white/10 text-white text-[12px] rounded-lg px-3 py-1.5 outline-none">
                                        <option>char pad_01[0xN]</option>
                                        <option>uint8 pad_01[0xN]</option>
                                    </select>
                                </div>
                            </div>

                            <div className="space-y-4">
                                <div>
                                    <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest block mb-2">Package Filter</label>
                                    <input type="text" placeholder="Leave empty for all..." className="w-full bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-4 py-2 outline-none focus:border-primary/50" />
                                </div>
                            </div>
                        </div>
                    </div>
                )}

                {/* Action Button */}
                <button className="w-full h-14 rounded-2xl bg-primary hover:bg-primary-dark text-white font-semibold text-[16px] tracking-tight shadow-[0_4px_20px_rgba(10,132,255,0.3)] hover:shadow-[0_4px_24px_rgba(10,132,255,0.4)] transition-all active:scale-[0.98]">
                    Generate {formats.find(f => f.id === activeFormat)?.name}
                </button>

                {/* History */}
                <div className="mt-12 space-y-4">
                    <h3 className="text-[13px] font-semibold text-white/50 uppercase tracking-widest ml-2">Recent Exports</h3>

                    <div className="space-y-2">
                        <div className="apple-glass-panel rounded-xl p-4 flex items-center justify-between group">
                            <div className="flex items-center gap-4">
                                <div className="w-8 h-8 rounded-full bg-green-500/10 flex items-center justify-center">
                                    <CheckCircle2 className="w-4 h-4 text-green-500" />
                                </div>
                                <div>
                                    <div className="text-[14px] font-medium text-white/90">USMAP Mappings</div>
                                    <div className="text-[11px] text-white/40">Completed in 240ms • 142,012 objects parsed</div>
                                </div>
                            </div>
                            <div className="flex gap-2 opacity-0 group-hover:opacity-100 transition-opacity">
                                <button className="px-3 py-1.5 rounded-lg bg-white/5 hover:bg-white/10 text-[12px] font-medium text-white">Open Folder</button>
                                <button className="w-8 h-8 rounded-lg bg-white/5 hover:bg-white/10 flex items-center justify-center text-white"><RotateCcw className="w-3.5 h-3.5" /></button>
                            </div>
                        </div>

                        <div className="apple-glass-panel rounded-xl p-4 flex items-center justify-between">
                            <div className="flex items-center gap-4">
                                <div className="w-8 h-8 rounded-full bg-green-500/10 flex items-center justify-center">
                                    <CheckCircle2 className="w-4 h-4 text-green-500" />
                                </div>
                                <div>
                                    <div className="text-[14px] font-medium text-white/90">C++ SDK Base</div>
                                    <div className="text-[11px] text-white/40">Completed in 2.6s • /Engine, /CoreUObject</div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

            </div>
        </div>
    );
}
