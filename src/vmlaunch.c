/*
 * vmlaunch.exe - starts Voicemod with vmhook.dll injected.
 *
 * Wine does not implement AppInit_DLLs, so the app is created suspended,
 * vmhook.dll is loaded into it with CreateRemoteThread(LoadLibraryW) and the
 * app is resumed. The app is started directly (app\last\Voicemod.exe
 * --launcher, as Voicemod's own launcher does): the launcher is skipped on
 * purpose because it would also self-update Voicemod.
 *
 * Exit codes: 0 ok, 2 no LOCALAPPDATA, 3 CreateProcess failed, 4/5 injection failed.
 */
#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    static const WCHAR hook[] = L"C:\\windows\\system32\\vmhook.dll";
    WCHAR local[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], cl[1024];
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    LPTHREAD_START_ROUTINE load;
    LPVOID remote;
    HANDLE th;
    (void)inst; (void)prev; (void)show;

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return 2;
    wsprintfW(dir, L"%s\\VoicemodV3\\app\\last", local);
    wsprintfW(exe, L"%s\\Voicemod.exe", dir);
    /* Voicemod exits on unknown arguments. Default: --launcher. A voicemod:// link (login
     * callback) can be passed instead: a second instance forwards it to the running one. */
    if (cmd && cmd[0] && wcslen(cmd) < 400)
        wsprintfW(cl, L"\"%s\" %s", exe, cmd);
    else
        wsprintfW(cl, L"\"%s\" --launcher", exe);

    if (!CreateProcessW(exe, cl, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) return 3;

    load = (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    remote = VirtualAllocEx(pi.hProcess, NULL, sizeof hook, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, hook, sizeof hook, NULL)) {
        TerminateProcess(pi.hProcess, 4);
        return 4;
    }
    th = CreateRemoteThread(pi.hProcess, NULL, 0, load, remote, 0, NULL);
    if (!th) {
        TerminateProcess(pi.hProcess, 5);
        return 5;
    }
    WaitForSingleObject(th, 10000);
    ResumeThread(pi.hThread);
    CloseHandle(th);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
