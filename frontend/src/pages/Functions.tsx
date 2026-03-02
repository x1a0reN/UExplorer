import { useState } from 'react';
import { Search, Filter, TerminalSquare, Play, Info, List, History, Power } from 'lucide-react';

export default function Functions() {
    const [activeTab, setActiveTab] = useState('Call');

    const functionTabs = [
        { id: 'Info', icon: Info },
        { id: 'Parameters', icon: List },
        { id: 'Call', icon: Play },
        { id: 'Hook', icon: Power },
        { id: 'Decompile', icon: TerminalSquare },
    ];

    return (
        <div className="flex-1 flex overflow-hidden">
            {/* Left Sidebar - Function List (Xcode Style) */}
            <div className="w-[320px] flex-none border-r border-white/5 flex flex-col bg-black/40 relative z-10 backdrop-blur-md">

                {/* Search Header */}
                <div className="p-4 border-b border-white/5 space-y-3">
                    <div className="relative group">
                        <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
                        <input
                            type="text"
                            placeholder="ClassName::FunctionName..."
                            className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-mono placeholder:text-white/30"
                        />
                    </div>

                    <div className="flex items-center justify-between">
                        <div className="flex gap-1 bg-white/5 p-1 rounded-[8px] border border-white/5">
                            <button className="px-2.5 py-1 rounded-[6px] bg-white/10 text-white text-[11px] font-semibold tracking-tight shadow-sm">All</button>
                            <button className="px-2.5 py-1 rounded-[6px] text-white/50 hover:text-white text-[11px] font-semibold tracking-tight transition-colors">Native</button>
                            <button className="px-2.5 py-1 rounded-[6px] text-white/50 hover:text-white text-[11px] font-semibold tracking-tight transition-colors">Blueprint</button>
                        </div>
                        <button className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/50 hover:text-white transition-colors">
                            <Filter className="w-3.5 h-3.5" />
                        </button>
                    </div>
                </div>

                {/* Virtual List Area */}
                <div className="flex-1 overflow-y-auto p-2 space-y-1 custom-scrollbar">
                    {/* Dummy Items to show style */}
                    {[...Array(20)].map((_, i) => (
                        <div key={i} className={`flex items-center gap-3 px-3 py-2 rounded-[10px] cursor-pointer transition-colors ${i === 0 ? 'bg-primary text-white shadow-sm' : 'hover:bg-white/5 text-white/70'}`}>
                            <div className={`w-7 h-7 rounded-[8px] flex items-center justify-center ${i === 0 ? 'bg-white/10 border-white/20' : 'bg-white/5'} border border-transparent flex-none`}>
                                <TerminalSquare className={`w-3.5 h-3.5 ${i === 0 ? 'text-white' : 'text-blue-400'}`} />
                            </div>
                            <div className="flex-1 min-w-0">
                                <div className={`text-[12px] font-mono truncate ${i === 0 ? 'text-white' : 'text-white/90'}`}>GetGameName</div>
                                <div className={`text-[10px] uppercase font-bold tracking-widest truncate ${i === 0 ? 'text-white/70' : 'text-white/30'}`}>GameEngine</div>
                            </div>
                        </div>
                    ))}
                </div>
            </div>

            {/* Right Content Area - Details */}
            <div className="flex-1 flex flex-col min-w-0 bg-[#0A0A0C] relative">
                {/* Top Apple Tab Bar */}
                <div className="h-14 border-b border-white/5 bg-white/[0.02] backdrop-blur-3xl flex items-center px-6 gap-6 z-20">
                    <nav className="flex items-center gap-4">
                        {functionTabs.map(tab => (
                            <button
                                key={tab.id}
                                onClick={() => setActiveTab(tab.id)}
                                className={`relative h-14 flex items-center gap-2 text-[13px] font-semibold tracking-tight transition-colors ${activeTab === tab.id ? 'text-white' : 'text-white/40 hover:text-white/70'}`}
                            >
                                <tab.icon className="w-4 h-4" />
                                {tab.id}
                                {activeTab === tab.id && (
                                    <div className="absolute bottom-0 left-0 right-0 h-[3px] bg-primary rounded-t-full shadow-[0_-2px_8px_rgba(10,132,255,0.5)]"></div>
                                )}
                            </button>
                        ))}
                    </nav>
                </div>

                {/* Content View */}
                <div className="flex-1 overflow-auto p-8 relative">
                    <div className="max-w-4xl space-y-6">

                        {/* Hero Info Box */}
                        <div className="apple-glass-panel rounded-[24px] p-6 relative overflow-hidden">
                            <div className="flex gap-5 relative z-10 items-center">
                                <div className="w-16 h-16 rounded-[14px] bg-gradient-to-br from-green-500/20 to-teal-500/10 border border-white/10 flex items-center justify-center shadow-lg flex-none">
                                    <TerminalSquare className="w-8 h-8 text-green-400 stroke-[1.5]" />
                                </div>
                                <div className="flex-1">
                                    <h1 className="text-[20px] font-mono text-white tracking-tight leading-tight mb-2">GameEngine::GetGameName</h1>

                                    <div className="flex flex-wrap gap-2">
                                        <span className="px-2.5 py-1 rounded-[6px] bg-blue-500/10 border border-blue-500/20 text-[11px] font-bold tracking-widest uppercase text-blue-400">Native</span>
                                        <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">Param Size: 0x10</span>
                                        <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">Flags: FUNC_Static | FUNC_Native</span>
                                    </div>
                                </div>
                            </div>
                        </div>

                        {/* Function Invocation Area */}
                        {activeTab === 'Call' && (
                            <div className="grid grid-cols-2 gap-6">
                                {/* Left Col: Setup */}
                                <div className="space-y-6">
                                    <div className="apple-glass-panel rounded-[24px] overflow-hidden p-6">
                                        <h3 className="text-[14px] font-semibold text-white/90 mb-4 tracking-tight">Invoke Parameters</h3>

                                        <div className="space-y-4">
                                            {/* Target Object (if not static) */}
                                            <div className="space-y-1.5">
                                                <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest">Target Object Address</label>
                                                <input
                                                    type="text"
                                                    placeholder="0x00..."
                                                    defaultValue="0x00000000000"
                                                    className="w-full bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary/50 transition-colors"
                                                />
                                            </div>

                                            {/* Param 1 */}
                                            <div className="space-y-1.5">
                                                <label className="text-[11px] font-bold text-white/40 uppercase tracking-widest flex items-center justify-between">
                                                    <span>PlayerIndex</span>
                                                    <span className="text-blue-400 lowercase font-mono">int32</span>
                                                </label>
                                                <input
                                                    type="text"
                                                    placeholder="0"
                                                    className="w-full bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-2 outline-none focus:border-primary/50 transition-colors"
                                                />
                                            </div>

                                            <button className="w-full py-2.5 rounded-lg bg-primary hover:bg-primary-dark text-white font-semibold text-[13px] tracking-tight shadow-md active:scale-[0.98] transition-all flex items-center justify-center gap-2 mt-4">
                                                <Play className="w-4 h-4 fill-current" />
                                                Execute ProcessEvent
                                            </button>
                                        </div>
                                    </div>
                                </div>

                                {/* Right Col: Result */}
                                <div className="space-y-6">
                                    <div className="apple-glass-panel rounded-[24px] overflow-hidden p-6 flex flex-col h-full">
                                        <div className="flex items-center justify-between mb-4">
                                            <h3 className="text-[14px] font-semibold text-white/90 tracking-tight">Execution Result</h3>
                                            <History className="w-4 h-4 text-white/40" />
                                        </div>

                                        <div className="flex-1 bg-black/50 border border-white/5 rounded-xl p-4 font-mono text-[13px] text-green-400 overflow-y-auto">
                                            <div className="text-white/30 text-[11px] mb-2">/* Return Value (FString) */</div>
                                            "Wandering_Sword"
                                        </div>
                                    </div>
                                </div>
                            </div>
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
}
