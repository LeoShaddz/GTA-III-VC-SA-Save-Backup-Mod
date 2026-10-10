// SaveTools.asi - GTA III / Vice City / San Andreas
// Part 1: duplicate the save after saving (optional, via .ini).
// Works with files only (does not modify game memory), so it supports all three games.
//
// How it works: monitors save files (sf1.b ... sf8.b). When one of them changes and
// finishes being written, it copies the file to the backup slot specified in the .ini.
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "shell32.lib")

#define NUM_SLOTS 8

static HMODULE hSelf = NULL;
static char gIni[MAX_PATH];
static char gFolder[MAX_PATH];
static char gPattern[32];   // e.g., "GTASAsf%d.b"
static int  gLogEnabled = 0; // [Main] Log setting in the .ini

// ---------------- Logging ----------------
static void Log(const char* fmt, ...)
{
    if (!gLogEnabled) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hSelf, path, MAX_PATH)) return;
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log"); else strcat(path, ".log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// ---------------- Save slots ----------------
struct SlotInfo { bool exists; FILETIME t; DWORD size; };

static void SlotPath(int slot, char* out)
{
    char name[64];
    sprintf(name, gPattern, slot);
    sprintf(out, "%s\\%s", gFolder, name);
}

static bool Query(int slot, SlotInfo& si)
{
    char p[MAX_PATH];
    SlotPath(slot, p);
    WIN32_FILE_ATTRIBUTE_DATA d;
    ZeroMemory(&si, sizeof si);
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d)) return false;
    si.exists = true;
    si.t = d.ftLastWriteTime;
    si.size = d.nFileSizeLow;
    return true;
}

static bool Same(const SlotInfo& a, const SlotInfo& b)
{
    return a.exists == b.exists && a.size == b.size && CompareFileTime(&a.t, &b.t) == 0;
}

static bool DirExists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------- Game detection / save folder ----------------
static bool DetectSaveFolder()
{
    struct G { const char* dir; const char* pattern; };
    const G sa = { "GTA San Andreas User Files", "GTASAsf%d.b" };
    const G vc = { "GTA Vice City User Files",   "GTAVCsf%d.b" };
    const G g3 = { "GTA3 User Files",            "GTA3sf%d.b"  };

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    _strlwr(exe);
    const char* base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;

    const G* game = NULL;
    if (strstr(base, "gta_sa") || strstr(base, "gta-sa")) game = &sa;
    else if (strstr(base, "vc"))                          game = &vc;
    else if (strstr(base, "gta3"))                        game = &g3;

    char docs[MAX_PATH] = "";
    SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs);

    if (!game)
    {
        // Unknown executable name: use the game whose save folder exists
        const G* all[3] = { &sa, &vc, &g3 };
        for (int i = 0; i < 3 && !game; i++)
        {
            char p[MAX_PATH];
            if (GetPrivateProfileIntA("Main", "Portable", 0, gIni))
            {
                // Portable mode: look for a save file for this game in <root>\userfiles
                char root[MAX_PATH];
                GetModuleFileNameA(NULL, root, MAX_PATH);
                char* sl = strrchr(root, '\\');
                if (sl) *sl = 0;
                for (int n = 1; n <= NUM_SLOTS && !game; n++)
                {
                    char name[64];
                    sprintf(name, all[i]->pattern, n);
                    sprintf(p, "%s\\userfiles\\%s", root, name);
                    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) game = all[i];
                }
            }
            else
            {
                sprintf(p, "%s\\%s", docs, all[i]->dir);
                if (DirExists(p)) game = all[i];
            }
        }
    }
    if (!game) { Log("Game could not be identified (exe: %s)", base); return false; }

    strcpy(gPattern, game->pattern);
    if (GetPrivateProfileIntA("Main", "Portable", 0, gIni))
    {
        // 1 = "userfiles" folder in the game root (the folder containing the .exe)
        char root[MAX_PATH];
        GetModuleFileNameA(NULL, root, MAX_PATH);
        char* slash = strrchr(root, '\\');
        if (slash) *slash = 0;
        sprintf(gFolder, "%s\\userfiles", root);
    }
    else
    {
        // 0 = default folder: Documents\GTA ... User Files
        sprintf(gFolder, "%s\\%s", docs, game->dir);
    }

    if (!DirExists(gFolder)) { Log("Save folder does not exist: %s", gFolder); return false; }
    Log("Save folder: %s (pattern %s)", gFolder, gPattern);
    return true;
}

// ---------------- Save duplication ----------------
static bool WaitStable(int slot)
{
    // Wait until the file stops changing (two consecutive identical readings)
    SlotInfo prev, cur;
    Query(slot, prev);
    for (int i = 0; i < 20; i++)
    {
        Sleep(500);
        Query(slot, cur);
        if (cur.exists && Same(prev, cur)) return true;
        prev = cur;
    }
    return false;
}

static int PickDestination(int savedSlot, int backupSlot)
{
    if (backupSlot >= 1 && backupSlot <= NUM_SLOTS) return backupSlot; // Fixed destination
    // 0 = last available slot (highest-numbered slot without a save)
    for (int s = NUM_SLOTS; s >= 1; s--)
    {
        if (s == savedSlot) continue;
        SlotInfo si;
        Query(s, si);
        if (!si.exists) return s;
    }
    return 0;
}

static void Duplicate(int src, int dst)
{
    char ps[MAX_PATH], pd[MAX_PATH];
    SlotPath(src, ps);
    SlotPath(dst, pd);

    if (!CopyFileA(ps, pd, FALSE)) { Log("Failed to copy slot %d -> %d (error %lu)", src, dst, GetLastError()); return; }

    // Preserve the original file's timestamp (so the backup does not become the "most recent" save)
    SlotInfo si;
    Query(src, si);
    HANDLE h = CreateFileA(pd, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, NULL, NULL, &si.t); CloseHandle(h); }

    Log("Save from slot %d duplicated to slot %d", src, dst);
}

static DWORD WINAPI Thread(LPVOID)
{
    if (!GetPrivateProfileIntA("Duplicate", "Enable", 0, gIni)) { Log("Duplicate disabled in .ini"); return 0; }
    int backup = GetPrivateProfileIntA("Duplicate", "BackupSlot", 8, gIni);

    // The game may take some time to create the folder; retry for a while
    bool ok = false;
    for (int i = 0; i < 60 && !ok; i++) { ok = DetectSaveFolder(); if (!ok) Sleep(5000); }
    if (!ok) return 0;

    SlotInfo snap[NUM_SLOTS + 1];
    for (int s = 1; s <= NUM_SLOTS; s++) Query(s, snap[s]);

    Log("Monitoring save files. BackupSlot=%d", backup);
    while (true)
    {
        Sleep(1000);
        for (int s = 1; s <= NUM_SLOTS; s++)
        {
            SlotInfo cur;
            Query(s, cur);
            if (Same(cur, snap[s])) continue;

            if (cur.exists && s != backup)
            {
                if (WaitStable(s))
                {
                    int dst = PickDestination(s, backup);
                    if (dst && dst != s) Duplicate(s, dst);
                    else Log("No destination slot available for save in slot %d", s);
                    if (dst) Query(dst, snap[dst]);
                }
            }
            Query(s, snap[s]);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        hSelf = hModule;
        GetModuleFileNameA(hModule, gIni, MAX_PATH);
        char* dot = strrchr(gIni, '.');
        if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");
        gLogEnabled = GetPrivateProfileIntA("Main", "Log", 0, gIni);
        CreateThread(0, 0, Thread, NULL, 0, NULL);
    }
    return TRUE;
}
