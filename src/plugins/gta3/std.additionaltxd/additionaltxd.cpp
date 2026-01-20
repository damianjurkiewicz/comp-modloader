/*
 * Copyright (C) 2026  Damix maddjurr@gmail.com
 * Licensed under the MIT License, see LICENSE at top level directory.
 *
 * std.additionaltxd -- Additional TXD dictionaries (fastloader*)
 *
 * Ported from plugin-sdk based implementation (src-additionaltxd.cpp).
 */

#include <stdinc.hpp>

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include <cstdio>  // vsnprintf
#include <cstdarg> // va_list

using namespace modloader;

// --- Minimal RenderWare forward declarations ---
struct RwTexDictionary;
struct RwTexture;

// 1. Forward Declaration
class additionaltxd;
extern additionaltxd additionaltxd_plugin;

namespace additionaltxd_impl
{
    // 2. LogF Declaration
    void LogF(const char* fmt, ...);

    static const char* AdditionalTxdListSharedName()
    {
        return "AdditionalTxdListGet";
    }

    using AdditionalTxdList = std::vector<std::string>;
    using AdditionalTxdListGetter = const AdditionalTxdList* (*)();

    // GTA SA 1.0 US addresses
    static constexpr uintptr_t kCall_FindNamedTexture = 0x731733;
    static constexpr uintptr_t kCall_AssignRemapTxd = 0x5B62C2;

    // Game functions
    static constexpr uintptr_t kFn_CTxdStore_AddRefByName = 0x731C80;
    static constexpr uintptr_t kFn_CTxdStore_GetTxdDictionary = 0x408340;
    static constexpr uintptr_t kFn_CStreaming_RequestTxdModel = 0x407100;
    static constexpr uintptr_t kFn_CStreaming_LoadAllRequestedModels = 0x40EA10;

    using fnFindNamedTex = RwTexture * (__cdecl*)(RwTexDictionary*, const char*);
    using fnAssignRemap = void(__cdecl*)(const char*, uint16_t);

    using fnAddRefByName = int(__cdecl*)(char*);
    using fnGetTxdDict = RwTexDictionary * (__cdecl*)(int);
    using fnRequestTxd = void(__cdecl*)(int, int);
    using fnLoadAllReq = void(__cdecl*)(bool);

    static fnFindNamedTex g_ogFindNamedTex = nullptr;
    static fnAssignRemap  g_ogAssignRemap = nullptr;

    static std::vector<uint16_t>          g_txdIds;
    static std::vector<RwTexDictionary*>  g_extraDicts;
    static AdditionalTxdList              g_additionalTxdFiles;

    // Renamed variable from g_hasFastloader to g_hasVehicleLoader for clarity
    static bool g_hasVehicleLoader = false;
    static bool g_hasAdditional = false;
    static bool g_loaded = false;
    static bool g_dirty = false;

    static std::string ToLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    static fnAddRefByName AddRefByName()
    {
        return reinterpret_cast<fnAddRefByName>(kFn_CTxdStore_AddRefByName);
    }

    static fnGetTxdDict GetTxdDictionary()
    {
        return reinterpret_cast<fnGetTxdDict>(kFn_CTxdStore_GetTxdDictionary);
    }

    static fnRequestTxd RequestTxdModel()
    {
        return reinterpret_cast<fnRequestTxd>(kFn_CStreaming_RequestTxdModel);
    }

    static fnLoadAllReq LoadAllRequestedModels()
    {
        return reinterpret_cast<fnLoadAllReq>(kFn_CStreaming_LoadAllRequestedModels);
    }

    // --- CHANGE: New function checking for 'vehicle' name and limit up to 5 ---
    static bool IsVehicleLoaderName(const char* txdName)
    {
        if (!txdName) return false;

        // 1. Check if the name starts with "vehicle" (7 characters)
        if (std::strncmp(txdName, "vehicle", 7) != 0)
            return false;

        // Pointer to the part after the word "vehicle"
        const char* suffix = txdName + 7;

        // 2. [CHANGE] Skip "vehicle.txd" (i.e., just the name "vehicle" without a number)
        if (*suffix == '\0')
        {
            // We intentionally return false here to ignore pure 'vehicle.txd'
            return false;
        }

        // 3. Check numbering (limit to 5)
        // Must be a digit for us to consider it
        if (std::isdigit(static_cast<unsigned char>(*suffix)))
        {
            int number = std::atoi(suffix);

            // LIMIT: only vehicle1 through vehicle5
            if (number >= 1 && number <= 5)
            {
                LogF("Detected 'vehicle' prefix with ID %d: %s", number, txdName);
                return true;
            }
        }

        return false;
    }

    // Function to synchronize the list with traits (from loader.txt)
    static void SyncAdditionalTxdFiles()
    {
        // Retrieve data from shared memory (where traits save data from loader.txt)
        AdditionalTxdList new_list;
        if (modloader_shdata_t* data = modloader::plugin_ptr->loader->FindSharedData(AdditionalTxdListSharedName()))
        {
            if (data->type == MODLOADER_SHDATA_FUNCTION)
            {
                auto getter = reinterpret_cast<AdditionalTxdListGetter>(data->f);
                if (const AdditionalTxdList* list = getter())
                    new_list = *list;
            }
        }

        // If the list has changed, update it
        if (new_list != g_additionalTxdFiles)
        {
            LogF("SyncAdditionalTxdFiles: Update detected! Old size: %d, New size: %d",
                (int)g_additionalTxdFiles.size(), (int)new_list.size());

            for (auto& entry : new_list) {
                // Normalize names (lowercase)
                entry = modloader::NormalizePath(entry);
                entry = ToLower(entry);
                LogF(" - Config Entry: '%s'", entry.c_str());
            }

            g_additionalTxdFiles = std::move(new_list);
            g_hasAdditional = !g_additionalTxdFiles.empty();
            g_dirty = true; // Mark that cache needs to be reloaded
        }
    }

    static bool IsAdditionalName(const char* txdName)
    {
        if (!txdName) return false;

        std::string normalized = modloader::NormalizePath(txdName);
        std::string base = modloader::GetPathComponentBack(normalized);
        auto dot = base.rfind('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);

        base = ToLower(base);

        for (const auto& entry : g_additionalTxdFiles)
        {
            if (entry == base)
            {
                LogF("Match found for additional TXD: '%s' (matches config entry: '%s')", txdName, entry.c_str());
                return true;
            }
        }
        return false;
    }

    static void RebuildCache()
    {
        // Variable updated
        if (!g_hasVehicleLoader && !g_hasAdditional)
            return;

        LogF("RebuildCache: Reloading %d stored TXD IDs...", (int)g_txdIds.size());
        g_extraDicts.clear();

        for (uint16_t txdId : g_txdIds)
            RequestTxdModel()(static_cast<int>(txdId), 2);

        LoadAllRequestedModels()(true);

        for (uint16_t txdId : g_txdIds)
        {
            RwTexDictionary* dict = GetTxdDictionary()(static_cast<int>(txdId));
            if (dict) g_extraDicts.push_back(dict);
            else LogF("RebuildCache: Warning! Failed to get dictionary for TXD ID %d", txdId);
        }

        g_loaded = !g_extraDicts.empty();
        g_dirty = false;
        LogF("RebuildCache: Cache rebuilt. Extra dicts count: %d", (int)g_extraDicts.size());
    }

    static void EnsureLoaded()
    {
        // Variable updated
        if (!g_hasVehicleLoader && !g_hasAdditional) return;

        if (g_dirty || !g_loaded)
        {
            LogF("EnsureLoaded: Dirty flag set or not loaded. Rebuilding...");
            RebuildCache();
        }
    }

    static void PreloadOne(uint16_t txdId)
    {
        LogF("PreloadOne: Requesting TXD ID %d", txdId);
        RequestTxdModel()(static_cast<int>(txdId), 2);
        LoadAllRequestedModels()(true);

        RwTexDictionary* dict = GetTxdDictionary()(static_cast<int>(txdId));
        if (dict)
        {
            if (std::find(g_extraDicts.begin(), g_extraDicts.end(), dict) == g_extraDicts.end())
            {
                g_extraDicts.push_back(dict);
                LogF("PreloadOne: Dictionary loaded and added to cache.");
            }
            g_loaded = true;
        }
    }

    static RwTexture* __cdecl hkRwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const char* name)
    {
        RwTexture* tex = g_ogFindNamedTex(dict, name);
        if (tex) return tex;

        // Variable updated
        if (!g_hasVehicleLoader && !g_hasAdditional) return nullptr;

        EnsureLoaded();

        for (RwTexDictionary* extraDict : g_extraDicts)
        {
            if (!extraDict) continue;
            tex = g_ogFindNamedTex(extraDict, name);
            if (tex)
            {
                LogF("Texture found in extra dict! Name: '%s'", name);
                return tex;
            }
        }
        return nullptr;
    }

    static void __cdecl hkAssignRemapTxd(const char* txdName, uint16_t txdId)
    {
        // Force list check from config
        SyncAdditionalTxdFiles();

        g_ogAssignRemap(txdName, txdId);

        // --- CHANGE: Using the new IsVehicleLoaderName function ---
        const bool is_vehicle_loader = IsVehicleLoaderName(txdName);
        const bool is_additional = IsAdditionalName(txdName);

        if (!is_vehicle_loader && !is_additional)
            return;

        LogF("Hook AssignRemap: '%s' (ID: %d) -> Identified as Vehicle/Additional", txdName, txdId);

        const bool isNew = (std::find(g_txdIds.begin(), g_txdIds.end(), txdId) == g_txdIds.end());
        if (isNew)
        {
            g_txdIds.push_back(txdId);
            g_dirty = true;
            LogF(" -> New ID added to list. Marking dirty.");
        }

        AddRefByName()(const_cast<char*>(txdName));

        // Flag updated
        g_hasVehicleLoader = g_hasVehicleLoader || is_vehicle_loader;
        g_hasAdditional = g_hasAdditional || is_additional;

        PreloadOne(txdId);
    }

    static bool PatchCall(uintptr_t callSite, void* newTarget, void** outOriginal)
    {
        auto* p = reinterpret_cast<uint8_t*>(callSite);
        if (*p != 0xE8) return false;

        int32_t oldRel = *reinterpret_cast<int32_t*>(p + 1);
        uintptr_t oldTarget = callSite + 5u + static_cast<uintptr_t>(oldRel);
        if (outOriginal) *outOriginal = reinterpret_cast<void*>(oldTarget);

        uintptr_t newT = reinterpret_cast<uintptr_t>(newTarget);
        int32_t newRel = static_cast<int32_t>(newT - (callSite + 5u));

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(callSite), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        *p = 0xE8;
        *reinterpret_cast<int32_t*>(p + 1) = newRel;

        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void*>(callSite), 5, oldProtect, &tmp);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(callSite), 5);
        return true;
    }

    static bool Hook()
    {
        LogF("Installing hooks...");
        bool ok1 = PatchCall(kCall_FindNamedTexture,
            reinterpret_cast<void*>(&hkRwTexDictionaryFindNamedTexture),
            reinterpret_cast<void**>(&g_ogFindNamedTex));

        bool ok2 = PatchCall(kCall_AssignRemapTxd,
            reinterpret_cast<void*>(&hkAssignRemapTxd),
            reinterpret_cast<void**>(&g_ogAssignRemap));

        return (ok1 && ok2);
    }
}

// 3. Class Definition
class additionaltxd : public modloader::basic_plugin
{
public:
    const info& GetInfo();
    bool OnStartup();
    bool OnShutdown();
    int GetBehaviour(modloader::file&);
    bool InstallFile(const modloader::file&);
    bool ReinstallFile(const modloader::file&);
    bool UninstallFile(const modloader::file&);
    void Update() override;

} additionaltxd_plugin;

REGISTER_ML_PLUGIN(::additionaltxd_plugin);

// 4. LogF Implementation
void additionaltxd_impl::LogF(const char* fmt, ...)
{
    char buffer[2048];
    int offset = snprintf(buffer, sizeof(buffer), "STD: ");
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
    va_end(args);
    additionaltxd_plugin.Log(buffer);
}

// --- Plugin Methods ---

const additionaltxd::info& additionaltxd::GetInfo()
{
    static const char* extable[] = { 0 };
    static const info xinfo = { "std.additionaltxd", get_version_by_date(), "LINK/2012 + port+fix", -1, extable };
    return xinfo;
}

bool additionaltxd::OnStartup()
{
    additionaltxd_impl::LogF("OnStartup called.");
    if (additionaltxd_impl::Hook())
        additionaltxd_impl::LogF("Hooks installed.");
    else
        additionaltxd_impl::LogF("Failed to install hooks!");
    return true;
}

bool additionaltxd::OnShutdown() { return true; }

void additionaltxd::Update()
{
    // Sync here as well, in case something changed on the fly (hot reload)
    additionaltxd_impl::SyncAdditionalTxdFiles();
}

int additionaltxd::GetBehaviour(modloader::file&) { return MODLOADER_BEHAVIOUR_NO; }
bool additionaltxd::InstallFile(const modloader::file&) { return true; }
bool additionaltxd::ReinstallFile(const modloader::file&) { return true; }
bool additionaltxd::UninstallFile(const modloader::file&) { return true; }