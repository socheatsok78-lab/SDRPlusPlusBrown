#include <config.h>
#include <utils/flog.h>
#include <fstream>

#include <filesystem>
#include "utils/wstr.h"
#include "utils/usleep.h"
#include "core.h"

ConfigManager::ConfigManager() {
}

ConfigManager::~ConfigManager() {
    disableAutoSave();
}

void ConfigManager::setPath(std::string file) {
    path = std::filesystem::absolute(file).string();
}

void ConfigManager::load(json def, bool lock) {
    if (lock) { mtx.lock(); }
    if (path == "") {
        flog::error("Config manager tried to load file with no path specified");
        return;
    }
    if (!std::filesystem::exists(path)) {
        flog::warn("Config file '{0}' does not exist, creating it", path);
        conf = def;
        save(false);
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("Config file '{0}' isn't a file", path);
        return;
    }

    auto filesize = std::filesystem::file_size(path);
    try {
        std::ifstream file(wstr::str2wstr(path));
        file >> conf;
        file.close();
    }
    catch (const std::exception e) {
        flog::error("Config file '{}' with size={} is corrupted ({}), resetting it", path, (int64_t)filesize, e.what());
        usleep(3000000);
        conf = def;
        save(false);
    }

    transformPaths(conf, true);

    if (lock) { mtx.unlock(); }
}

void ConfigManager::save(bool lock) {
    if (lock) { mtx.lock(); }
    auto justpath = wstr::str2wstr(path);
    auto newpath = wstr::str2wstr(path + ".new");
    std::ofstream file(newpath);
    auto tmp_conf = conf;
    transformPaths(tmp_conf, false);
    file << tmp_conf.dump(4);
    file.close();
    std::filesystem::rename(newpath, justpath);
    if (lock) { mtx.unlock(); }
}

void ConfigManager::enableAutoSave() {
    if (autoSaveEnabled) { return; }
    autoSaveEnabled = true;
    termFlag = false;
    autoSaveThread = std::thread(&ConfigManager::autoSaveWorker, this);
}

void ConfigManager::disableAutoSave() {
    if (!autoSaveEnabled) { return; }
    {
        std::lock_guard<std::mutex> lock(termMtx);
        autoSaveEnabled = false;
        termFlag = true;
    }
    termCond.notify_one();
    if (autoSaveThread.joinable()) { autoSaveThread.join(); }
}

void ConfigManager::acquire() {
    mtx.lock();
}

void ConfigManager::release(bool modified) {
    changed |= modified;
    mtx.unlock();
}

void ConfigManager::autoSaveWorker() {
    while (autoSaveEnabled) {
        if (!mtx.try_lock()) {
            flog::warn("ConfigManager locked, waiting...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (changed) {
            changed = false;
            save(false);
        }
        mtx.unlock();

        SetThreadName("autosave");
        // Sleep but listen for wakeup call
        {
            std::unique_lock<std::mutex> lock(termMtx);
            termCond.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return termFlag; });
        }
    }
}

void ConfigManager::transformPaths(json& conf, bool expand) {
    const char* env_prefix = std::getenv("SDRPP_PREFIX");
    const std::string prefix = env_prefix ? env_prefix : INSTALL_PREFIX;

    std::vector<std::string> directoryKeys = {
        "modulesDirectory",
        "resourcesDirectory"
    };

    const std::string from = expand ? "@prefix@" : prefix;
    const std::string to = expand ? prefix : "@prefix@";

    for (const auto& key : directoryKeys) {
        if (conf.contains(key)) {
            auto dir = conf[key].get<std::string>();

            size_t pos = dir.find(from);
            if (pos != std::string::npos) {
                dir.replace(pos, from.length(), to);
                conf[key] = dir;
            }
        }
    }
}
