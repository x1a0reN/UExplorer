import { Terminal, Binary, Bookmark, Search, ArrowRight, ArrowLeft, RefreshCw, Layers } from 'lucide-react';

export default function Memory() {
    return (
        <div className="flex-1 flex flex-col bg-[#0A0A0C] overflow-hidden">
            {/* Top Bar - Address Navigation */}
            <div className="flex-none h-14 bg-white/[0.02] border-b border-white/5 backdrop-blur-3xl px-6 flex items-center justify-between z-20">
                <div className="flex items-center gap-3 w-1/2">
                    <div className="flex items-center gap-1.5 mr-2">
                        <button className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/40 hover:text-white transition-colors">
                            <ArrowLeft className="w-4 h-4" />
                        </button>
                        <button className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/40 hover:text-white transition-colors">
                            <ArrowRight className="w-4 h-4" />
                        </button>
                    </div>

                    <div className="relative flex-1 group">
                        <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
                        <input
                            type="text"
                            defaultValue="0x0000021A4B00000"
                            className="w-full bg-black/40 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-1.5 outline-none focus:border-primary/50 focus:bg-black/60 transition-all font-mono placeholder:text-white/30"
                        />
                    </div>

                    <button className="w-8 h-8 flex items-center justify-center rounded-[8px] bg-white/5 hover:bg-white/10 border border-white/10 text-white transition-colors ml-2">
                        <RefreshCw className="w-4 h-4" />
                    </button>
                </div>

                <div className="flex items-center gap-4 text-[13px] font-medium text-white/50">
                    <button className="flex items-center gap-2 hover:text-white transition-colors">
                        <Bookmark className="w-4 h-4" />
                        Bookmarks
                    </button>
                </div>
            </div>

            {/* Main Split Content */}
            <div className="flex-1 flex min-h-0">
                {/* Hex View Center */}
                <div className="flex-1 flex flex-col min-w-0 border-r border-white/5 bg-[#0A0A0C]">
                    <div className="p-2 border-b border-white/5 flex items-center gap-4 bg-black/40 px-6">
                        <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest">Hex Editor</span>
                        <span className="text-[11px] font-mono text-white/50">16 Bytes/Row</span>
                    </div>
                    <div className="flex-1 overflow-auto p-6 font-mono text-[13px] leading-relaxed relative">
                        <table className="w-full border-collapse">
                            <tbody className="text-white/60">
                                {[...Array(20)].map((_, i) => (
                                    <tr key={i} className="hover:bg-white/5">
                                        <td className="pr-6 text-white/30 user-select-none border-r border-white/5">
                                            {`0000021A4B${(i * 16).toString(16).padStart(4, '0').toUpperCase()}`}
                                        </td>
                                        <td className="px-6 tracking-[0.2em] text-[#A3E635]">
                                            {Array.from({ length: 16 }).map((_, j) => (
                                                <span key={j} className="hover:text-white hover:bg-white/20 cursor-pointer rounded-sm px-0.5">{(Math.floor(Math.random() * 256)).toString(16).padStart(2, '0').toUpperCase()}</span>
                                            ))}
                                        </td>
                                        <td className="pl-6 text-white/40 tracking-widest break-all">
                                            {Array.from({ length: 16 }).map((_item, _j) => {
                                                const charCode = Math.floor(Math.random() * 94) + 33;
                                                return String.fromCharCode(charCode);
                                            }).join('')}
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                </div>

                {/* Right Sidebar - Inspector */}
                <div className="w-[320px] flex-none bg-black/40 backdrop-blur-md flex flex-col">
                    <div className="p-4 border-b border-white/5">
                        <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest flex items-center gap-2">
                            <Binary className="w-3.5 h-3.5" /> Data Inspector
                        </span>
                    </div>

                    <div className="flex-1 overflow-y-auto p-5 space-y-6">
                        <div className="space-y-3">
                            <InspectorRow label="Int8" value="-12" />
                            <InspectorRow label="UInt8" value="244" />
                            <InspectorRow label="Int16" value="3250" />
                            <InspectorRow label="UInt16" value="3250" />
                            <InspectorRow label="Int32" value="21474836" />
                            <InspectorRow label="Float" value="3.14159f" />
                            <InspectorRow label="Double" value="1.2e-5" />
                            <InspectorRow label="Pointer" value="0x0000021A4B00000" isLink />
                        </div>

                        <div className="border-t border-white/5 pt-6 space-y-4">
                            <div className="text-[11px] font-bold text-white/30 uppercase tracking-widest flex items-center justify-between">
                                <span>Pointer Chain</span>
                                <Layers className="w-3.5 h-3.5" />
                            </div>
                            <div className="apple-glass-panel rounded-xl p-3 space-y-3">
                                <div className="flex items-center gap-2">
                                    <span className="text-[10px] text-white/40 font-mono w-4">B</span>
                                    <input type="text" className="flex-1 bg-black/40 border border-white/10 text-white font-mono text-[11px] rounded p-1" defaultValue="0x00" />
                                </div>
                                <div className="flex items-center gap-2">
                                    <span className="text-[10px] text-white/40 font-mono w-4">+</span>
                                    <input type="text" className="flex-1 bg-black/40 border border-white/10 text-white font-mono text-[11px] rounded p-1" placeholder="Offsets e.g. 1A, 20" />
                                </div>
                                <button className="w-full bg-white/5 hover:bg-white/10 border border-white/10 text-white text-[11px] py-1 rounded">Resolve</button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>

            {/* Console Bottom View */}
            <div className="h-[250px] flex-none border-t border-white/10 bg-[#000000] flex flex-col">
                <div className="h-8 border-b border-white/5 flex items-center px-4 bg-white/[0.02]">
                    <span className="text-[10px] font-bold text-white/40 uppercase tracking-widest flex items-center gap-2">
                        <Terminal className="w-3 h-3" /> UExplorer Console
                    </span>
                </div>
                <div className="flex-1 overflow-auto p-4 font-mono text-[12px] space-y-1">
                    <div className="text-white/40">UExplorer Console [Version 1.0.0]</div>
                    <div className="text-white/40 mb-4">(c) Antigravity. Connected to game process.</div>

                    <div className="flex items-center gap-2 mt-2">
                        <span className="text-primary">{'>'}</span>
                        <input type="text" className="flex-1 bg-transparent text-white outline-none" placeholder="Enter command..." />
                    </div>
                </div>
            </div>
        </div>
    );
}

function InspectorRow({ label, value, isLink }: { label: string, value: string, isLink?: boolean }) {
    return (
        <div className="flex items-center justify-between">
            <span className="text-[12px] font-medium text-white/50">{label}</span>
            <span className={`font-mono text-[12px] bg-black/40 px-2 py-0.5 rounded border border-white/5 ${isLink ? 'text-blue-400 cursor-pointer hover:underline' : 'text-white'}`}>{value}</span>
        </div>
    );
}
