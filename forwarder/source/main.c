#include <switch.h>
#include <string.h>

// Sentinel written into g_nextArgv before handing control to the NRO.
// If it's still there when loadNro() is called again, the NRO exited cleanly.
#define EXIT_DETECTION_STR "if this isn't replaced i will exit :)"

static char g_argv[2048]              = {0};
static char g_nextArgv[2048]          = {0};
static char g_nextNroPath[FS_MAX_PATH] = {0};
static char g_defaultArgv[2048]       = {0};
static char g_defaultNroPath[FS_MAX_PATH] = {0};

static const char g_noticeText[] = {
    "nx-hbloader " VERSION
    "\0"
    "Do you mean to tell me that you're thinking seriously of building that way, when and if you are an architect?"
};

static u64      g_nroSize = 0;
static NroHeader g_nroHeader = {0};

static enum {
    CodeMemoryUnavailable    = 0,
    CodeMemoryForeignProcess = BIT(0),
    CodeMemorySameProcess    = BIT(0) | BIT(1),
} g_codeMemoryCapability = CodeMemoryUnavailable;

static void*  g_heapAddr = {0};
static size_t g_heapSize = {0};

static Handle g_procHandle   = {0};
static u128   g_userIdStorage = {0};
static u8     g_savedTls[0x100] = {0};

// Used by trampoline.s
u64    g_nroAddr = 0;
Result g_lastRet = 0;

void NX_NORETURN nroEntrypointTrampoline(const ConfigEntry* entries, u64 handle, u64 entrypoint);

// Strip the "sdmc:" prefix that hbmenu may prefix paths with,
// since fsFsOpenFile doesn't accept it.
static void fix_nro_path(char* path) {
    if (!strncmp(path, "sdmc:/", 6))
        memmove(path, path + 5, FS_MAX_PATH - 5);
}

// Send the AM "Exit" command via appletOE so the OS returns to the home menu
// cleanly. Falls back to diagAbortWithResult if any IPC step fails.
// Credit: HookedBehemoth (https://github.com/HookedBehemoth/nx-hbloader)
static void NX_NORETURN selfExit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        goto fail0;

    Service applet, proxy, self;

    rc = smGetService(&applet, "appletOE");
    if (R_FAILED(rc))
        goto fail1;

    const u32 cmd_id  = 0;
    const u64 reserved = 0;

    // GetSessionProxy (cmd 0): provides pid + own process handle, receives IApplicationProxy
    rc = serviceDispatchIn(&applet, cmd_id, reserved,
        .in_send_pid    = true,
        .in_num_handles = 1,
        .in_handles     = { g_procHandle },
        .out_num_objects = 1,
        .out_objects    = &proxy,
    );
    if (R_FAILED(rc))
        goto fail2;

    // GetSelfController (cmd 1)
    rc = serviceDispatch(&proxy, 1,
        .out_num_objects = 1,
        .out_objects     = &self,
    );
    if (R_FAILED(rc))
        goto fail3;

    // Exit (cmd 0) — signals AM to return to home menu
    rc = serviceDispatch(&self, 0);
    serviceClose(&self);

fail3: serviceClose(&proxy);
fail2: serviceClose(&applet);
fail1: smExit();
fail0:
    if (R_SUCCEEDED(rc)) {
        // OS processes the Exit request and terminates us; sleep until it does.
        while (1) svcSleepThread(86400000000000ULL);
        svcExitProcess();
        __builtin_unreachable();
    } else {
        diagAbortWithResult(rc);
    }
}

static u64 calculateMaxHeapSize(void) {
    u64 size = 0;
    u64 mem_available = 0, mem_used = 0;

    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used,      InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

    if (mem_available > mem_used + 0x200000)
        size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
        size = 0x2000000 * 16;
    if (size > 0x6000000)
        size -= 0x6000000;

    return size;
}

static void setupHbHeap(void) {
    void* addr = NULL;
    u64 size = calculateMaxHeapSize();
    Result rc = svcSetHeapSize(&addr, size);

    if (R_FAILED(rc) || addr == NULL)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 9));

    g_heapAddr = addr;
    g_heapSize = size;
}

static void procHandleReceiveThread(void* arg) {
    Handle session = (Handle)(uintptr_t)arg;
    Result rc;

    void* base = armGetTls();
    hipcMakeRequestInline(base);

    s32 idx = 0;
    rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles != 1)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 17));

    g_procHandle = r.data.copy_handles[0];
    svcCloseHandle(session);
}

static void getOwnProcessHandle(void) {
    Result rc;

    Handle server_handle, client_handle;
    rc = svcCreateSession(&server_handle, &client_handle, 0, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 12));

    Thread t;
    u8* stack = g_heapAddr;
    rc = threadCreate(&t, &procHandleReceiveThread, (void*)(uintptr_t)server_handle, stack, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 10));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(),
        .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;

    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);

    threadWaitForExit(&t);
    threadClose(&t);
}

static bool isKernel5xOrLater(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_UserExceptionContextAddress, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static bool isKernel4x(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_InitialProcessIdRange, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static void getCodeMemoryCapability(void) {
    if (detectMesosphere()) {
        g_codeMemoryCapability = CodeMemorySameProcess;
    } else if (isKernel5xOrLater()) {
        Handle code;
        Result rc = svcCreateCodeMemory(&code, g_heapAddr, 0x1000);
        if (R_SUCCEEDED(rc)) {
            rc = svcControlCodeMemory(code, (CodeMapOperation)-1, 0, 0x1000, 0);
            svcCloseHandle(code);

            if (R_VALUE(rc) == KERNELRESULT(InvalidEnumValue))
                g_codeMemoryCapability = CodeMemorySameProcess;
            else
                g_codeMemoryCapability = CodeMemoryForeignProcess;
        }
    } else if (isKernel4x()) {
        g_codeMemoryCapability = CodeMemorySameProcess;
    }
}

void NX_NORETURN loadNro(void) {
    NroHeader* header = NULL;
    size_t rw_size    = 0;
    Result rc         = 0;

    memcpy((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    // Exit detection: if g_nextArgv still contains our sentinel, the NRO exited
    // cleanly without requesting a chain-load via envSetNextLoad().
    if (!strcmp(g_nextArgv, EXIT_DETECTION_STR)) {
        if (!strcmp(g_nextNroPath, g_defaultNroPath)) {
            selfExit();
        } else {
            // NRO requested a different target; now return to default NRO
            strcpy(g_nextNroPath, g_defaultNroPath);
            strcpy(g_nextArgv,    g_defaultArgv);
        }
    }

    if (g_nroSize) {
        // Unmap previously loaded NRO
        header = &g_nroHeader;
        rw_size = header->segments[2].size + header->bss_size;
        rw_size = (rw_size + 0xFFF) & ~0xFFF;

        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreUnloadDll, g_nroAddr, g_nroSize);

        rc = svcUnmapProcessCodeMemory(g_procHandle,
            g_nroAddr + header->segments[0].file_off,
            (u64)g_heapAddr + header->segments[0].file_off,
            header->segments[0].size);
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 24));

        rc = svcUnmapProcessCodeMemory(g_procHandle,
            g_nroAddr + header->segments[1].file_off,
            (u64)g_heapAddr + header->segments[1].file_off,
            header->segments[1].size);
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 25));

        rc = svcUnmapProcessCodeMemory(g_procHandle,
            g_nroAddr + header->segments[2].file_off,
            (u64)g_heapAddr + header->segments[2].file_off,
            rw_size);
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 26));

        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostUnloadDll, g_nroAddr, g_nroSize);

        g_nroAddr = g_nroSize = 0;
    } else {
        // First launch: read NRO path and argv from the romfs embedded in this NSP.
        // Files are sorted alphabetically: nextArgv first, then nextNroPath.
        FsStorage s;
        romfs_header romfs_hdr;

        if (R_FAILED(rc = fsOpenDataStorageByCurrentProcess(&s)))
            diagAbortWithResult(rc);
        if (R_FAILED(rc = fsStorageRead(&s, 0, &romfs_hdr, sizeof(romfs_hdr))))
            diagAbortWithResult(rc);

        u8 romfs_dirs[1024 * 2];
        u8 romfs_files[1024 * 4];

        if (romfs_hdr.dirTableSize  > sizeof(romfs_dirs) ||
            romfs_hdr.fileTableSize > sizeof(romfs_files))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_OutOfMemory));

        if (R_FAILED(rc = fsStorageRead(&s, romfs_hdr.dirTableOff,  romfs_dirs,  romfs_hdr.dirTableSize)))
            diagAbortWithResult(rc);
        if (R_FAILED(rc = fsStorageRead(&s, romfs_hdr.fileTableOff, romfs_files, romfs_hdr.fileTableSize)))
            diagAbortWithResult(rc);

        const romfs_dir*  dir            = (const romfs_dir*)romfs_dirs;
        const romfs_file* next_argv_file = (const romfs_file*)(romfs_files + dir->childFile);
        const romfs_file* next_nro_file  = (const romfs_file*)(romfs_files + next_argv_file->sibling);

        if (R_FAILED(rc = fsStorageRead(&s, romfs_hdr.fileDataOff + next_argv_file->dataOff,
                g_nextArgv, next_argv_file->dataSize)))
            diagAbortWithResult(rc);
        if (R_FAILED(rc = fsStorageRead(&s, romfs_hdr.fileDataOff + next_nro_file->dataOff,
                g_nextNroPath, next_nro_file->dataSize)))
            diagAbortWithResult(rc);

        fsStorageClose(&s);

        // Trim trailing newline if the romfs file was saved as a text file
        char* nl;
        if ((nl = strchr(g_nextNroPath, '\n')) != NULL) *nl = '\0';
        if ((nl = strchr(g_nextArgv,    '\n')) != NULL) *nl = '\0';

        strcpy(g_defaultNroPath, g_nextNroPath);
        strcpy(g_defaultArgv,    g_nextArgv);
    }

    {
        char fixedNextNroPath[FS_MAX_PATH];
        strcpy(fixedNextNroPath, g_nextNroPath);
        fix_nro_path(fixedNextNroPath);

        memcpy(g_argv, g_nextArgv, sizeof(g_argv));
        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreLoadDll, (uintptr_t)g_argv, sizeof(g_argv));

        uint8_t*  nrobuf = (uint8_t*)g_heapAddr;
        NroStart* start  = (NroStart*)(nrobuf + 0);
        header           = (NroHeader*)(nrobuf + sizeof(NroStart));

        FsFileSystem fs;
        if (R_FAILED(rc = fsOpenSdCardFileSystem(&fs)))
            diagAbortWithResult(rc);

        FsFile f;
        if (R_FAILED(rc = fsFsOpenFile(&fs, fixedNextNroPath, FsOpenMode_Read, &f)))
            diagAbortWithResult(rc);

        u64 bytes_read;
        if (R_FAILED(rc = fsFileRead(&f, 0, start, g_heapSize, FsReadOption_None, &bytes_read)) ||
                header->magic != NROHEADER_MAGIC ||
                bytes_read < sizeof(*start) + sizeof(*header) + header->size)
            diagAbortWithResult(rc);

        fsFileClose(&f);
        fsFsClose(&fs);
    }

    rw_size = header->segments[2].size + header->bss_size;
    rw_size = (rw_size + 0xFFF) & ~0xFFF;

    for (int i = 0; i < 3; i++) {
        if (header->segments[i].file_off >= header->size ||
                header->segments[i].size > header->size ||
                (header->segments[i].file_off + header->segments[i].size) > header->size)
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
    }

    memcpy(&g_nroHeader, header, sizeof(g_nroHeader));
    header = &g_nroHeader;

    virtmemLock();
    const size_t total_size = (header->size + header->bss_size + 0xFFF) & ~0xFFF;
    void* map_addr = virtmemFindCodeMemory(total_size, 0);
    rc = svcMapProcessCodeMemory(g_procHandle, (u64)map_addr, (u64)g_heapAddr, total_size);
    virtmemUnlock();

    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 18));

    rc = svcSetProcessMemoryPermission(g_procHandle,
        (u64)map_addr + header->segments[0].file_off, header->segments[0].size, Perm_R | Perm_X);
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    rc = svcSetProcessMemoryPermission(g_procHandle,
        (u64)map_addr + header->segments[1].file_off, header->segments[1].size, Perm_R);
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    rc = svcSetProcessMemoryPermission(g_procHandle,
        (u64)map_addr + header->segments[2].file_off, rw_size, Perm_Rw);
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    const u64 nro_size       = header->segments[2].file_off + rw_size;
    const u64 nro_heap_start = (u64)g_heapAddr + nro_size;
    const u64 nro_heap_size  = g_heapSize + (u64)g_heapAddr - nro_heap_start;

#define M EntryFlag_IsMandatory

    static ConfigEntry entries[] = {
        { EntryType_MainThreadHandle,      0, {0, 0} },
        { EntryType_ProcessHandle,         0, {0, 0} },
        { EntryType_AppletType,            0, {AppletType_SystemApplication, EnvAppletFlags_ApplicationOverride} },
        { EntryType_OverrideHeap,          M, {0, 0} },
        { EntryType_Argv,                  0, {0, 0} },
        { EntryType_NextLoadPath,          0, {0, 0} },
        { EntryType_LastLoadResult,        0, {0, 0} },
        { EntryType_SyscallAvailableHint,  0, {UINT64_MAX, UINT64_MAX} },
        { EntryType_SyscallAvailableHint2, 0, {UINT64_MAX, 0} },
        { EntryType_RandomSeed,            0, {0, 0} },
        { EntryType_UserIdStorage,         0, {(u64)(uintptr_t)&g_userIdStorage, 0} },
        { EntryType_HosVersion,            0, {0, 0} },
        { EntryType_EndOfList,             0, {(u64)(uintptr_t)g_noticeText, sizeof(g_noticeText)} }
    };

    ConfigEntry* entry_Syscalls = &entries[7];

    if (!(g_codeMemoryCapability & BIT(0)))
        entry_Syscalls->Value[0x4B / 64] &= ~(1UL << (0x4B % 64));
    if (!(g_codeMemoryCapability & BIT(1)))
        entry_Syscalls->Value[0x4C / 64] &= ~(1UL << (0x4C % 64));

    entries[0].Value[0]  = envGetMainThreadHandle();
    entries[1].Value[0]  = g_procHandle;
    entries[3].Value[0]  = nro_heap_start;
    entries[3].Value[1]  = nro_heap_size;
    entries[4].Value[1]  = (u64)(uintptr_t)&g_argv[0];
    entries[5].Value[0]  = (u64)(uintptr_t)&g_nextNroPath[0];
    entries[5].Value[1]  = (u64)(uintptr_t)&g_nextArgv[0];
    entries[6].Value[0]  = g_lastRet;
    entries[9].Value[0]  = randomGet64();
    entries[9].Value[1]  = randomGet64();
    entries[11].Value[0] = hosversionGet();
    entries[11].Value[1] = hosversionIsAtmosphere() ? 0x41544d4f53504852UL : 0;

    g_nroAddr = (u64)map_addr;
    g_nroSize = nro_size;

    svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostLoadDll, g_nroAddr, g_nroSize);

    // Write exit-detection sentinel into g_nextArgv before handing control to the NRO.
    // If the NRO exits without chain-loading, this string will still be here on re-entry.
    strcpy(g_nextArgv, EXIT_DETECTION_STR);
    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);
}

int main(int argc, char** argv) {
    memcpy(g_savedTls, (const u8*)armGetTls() + 0x100, 0x100);
    setupHbHeap();
    getOwnProcessHandle();
    getCodeMemoryCapability();
    loadNro();
}

// libnx overrides
u32  __nx_applet_type             = AppletType_Application;
u32  __nx_fs_num_sessions         = 1;
u32  __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd       = false;

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = NULL;
    fake_heap_end   = NULL;
}

void __appInit(void) {
    Result rc;

    Handle dummy;
    rc = svcConnectToNamedPort(&dummy, "ams");
    u32 ams_flag = (R_VALUE(rc) != KERNELRESULT(NotFound)) ? BIT(31) : 0;
    if (R_SUCCEEDED(rc))
        svcCloseHandle(dummy);

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 1));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(ams_flag | MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 2));

    smExit();
}

void __appExit(void) {}

void __wrap_exit(void) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 39));
}

void* __libnx_alloc(size_t size) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 40));
}

void* __libnx_aligned_alloc(size_t alignment, size_t size) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 41));
}

void __libnx_free(void* p) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 43));
}
