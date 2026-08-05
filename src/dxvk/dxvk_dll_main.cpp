// ---------------------------------------------------------------------------
// VEGAS: process-lifecycle hook — DllMain
//
// DXVK 2.x defines no DllMain anywhere; the Vegas session tracker (crash
// marker + session report) therefore never ran its cleanup on a normal
// process exit:
//   - the DxvkDevice dtor is skipped during module detachment
//     (this_thread::isInModuleDetachment(), dxvk_device.cpp)
//   - games that ExitProcess without releasing COM never run the dtor
// Result: the crash marker stayed stale after every normal quit, so the
// next session reported a false crash.
//
// This DllMain(PROCESS_DETACH) fires on normal process exit (Wine runs
// DLL detach notifications on ExitProcess; TerminateProcess / unhandled
// exceptions do not), giving Vegas a reliable clean-exit hook. The file
// is part of the dxvk core, so every VEGAS DLL (d3d9/d3d10core/d3d11/dxgi)
// carries it. endSession() is idempotent (s_sessionActive guard), so only
// the DLL that actually owns the active session performs cleanup, exactly
// once.
// ---------------------------------------------------------------------------

#include <windows.h>

#include "dxvk_vegas.h"

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_DETACH:
      // Clean-exit path: write the session report and delete the crash
      // marker. No-op when no session is active (e.g. DLL loaded but
      // never used by this process, or the device was already torn down).
      dxvk::Vegas::endSession();
      break;

    default:
      break;
  }
  return TRUE;
}
