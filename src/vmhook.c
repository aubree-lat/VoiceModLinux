/*
 * vmhook.dll - injected into Voicemod.exe by vmlaunch.exe (Wine has no AppInit_DLLs).
 *
 * It works around the places where Voicemod V3 depends on Windows behaviour
 * that Wine does not provide. Everything is done in memory; no Voicemod file
 * is modified on disk.
 *
 *  1. DeviceIoControl(0x1d2007): Voicemod registers an event with its virtual
 *     audio driver passing a HANDLE as a METHOD_NEITHER input buffer. Windows
 *     passes it through untouched, Wine tries to copy the buffer and fails with
 *     ERROR_NOACCESS (998). The call is answered with success here.
 *  2. IAudioSessionManager2::RegisterSessionNotification is a failing stub in
 *     Wine's mmdevapi; Voicemod aborts its audio init without it. The shared
 *     vtable is patched to return S_OK.
 *  3. VADService::checkDriverVersion compares the installed driver version
 *     (read through SetupAPI/registry, absent under Wine) with the known ones
 *     and otherwise asks to install the driver. The comparison is located by
 *     content and its two "mismatch" branches are removed.
 *  4. AudioTopology switches the audio engine to playback-only (soundboard,
 *     no microphone) unless some app is capturing from the Voicemod mic, which
 *     Voicemod learns from Windows audio sessions. Linux apps (Discord, native
 *     games) never create those, so the voice only worked on the settings page.
 *     The "capture needed" decision is patched to always be true.
 *  5. The audio SDK throws when a voice parameter value is a name instead of a
 *     number ("NoReverb"), so some voices cannot be selected. Names are turned
 *     into the values measured to sound right before they are converted.
 *  6. Optional: name resolution of the hosts listed in VMWINE_BLOCK_HOSTS
 *     (';'-separated) fails in every loaded module. vmwine uses it to block the
 *     automatic download of updates until the user approves them.
 *
 * Log: Z:\tmp\vmwine-hook.log (i.e. /tmp/vmwine-hook.log), read by vmwine.
 */
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern IMAGE_DOS_HEADER __ImageBase;

static void hooklog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    HANDLE f;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\n';
    f = CreateFileW(L"Z:\\tmp\\vmwine-hook.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, line, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
}

/* ---- import table patching ---------------------------------------------- */

/*
 * Replaces the import `name` in the IAT of the module at `base`. The first
 * original pointer seen is stored in *orig. Returns the number of slots
 * patched, or -1 if the import is present but not bound yet by the loader.
 */
static int hook_import(BYTE *base, const char *name, void *hook, void **orig)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int hits = 0;

    if (!dd->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (!imp->OriginalFirstThunk) continue;
        oft = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        ft  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData))->Name, name)) continue;
            if (ft->u1.Function == oft->u1.AddressOfData) return -1;   /* not bound yet */
            if ((void *)ft->u1.Function == hook) continue;
            if (!*orig) *orig = (void *)ft->u1.Function;
            if (VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                ft->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                hits++;
            }
        }
    }
    return hits;
}

/* ---- 1. DeviceIoControl -------------------------------------------------- */

typedef BOOL (WINAPI *DIOC)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static DIOC real_dioc;

static BOOL WINAPI my_dioc(HANDLE h, DWORD code, LPVOID in, DWORD insz, LPVOID out, DWORD outsz,
                           LPDWORD ret, LPOVERLAPPED ov)
{
    if (code == 0x1d2007) {             /* register event: the stub driver ignores it */
        if (ret) *ret = 0;
        return TRUE;
    }
    return real_dioc(h, code, in, insz, out, outsz, ret, ov);
}

/* ---- 2. IAudioSessionManager2 -------------------------------------------- */

static const GUID g_clsid_enum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID g_iid_enum   = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID g_iid_asm2   = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};

static HRESULT STDMETHODCALLTYPE my_reg_notif(IAudioSessionManager2 *self, IAudioSessionNotification *n)
{
    (void)self; (void)n;
    return S_OK;                        /* new-session notifications will simply never arrive */
}

static HRESULT STDMETHODCALLTYPE my_unreg_notif(IAudioSessionManager2 *self, IAudioSessionNotification *n)
{
    (void)self; (void)n;
    return S_OK;
}

static void patch_session_manager(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager2 *mgr = NULL;
    IAudioSessionManager2Vtbl *vt;
    DWORD old;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&g_clsid_enum, NULL, CLSCTX_ALL, &g_iid_enum, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &g_iid_asm2, CLSCTX_ALL, NULL, (void **)&mgr);
    if (FAILED(hr)) { hooklog("sessionmgr fail 0x%08lx", (unsigned long)hr); return; }

    vt = mgr->lpVtbl;
    if (!VirtualProtect(&vt->RegisterSessionNotification, 2 * sizeof(void *), PAGE_READWRITE, &old)) {
        hooklog("sessionmgr fail protect");
        return;
    }
    vt->RegisterSessionNotification   = my_reg_notif;
    vt->UnregisterSessionNotification = my_unreg_notif;
    VirtualProtect(&vt->RegisterSessionNotification, 2 * sizeof(void *), old, &old);
    hooklog("sessionmgr ok");
    /* references intentionally kept: the vtable must stay alive */
}

/* ---- 3. driver version check --------------------------------------------- */

static void nop(BYTE *at, int len)
{
    DWORD old;
    if (VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) {
        memset(at, 0x90, len);
        VirtualProtect(at, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, len);
    }
}

/*
 * The accepted versions are compared inline; the last one is "2025.10.8.0":
 *     cmp r10,0xb ; jne mismatch ; mov rcx,[rdx] ; movabs rax,"2025.10." ...
 *     test rcx,rcx ; jne mismatch
 * Both jne (0F 85 rel32) are removed so the check always passes.
 */
static void patch_version_check(void)
{
    static const BYTE pat[] = { 0x49, 0x83, 0xFA, 0x0B, 0x0F, 0x85, 0, 0, 0, 0,
                                0x48, 0x8B, 0x0A, 0x48, 0xB8, '2', '0', '2', '5', '.', '1', '0', '.' };
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD s;

    for (s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        BYTE *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - sizeof pat - 0x40;
        for (; p < end; p++) {
            unsigned i;
            BYTE *q;
            for (i = 0; i < sizeof pat; i++)
                if ((i < 6 || i > 9) && p[i] != pat[i]) break;
            if (i != sizeof pat) continue;
            for (q = p + sizeof pat; q < p + sizeof pat + 0x40; q++) {
                if (q[0] == 0x48 && q[1] == 0x85 && q[2] == 0xC9 && q[3] == 0x0F && q[4] == 0x85) {
                    nop(p + 4, 6);
                    nop(q + 3, 6);
                    hooklog("vercheck ok %p", (void *)p);
                    return;
                }
            }
            hooklog("vercheck partial %p", (void *)p);
            return;
        }
    }
    hooklog("vercheck notfound");
}

/* ---- 4. always capture the microphone -------------------------------------- */

/*
 * AudioTopology::updateCurrentTopologyInternal:
 *     need = forced || session || hearMyself || recording || vumeter
 *     test r8b,r8b ; jne 1f ; test al,al ; jne 1f ; test dl,dl ; jne 1f
 *     test r9b,r9b ; jne 1f ; test cl,cl ; jne 1f
 *     xor sil,sil ; jmp 2f ; 1: mov sil,1 ; 2:
 * "xor sil,sil" (40 32 F6) becomes "mov sil,1" (40 B6 01).
 */
static void patch_always_capture(void)
{
    /* 0x100 = any byte (jne displacement) */
    static const unsigned short pat[] = {
        0x45, 0x84, 0xC0, 0x75, 0x100, 0x84, 0xC0, 0x75, 0x100, 0x84, 0xD2, 0x75, 0x100,
        0x45, 0x84, 0xC9, 0x75, 0x100, 0x84, 0xC9, 0x75, 0x100,
        0x40, 0x32, 0xF6, 0xEB, 0x03, 0x40, 0xB6, 0x01 };
    const unsigned n = sizeof pat / sizeof pat[0];
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD s;

    for (s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        BYTE *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - n;
        for (; p < end; p++) {
            unsigned i;
            DWORD old;
            for (i = 0; i < n; i++)
                if (pat[i] != 0x100 && p[i] != pat[i]) break;
            if (i != n) continue;
            if (VirtualProtect(p + 22, 3, PAGE_EXECUTE_READWRITE, &old)) {
                p[22] = 0x40; p[23] = 0xB6; p[24] = 0x01;      /* mov sil,1 */
                VirtualProtect(p + 22, 3, old, &old);
                FlushInstructionCache(GetCurrentProcess(), p + 22, 3);
                hooklog("capture ok %p", (void *)p);
            }
            return;
        }
    }
    hooklog("capture notfound");
}

/* ---- 5. voice parameters that are not numbers ------------------------------ */

/*
 * Some voices (Clean Mic among them) fail to load under Wine with
 *     [usdk] Exception: Error during string conversion NoReverb to float
 * and the app says "voice couldn't be selected". The audio SDK reads a list
 * parameter as floats (one std::from_chars per value) and throws when a value is
 * the name of an option ("NoReverb", "NoSpace", "NaturalVoice") instead of a number.
 * NoReverb and NoSpace mean "this option on": they are read as 1. Measured on Clean
 * Mic with the same recorded speech: 1 gives the input level and no reverb, while
 * dropping the value leaves the reverb on (230 ms tail).
 * NaturalVoice is different: it mixes the unprocessed voice in. Its list is
 * "NaturalVoice, 1.0" on Clean Mic, where the voice must pass (0 silences it,
 * -45 dB), and "NaturalVoice, 1.0, 1" on changed voices like Banana, where 1 puts
 * the normal voice under the changed one (original voice at -10 dB in the output)
 * and 0 removes it (-24 dB, i.e. none). So the name is read as 1 and turned into 0
 * as soon as its list gets a third value.
 * VMHOOK_PARAM_VALUES="NoReverb=0,NaturalVoice=1,..." forces the value per name
 * (a forced NaturalVoice is left alone).
 *
 * The converter is found through its error string and replaced (fallback); its
 * only caller, the function that appends one value to the parameter's list, is
 * detoured so that a name is turned into its number before the conversion.
 */
typedef struct { const char *ptr; int ec; } fc_result;
typedef fc_result (*FROM_CHARS)(const char *, const char *, float *, int);
static FROM_CHARS real_from_chars;

/* MSVC std::string: inline buffer (or pointer) at +0, size at +0x10, capacity at +0x18 */
static const char *str_ptr(const void *s, size_t *len)
{
    const BYTE *str = (const BYTE *)s;
    size_t cap = *(const size_t *)(str + 0x18);
    *len = *(const size_t *)(str + 0x10);
    return cap >= 16 ? *(const char * const *)str : (const char *)str;
}

/* value for an option name: from VMHOOK_PARAM_VALUES, else "1" */
static const char *name_value(const char *p, size_t len)
{
    static char map[256], val[16];
    static int loaded;
    char *e;
    if (!loaded) {
        loaded = 1;
        DWORD n = GetEnvironmentVariableA("VMHOOK_PARAM_VALUES", map, sizeof map);
        if (!n || n >= sizeof map) map[0] = 0;
    }
    for (e = map; *e; ) {
        char *eq = strchr(e, '='), *comma = strchr(e, ',');
        if (!comma) comma = e + strlen(e);
        if (eq && eq < comma && (size_t)(eq - e) == len && !memcmp(e, p, len)) {
            size_t n = comma - eq - 1 < 15 ? comma - eq - 1 : 15;
            memcpy(val, eq + 1, n); val[n] = 0;
            return val;
        }
        e = *comma ? comma + 1 : comma;
    }
    return "1";
}

static float tolerant_stof(const void *s)
{
    size_t len;
    const char *p = str_ptr(s, &len);
    float v = 0.0f;
    fc_result r = real_from_chars(p, p + len, &v, 3);   /* chars_format::general */
    if (r.ec) {
        const char *n = name_value(p, len);
        real_from_chars(n, n + strlen(n), &v, 3);
    }
    return v;
}

static BYTE *find_bytes(const BYTE *pat, size_t n, int exec)
{
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD s;

    for (s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        BYTE *p, *end;
        if (!!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != exec) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - n;
        for (; p < end; p++)
            if (p[0] == pat[0] && !memcmp(p, pat, n)) return p;
    }
    return NULL;
}

typedef void (*LIST_ADD)(void *param, const void *token);
static LIST_ADD real_list_add;   /* trampoline: the original prologue + jmp back */

/* the parameter's values: std::vector<float> at +0xb8 (begin) / +0xc0 (end) */
static float *param_values(void *param, size_t *n)
{
    float *b = *(float **)((BYTE *)param + 0xb8), *e = *(float **)((BYTE *)param + 0xc0);
    *n = e - b;
    return b;
}

static void tolerant_list_add(void *param, const void *token)
{
    static void *natural;   /* list that started with NaturalVoice (not forced) */
    size_t len, n;
    const char *p = str_ptr(token, &len), *v;
    float f, *vals;
    struct { char buf[16]; size_t size, cap; } s;   /* small-string layout, capacity 15 */

    if (getenv("VMHOOK_DEBUG")) hooklog("list %p '%.*s'", param, (int)(len < 64 ? len : 64), p);
    if (!real_from_chars(p, p + len, &f, 3).ec) {
        if (param != natural) natural = NULL;   /* a list's values arrive in a row */
        real_list_add(param, token);
        if (param == natural && (vals = param_values(param, &n)) && n == 3) {
            vals[0] = 0.0f;
            hooklog("param 'NaturalVoice' -> 0 (changed voice)");
        }
        return;
    }
    v = name_value(p, len);
    natural = len == 12 && !memcmp(p, "NaturalVoice", 12) && !strcmp(v, "1") ? param : NULL;
    {
        static int logged;
        if (logged++ < 20) hooklog("param '%.*s' -> %s", (int)(len < 64 ? len : 64), p, v);
    }
    memset(&s, 0, sizeof s);
    s.size = strlen(v); memcpy(s.buf, v, s.size); s.cap = 15;
    real_list_add(param, &s);
}

static void patch_list_add(BYTE *stof)
{
    /* push rbp,r15,r14,r12,rsi,rdi,rbx ; sub rsp,imm32  (17 bytes, no rip-relative operands) */
    static const BYTE prologue[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC };
    enum { PLEN = sizeof prologue + 4 };
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *call = NULL, *fn = NULL, *q, *tramp;
    void *target;
    WORD s;
    DWORD old;

    for (s = 0; s < nt->FileHeader.NumberOfSections && !call; s++, sec++) {
        BYTE *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - 5;
        for (; p < end; p++)
            if (p[0] == 0xE8 && p + 5 + *(INT32 *)(p + 1) == stof) { call = p; break; }
    }
    if (!call) { hooklog("listadd nocall"); return; }
    for (q = call; q > call - 0x100; q--)
        if (q[-1] == 0xCC && !memcmp(q, prologue, sizeof prologue)) { fn = q; break; }
    if (!fn) { hooklog("listadd nostart %p", (void *)call); return; }

    tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { hooklog("listadd noalloc"); return; }
    memcpy(tramp, fn, PLEN);
    tramp[PLEN] = 0xFF; tramp[PLEN + 1] = 0x25; memset(tramp + PLEN + 2, 0, 4);
    target = fn + PLEN;
    memcpy(tramp + PLEN + 6, &target, 8);
    real_list_add = (LIST_ADD)tramp;

    if (VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old)) {
        target = (void *)tolerant_list_add;
        fn[0] = 0xFF; fn[1] = 0x25; memset(fn + 2, 0, 4);
        memcpy(fn + 6, &target, 8);
        VirtualProtect(fn, 14, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 14);
        hooklog("listadd ok %p", (void *)fn);
    }
}

static void patch_tolerant_stof(void)
{
    static const char msg[] = "Error during string conversion ";
    static const BYTE prologue[] = { 0x55, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC };   /* push rbp,rsi,rdi,rbx ; sub rsp */
    static const BYTE fmt3[] = { 0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00 };  /* mov dword [rsp+20h],3 */
    BYTE *str = find_bytes((const BYTE *)msg, sizeof msg - 1, 0);
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *lea = NULL, *fn = NULL, *q;
    WORD s;
    DWORD old;

    if (!str) { hooklog("stof nostring"); return; }
    /* lea rdx,[rip+rel32] pointing at the message */
    for (s = 0; s < nt->FileHeader.NumberOfSections && !lea; s++, sec++) {
        BYTE *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        p = base + sec->VirtualAddress;
        end = p + sec->Misc.VirtualSize - 7;
        for (; p < end; p++)
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && p + 7 + *(INT32 *)(p + 3) == str) { lea = p; break; }
    }
    if (!lea) { hooklog("stof noref"); return; }
    /* start of the function: int3 padding followed by the known prologue */
    for (q = lea; q > lea - 0x200; q--)
        if (q[-1] == 0xCC && !memcmp(q, prologue, sizeof prologue)) { fn = q; break; }
    if (!fn) { hooklog("stof nostart %p", (void *)lea); return; }
    for (q = fn; q < lea; q++)
        if (!memcmp(q, fmt3, sizeof fmt3)) {
            BYTE *c;
            for (c = q + sizeof fmt3; c < q + sizeof fmt3 + 12; c++)
                if (c[0] == 0xE8) { real_from_chars = (FROM_CHARS)(c + 5 + *(INT32 *)(c + 1)); break; }
            break;
        }
    if (!real_from_chars) { hooklog("stof nofromchars %p", (void *)fn); return; }
    patch_list_add(fn);
    /* jmp [rip+0] ; dq tolerant_stof */
    if (VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old)) {
        void *target = (void *)tolerant_stof;
        fn[0] = 0xFF; fn[1] = 0x25; memset(fn + 2, 0, 4);
        memcpy(fn + 6, &target, 8);
        VirtualProtect(fn, 14, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 14);
        hooklog("stof ok %p", (void *)fn);
    }
}

/* ---- 6. blocking update downloads ---------------------------------------- */

static char g_block[512];

static int host_blocked(const char *h)
{
    const char *p = g_block;
    if (!h || !*p) return 0;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && strlen(h) == n && !_strnicmp(h, p, n)) return 1;
        p += n + (e ? 1 : 0);
    }
    return 0;
}

static int host_blocked_w(const WCHAR *h)
{
    char a[256];
    int i;
    if (!h) return 0;
    for (i = 0; i < 255 && h[i]; i++) a[i] = h[i] < 128 ? (char)h[i] : '?';
    a[i] = 0;
    return host_blocked(a);
}

typedef INT (WSAAPI *GAI_A)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
typedef INT (WSAAPI *GAI_W)(PCWSTR, PCWSTR, const ADDRINFOW *, PADDRINFOW *);
typedef INT (WSAAPI *GAI_EXW)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW *, PADDRINFOEXW *,
                              struct timeval *, LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
typedef HINTERNET (WINAPI *WHC)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
static GAI_A real_gai_a;
static GAI_W real_gai_w;
static GAI_EXW real_gai_exw;
static WHC real_whc;

static INT WSAAPI my_gai_a(PCSTR node, PCSTR svc, const ADDRINFOA *hints, PADDRINFOA *res)
{
    if (host_blocked(node)) { hooklog("blocked %s", node); WSASetLastError(WSAHOST_NOT_FOUND); return WSAHOST_NOT_FOUND; }
    return real_gai_a(node, svc, hints, res);
}

static INT WSAAPI my_gai_w(PCWSTR node, PCWSTR svc, const ADDRINFOW *hints, PADDRINFOW *res)
{
    if (host_blocked_w(node)) { hooklog("blocked %ls", node); WSASetLastError(WSAHOST_NOT_FOUND); return WSAHOST_NOT_FOUND; }
    return real_gai_w(node, svc, hints, res);
}

static INT WSAAPI my_gai_exw(PCWSTR node, PCWSTR svc, DWORD ns, LPGUID id, const ADDRINFOEXW *hints,
                             PADDRINFOEXW *res, struct timeval *tv, LPOVERLAPPED ov,
                             LPLOOKUPSERVICE_COMPLETION_ROUTINE cb, LPHANDLE h)
{
    if (host_blocked_w(node)) { hooklog("blocked %ls", node); WSASetLastError(WSAHOST_NOT_FOUND); return WSAHOST_NOT_FOUND; }
    return real_gai_exw(node, svc, ns, id, hints, res, tv, ov, cb, h);
}

static HINTERNET WINAPI my_whc(HINTERNET s, LPCWSTR server, INTERNET_PORT port, DWORD r)
{
    if (host_blocked_w(server)) { hooklog("blocked %ls", server); SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED); return NULL; }
    return real_whc(s, server, port, r);
}

static void block_hosts(void)
{
    static HMODULE mods[512];
    DWORD need, i, n;
    int hits = 0;

    if (!g_block[0] || !K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    n = need / sizeof(HMODULE);
    if (n > 512) n = 512;
    for (i = 0; i < n; i++) {
        BYTE *b = (BYTE *)mods[i];
        int r;
        if (mods[i] == (HMODULE)&__ImageBase) continue;
        if ((r = hook_import(b, "getaddrinfo",    (void *)my_gai_a,   (void **)&real_gai_a))   > 0) hits += r;
        if ((r = hook_import(b, "GetAddrInfoW",   (void *)my_gai_w,   (void **)&real_gai_w))   > 0) hits += r;
        if ((r = hook_import(b, "GetAddrInfoExW", (void *)my_gai_exw, (void **)&real_gai_exw)) > 0) hits += r;
        if ((r = hook_import(b, "WinHttpConnect", (void *)my_whc,     (void **)&real_whc))     > 0) hits += r;
    }
    hooklog("blockhosts %d slots in %lu modules", hits, (unsigned long)n);
}

/* ---- 7. hiding the engine's own window ------------------------------------ */

/*
 * With VMWINE_HIDE_QT=1 the UI is shown by a native Electron window instead, so
 * Voicemod's own Qt window (blank under Wine) is hidden. It must be hidden, not
 * closed: WM_CLOSE would quit Voicemod. Qt shows it again during startup and when
 * a voicemod:// link arrives, so this keeps hiding it.
 * Idea and approach from AriCuteGirl/voicemod-linux (MIT).
 */
static BOOL CALLBACK hide_main_window(HWND hwnd, LPARAM unused)
{
    WCHAR title[32];
    DWORD pid;
    (void)unused;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    if (!GetWindowTextW(hwnd, title, 32) || lstrcmpW(title, L"VoicemodV3")) return TRUE;
    ShowWindow(hwnd, SW_HIDE);
    return TRUE;
}

static DWORD WINAPI hide_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(250);
        EnumWindows(hide_main_window, 0);
    }
    return 0;
}

/* ---- startup -------------------------------------------------------------- */

static DWORD WINAPI patch_thread(LPVOID arg)
{
    int i, r = -1;
    (void)arg;

    /* the main module's imports may not be bound yet when we are loaded */
    for (i = 0; i < 6000 && r < 0; i++) {
        r = hook_import((BYTE *)GetModuleHandleW(NULL), "DeviceIoControl", (void *)my_dioc, (void **)&real_dioc);
        if (r < 0) Sleep(5);
    }
    hooklog("deviceiocontrol %s", r > 0 ? "ok" : "fail");

    patch_session_manager();
    patch_version_check();
    patch_always_capture();
    patch_tolerant_stof();

    if (GetEnvironmentVariableA("VMWINE_BLOCK_HOSTS", g_block, sizeof g_block) >= sizeof g_block)
        g_block[0] = 0;
    block_hosts();
    Sleep(15000);                       /* modules loaded later (Qt6Network, winhttp, ...) */
    block_hosts();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR path[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        DisableThreadLibraryCalls(inst);
        /* only the main Voicemod process, not QtWebEngineProcess & co. */
        if (n > 12 && !lstrcmpiW(path + n - 12, L"Voicemod.exe")) {
            char hide[4];
            hooklog("attach");
            CloseHandle(CreateThread(NULL, 0, patch_thread, NULL, 0, NULL));
            if (GetEnvironmentVariableA("VMWINE_HIDE_QT", hide, sizeof hide) && hide[0] == '1')
                CloseHandle(CreateThread(NULL, 0, hide_thread, NULL, 0, NULL));
        }
    }
    return TRUE;
}
