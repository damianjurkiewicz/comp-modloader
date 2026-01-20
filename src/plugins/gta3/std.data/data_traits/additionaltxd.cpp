/*
 * Copyright (C) 2026  Damix maddjurr@gmail.com
 * Licensed under the MIT License, see LICENSE at top level directory.
 *
 */
#include <stdinc.hpp>
#include "../data_traits.hpp"
#include <unordered_set>
#include <cstdio>
#include <cstdarg>

using namespace modloader;
using AdditionalTxdList = std::vector<std::string>;

// --- Helper for logging with the "traits" prefix ---
static void LogTraits(const char* fmt, ...)
{
    if (!modloader::plugin_ptr) return;

    char buffer[2048];
    // Add a constant "traits: " prefix
    int offset = snprintf(buffer, sizeof(buffer), "traits: ");

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
    va_end(args);

    modloader::plugin_ptr->Log(buffer);
}

static const char* AdditionalTxdListSharedName()
{
    return "AdditionalTxdListGet";
}

// Structures must be globally visible for RTTI
struct additionaltxd_traits : public data_traits
{
    struct dtraits : modloader::dtraits::OpenFile
    {
        static const char* what() { return "additional txd"; }

        // FIX: Changed from a variable to a static function
        // data.hpp expects a call to datafile() to calculate the hash
        static const char* datafile() { return "additionaltxd"; }
    };
};

struct additionaltxd_store
{
    using traits_type = additionaltxd_traits;
    std::vector<std::string> entries;

    template<class Archive>
    void serialize(Archive& archive)
    {
        archive(entries);
    }
};

// RTTI registration in global scope
REGISTER_RTTI_FOR_ANY(additionaltxd_store);

static modloader_shdata_t* g_additional_txd_shdata = nullptr;
static AdditionalTxdList g_additional_txd_list;

static const AdditionalTxdList* GetAdditionalTxdList()
{
    LogTraits("GetAdditionalTxdList: Requesting list refresh...");
    g_additional_txd_list.clear();
    std::unordered_set<std::string> seen;

    auto readme_data = modloader::plugin_ptr->cast<DataPlugin>().QueryReadmeData<additionaltxd_store>();

    size_t count = 0;
    for (const auto& entry : readme_data)
    {
        const auto& store = entry.second.second.get();
        for (const auto& name : store.entries)
        {
            if (seen.emplace(name).second)
            {
                g_additional_txd_list.emplace_back(name);
                LogTraits("GetAdditionalTxdList: Added entry '%s'", name.c_str());
                count++;
            }
            else
            {
                LogTraits("GetAdditionalTxdList: Skipped duplicate '%s'", name.c_str());
            }
        }
    }

    LogTraits("GetAdditionalTxdList: Finished. Total entries: %zu", count);
    return &g_additional_txd_list;
}

static void CreateAdditionalTxdSharedData()
{
    if (g_additional_txd_shdata)
        return;

    LogTraits("CreateAdditionalTxdSharedData: Creating shared data interface...");
    g_additional_txd_shdata = modloader::plugin_ptr->loader->CreateSharedData(AdditionalTxdListSharedName());
    if (g_additional_txd_shdata)
    {
        g_additional_txd_shdata->type = MODLOADER_SHDATA_FUNCTION;
        g_additional_txd_shdata->f = reinterpret_cast<void*>(&GetAdditionalTxdList);
        LogTraits("CreateAdditionalTxdSharedData: Success.");
    }
    else
    {
        LogTraits("CreateAdditionalTxdSharedData: Failed to create shared data!");
    }
}

static std::string ExtractTxdName(const std::string& path)
{
    std::string normalized = modloader::NormalizePath(path);
    std::string base = modloader::GetPathComponentBack(normalized);
    auto dot = base.rfind('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);

    // LogTraits("ExtractTxdName: Converted '%s' -> '%s'", path.c_str(), base.c_str());
    return base;
}

void ShutdownAdditionalTxdSharedData()
{
    if (g_additional_txd_shdata)
    {
        LogTraits("ShutdownAdditionalTxdSharedData: Cleaning up.");
        modloader::plugin_ptr->loader->DeleteSharedData(g_additional_txd_shdata);
        g_additional_txd_shdata = nullptr;
    }
}

static auto xinit = initializer([](DataPlugin* plugin_ptr)
    {
        LogTraits("Initializer: Starting setup for additionaltxd traits...");
        CreateAdditionalTxdSharedData();

        plugin_ptr->AddReader<additionaltxd_store>([](const std::string& line) -> maybe_readable<additionaltxd_store>
            {
                // Optional: log every line (note: might be spammy)
                // LogTraits("Reader: processing line: '%s'", line.c_str());

                if (!modloader::starts_with(line.c_str(), "ATXD", false))
                    return nothing;

                std::string tag;
                std::string rest;
                std::stringstream stream(line);
                if (!(stream >> tag))
                    return nothing;

                std::getline(stream, rest);
                modloader::trim(rest);
                if (rest.empty())
                {
                    LogTraits("Reader: Warning - Found ATXD tag but no value.");
                    return nothing;
                }

                std::string name = ExtractTxdName(rest);
                if (name.empty())
                {
                    LogTraits("Reader: Warning - Failed to extract TXD name from '%s'", rest.c_str());
                    return nothing;
                }

                LogTraits("Reader: Parsed config line. Path: '%s' -> Name: '%s'", rest.c_str(), name.c_str());

                additionaltxd_store store;
                store.entries.emplace_back(std::move(name));
                return store;
            });

        LogTraits("Initializer: Setup complete.");
    });