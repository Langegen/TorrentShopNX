#pragma once

// UI stall watchdog.
//
// The UI thread calls beat() once per frame. A separate thread watches the
// beat; when a frame takes longer than ~0.5 s it pauses the UI thread for a
// few microseconds, copies its registers and the top of its stack, resumes it
// and writes one "[uistall]" line per sample to log.txt:
//
//   [uistall] gap=1200ms pc=0x1a2b3c lr=0x1a2000 stack=0x1b0000,0x1c0040,...
//
// Addresses are offsets into the NRO's code, i.e. addresses in
// TorrentShopNX.elf; scripts/symbolize_uistall.ps1 turns them into function
// names with addr2line. On non-Switch builds everything is a no-op.

namespace uiwd {

void start();   // once, from main() before the main loop
void beat();    // once per frame, from the UI thread
void stop();    // before shutdown

// Writes one "[uistall] gap=0ms <tag> ..." line with the calling thread's
// stack (same format, same symbolize script). For fatal paths like terminate.
void logCurrentStack(const char* tag);

} // namespace uiwd
