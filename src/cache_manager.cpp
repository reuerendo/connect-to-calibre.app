#include "cache_manager.h"
#include <json-c/json.h>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <unistd.h> // Для fsync, unlink, rename

// Оптимизация логгера: добавлен fflush и проверка указателя
#define LOG_CACHE(fmt, ...) { \
    FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a"); \
    if(f) { \
        fprintf(f, "[CACHE] " fmt "\n", ##__VA_ARGS__); \
        fflush(f); \
        fclose(f); \
    } \
}

CacheManager::CacheManager() {
}

CacheManager::~CacheManager() {
}

bool CacheManager::initialize(const std::string& deviceUuid) {
    if (deviceUuid.empty()) {
        LOG_CACHE("Cannot initialize: empty device UUID");
        return false;
    }
    
    this->deviceUuid = deviceUuid;
    // Формируем путь. Можно вынести базовый путь в константу.
    cacheFilePath = "/mnt/ext1/system/calibre_cache_" + deviceUuid + ".json";
    
    LOG_CACHE("Initialized cache for device: %s", deviceUuid.c_str());
    
    return loadCache();
}

std::string CacheManager::getCurrentTimestamp() const {
    time_t now = time(NULL);
    struct tm* tm_info = gmtime(&now);
    char buffer[32];
    // Оптимизация: использование фиксированного формата быстрее чем сложный strftime, но оставим для надежности
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", tm_info);
    return std::string(buffer);
}

time_t CacheManager::parseTimestamp(const std::string& isoTime) const {
    if (isoTime.empty()) return 0;
    
    struct tm tm = {0};
    int y, m, d, H, M, S;
    
    // sscanf достаточно быстр
    if (sscanf(isoTime.c_str(), "%d-%d-%dT%d:%d:%d", 
               &y, &m, &d, &H, &M, &S) >= 6) {
        tm.tm_year = y - 1900;
        tm.tm_mon = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = H;
        tm.tm_min = M;
        tm.tm_sec = S;
        return timegm(&tm);
    }
    
    return 0;
}

bool CacheManager::loadCache() {
    FILE* f = fopen(cacheFilePath.c_str(), "r");
    if (!f) {
        LOG_CACHE("Cache file not found, starting fresh");
        return true;
    }
    
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize <= 0 || fileSize > 50 * 1024 * 1024) { // 50MB limit
        fclose(f);
        LOG_CACHE("Invalid cache file size: %ld", fileSize);
        return true;
    }
    
    std::vector<char> buffer(fileSize + 1);
    size_t read = fread(buffer.data(), 1, fileSize, f);
    fclose(f);
    
    if (read != (size_t)fileSize) {
        LOG_CACHE("Failed to read cache file completely");
        return false;
    }
    
    buffer[fileSize] = '\0';
    
    json_object* root = json_tokener_parse(buffer.data());
    if (!root) {
        LOG_CACHE("Failed to parse cache JSON");
        return false;
    }
    
    int loaded = 0;
    
    json_object_object_foreach(root, key, val) {
        (void)key;
        
        json_object* bookObj = NULL;
        json_object* lastUsedObj = NULL;
        
        if (!json_object_object_get_ex(val, "book", &bookObj) ||
            !json_object_object_get_ex(val, "last_used", &lastUsedObj)) {
            continue;
        }
        
        BookMetadata metadata;
        json_object* tmp = NULL;
        
        auto getString = [&](const char* field) -> std::string {
            if (json_object_object_get_ex(bookObj, field, &tmp)) {
                const char* str = json_object_get_string(tmp);
                return str ? std::string(str) : "";
            }
            return "";
        };
        
        metadata.uuid = getString("uuid");
        metadata.title = getString("title");
        metadata.authors = getString("authors");
        metadata.lpath = getString("lpath");
        metadata.lastModified = getString("last_modified");
        
        if (json_object_object_get_ex(bookObj, "_is_read_", &tmp)) {
            metadata.isRead = json_object_get_boolean(tmp);
        }
        
        metadata.lastReadDate = getString("_last_read_date_");
        
        if (json_object_object_get_ex(bookObj, "_is_favorite_", &tmp)) {
            metadata.isFavorite = json_object_get_boolean(tmp);
        }
        
        const char* lastUsedStr = json_object_get_string(lastUsedObj);
        std::string lastUsed = lastUsedStr ? lastUsedStr : "";
        
        if (!metadata.lpath.empty()) {
            cacheData.emplace(metadata.lpath, CacheEntry(metadata, lastUsed));
            loaded++;
        }
    }
    
    json_object_put(root);
    
    LOG_CACHE("Loaded %d entries from cache", loaded);
    return true;
}

bool CacheManager::saveCache() {
    LOG_CACHE("Saving cache with %d entries", (int)cacheData.size());
    
    purgeOldEntries(30);
    
    std::string tmpFilePath = cacheFilePath + ".tmp";
    FILE* f = fopen(tmpFilePath.c_str(), "w");
    if (!f) {
        LOG_CACHE("Failed to open tmp cache file for writing");
        return false;
    }
    
    fprintf(f, "{");
    
    bool first = true;
    for (const auto& entry : cacheData) {
        if (!first) fprintf(f, ",");
        first = false;

        const BookMetadata& meta = entry.second.metadata;
        
        // Escape lpath for JSON key
        std::string escapedPath = entry.first;
        size_t pos = 0;
        while ((pos = escapedPath.find('\\', pos)) != std::string::npos) {
            escapedPath.replace(pos, 1, "\\\\");
            pos += 2;
        }
        pos = 0;
        while ((pos = escapedPath.find('"', pos)) != std::string::npos) {
            escapedPath.replace(pos, 1, "\\\"");
            pos += 2;
        }
        
        fprintf(f, "\n  \"%s\": {", escapedPath.c_str());
        fprintf(f, "\n    \"book\": {");
        fprintf(f, "\n      \"uuid\": \"%s\",", meta.uuid.c_str());
        fprintf(f, "\n      \"title\": \"%s\",", meta.title.c_str());
        fprintf(f, "\n      \"authors\": \"%s\",", meta.authors.c_str());
        fprintf(f, "\n      \"lpath\": \"%s\",", meta.lpath.c_str());
        fprintf(f, "\n      \"last_modified\": \"%s\",", meta.lastModified.c_str());
        fprintf(f, "\n      \"_is_read_\": %s,", meta.isRead ? "true" : "false");
        
        if (!meta.lastReadDate.empty()) {
            fprintf(f, "\n      \"_last_read_date_\": \"%s\",", meta.lastReadDate.c_str());
        }
        
        fprintf(f, "\n      \"_is_favorite_\": %s", meta.isFavorite ? "true" : "false");
        fprintf(f, "\n    },");
        fprintf(f, "\n    \"last_used\": \"%s\"", entry.second.lastUsed.c_str());
        fprintf(f, "\n  }");
    }
    
    fprintf(f, "\n}\n");
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    if (rename(tmpFilePath.c_str(), cacheFilePath.c_str()) != 0) {
        LOG_CACHE("Failed to rename temp file to cache file");
        unlink(tmpFilePath.c_str());
        return false;
    }
    
    LOG_CACHE("Cache saved successfully (streaming write)");
    return true;
}

std::string CacheManager::getUuidForLpath(const std::string& lpath) const {
    auto it = cacheData.find(lpath);
    if (it != cacheData.end()) {
        return it->second.metadata.uuid;
    }
    return "";
}

bool CacheManager::getCachedMetadata(const std::string& lpath, BookMetadata& outMetadata) const {
    auto it = cacheData.find(lpath);
    if (it != cacheData.end()) {
        outMetadata = it->second.metadata;
        return true;
    }
    return false;
}

void CacheManager::updateCache(const BookMetadata& metadata) {
    if (metadata.lpath.empty()) {
        return;
    }
    
    std::string uuidToStore = metadata.uuid;
    
    // Используем find, который в unordered_map работает за O(1)
    auto it = cacheData.find(metadata.lpath);
    if (it != cacheData.end()) {
        if (uuidToStore.empty()) {
            uuidToStore = it->second.metadata.uuid;
        }
    }
    
    // Оптимизация: избегаем лишнего копирования при создании записи
    BookMetadata newMeta = metadata;
    newMeta.uuid = uuidToStore; // перемещение строки, если возможно, но здесь просто присваивание
    
    std::string timestamp = getCurrentTimestamp();
    
    // insert_or_assign (C++17) или оператор []
    cacheData[metadata.lpath] = CacheEntry(newMeta, timestamp);
}

void CacheManager::removeFromCache(const std::string& lpath) {
    cacheData.erase(lpath);
    LOG_CACHE("Removed from cache: %s", lpath.c_str());
}

void CacheManager::purgeOldEntries(int days) {
    time_t now = time(NULL);
    time_t threshold = now - (days * 24 * 60 * 60);
    
    auto it = cacheData.begin();
    while (it != cacheData.end()) {
        // Оптимизация: быстрая проверка перед парсингом, если строка пустая
        if (it->second.lastUsed.empty()) {
             it = cacheData.erase(it);
             continue;
        }

        time_t lastUsed = parseTimestamp(it->second.lastUsed);
        if (lastUsed > 0 && lastUsed < threshold) {
            it = cacheData.erase(it);
        } else {
            ++it;
        }
    }
}

void CacheManager::clearCache() {
    cacheData.clear();
    LOG_CACHE("Cache cleared");
}
