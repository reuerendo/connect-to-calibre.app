#include "cache_manager.h"
#include <json-c/json.h>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>  // <--- Added this to fix 'strlen' error

#define LOG_CACHE(fmt, ...) { \
    FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a"); \
    if(f) { fprintf(f, "[CACHE] " fmt "\n", ##__VA_ARGS__); fclose(f); } \
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
    cacheFilePath = "/mnt/ext1/system/calibre_cache_" + deviceUuid + ".json";
    
    LOG_CACHE("Initialized cache for device: %s", deviceUuid.c_str());
    LOG_CACHE("Cache file path: %s", cacheFilePath.c_str());
    
    return loadCache();
}

std::string CacheManager::getCurrentTimestamp() const {
    time_t now = time(NULL);
    struct tm* tm_info = gmtime(&now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", tm_info);
    return std::string(buffer);
}

time_t CacheManager::parseTimestamp(const std::string& isoTime) const {
    if (isoTime.empty()) return 0;
    
    struct tm tm = {0};
    int y, m, d, H, M, S;
    
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
    struct stat st;
    if (stat(cacheFilePath.c_str(), &st) != 0) {
        LOG_CACHE("Cache file does not exist, starting fresh");
        return true;
    }
    
    FILE* f = fopen(cacheFilePath.c_str(), "r");
    if (!f) {
        LOG_CACHE("Failed to open cache file for reading");
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize <= 0) {
        fclose(f);
        LOG_CACHE("Empty cache file");
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
        if (json_object_object_get_ex(bookObj, "uuid", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.uuid = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "title", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.title = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "authors", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.authors = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "lpath", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.lpath = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "last_modified", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.lastModified = str ? str : "";
        }
        
        // Current sync fields
        if (json_object_object_get_ex(bookObj, "_is_read_", &tmp)) {
            metadata.isRead = json_object_get_boolean(tmp);
        }
        if (json_object_object_get_ex(bookObj, "_last_read_date_", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.lastReadDate = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "_is_favorite_", &tmp)) {
            metadata.isFavorite = json_object_get_boolean(tmp);
        }
        
        // NEW: Load original values from cache
        if (json_object_object_get_ex(bookObj, "_original_is_read_", &tmp)) {
            metadata.originalIsRead = json_object_get_boolean(tmp);
            metadata.hasOriginalValues = true;
        }
        if (json_object_object_get_ex(bookObj, "_original_last_read_date_", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.originalLastReadDate = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "_original_is_favorite_", &tmp)) {
            metadata.originalIsFavorite = json_object_get_boolean(tmp);
        }
        
        const char* lastUsedStr = json_object_get_string(lastUsedObj);
        std::string lastUsed = lastUsedStr ? lastUsedStr : "";
        
        if (!metadata.lpath.empty()) {
            cacheData[metadata.lpath] = CacheEntry(metadata, lastUsed);
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
    
    json_object* root = json_object_new_object();
    
    for (const auto& entry : cacheData) {
        json_object* entryObj = json_object_new_object();
        json_object* bookObj = json_object_new_object();
        
        const BookMetadata& meta = entry.second.metadata;
        
        std::string jsonKey = meta.lpath; 
        
        json_object_object_add(bookObj, "uuid", json_object_new_string(meta.uuid.c_str()));
        json_object_object_add(bookObj, "title", json_object_new_string(meta.title.c_str()));
        json_object_object_add(bookObj, "authors", json_object_new_string(meta.authors.c_str()));
        json_object_object_add(bookObj, "lpath", json_object_new_string(meta.lpath.c_str()));
        json_object_object_add(bookObj, "last_modified", json_object_new_string(meta.lastModified.c_str()));
        
        // Current sync fields
        json_object_object_add(bookObj, "_is_read_", json_object_new_boolean(meta.isRead));
        if (!meta.lastReadDate.empty()) {
            json_object_object_add(bookObj, "_last_read_date_", json_object_new_string(meta.lastReadDate.c_str()));
        }
        json_object_object_add(bookObj, "_is_favorite_", json_object_new_boolean(meta.isFavorite));
        
        // NEW: Save original values to cache
        if (meta.hasOriginalValues) {
            json_object_object_add(bookObj, "_original_is_read_", 
                                  json_object_new_boolean(meta.originalIsRead));
            if (!meta.originalLastReadDate.empty()) {
                json_object_object_add(bookObj, "_original_last_read_date_", 
                                      json_object_new_string(meta.originalLastReadDate.c_str()));
            }
            json_object_object_add(bookObj, "_original_is_favorite_", 
                                  json_object_new_boolean(meta.originalIsFavorite));
        }
        
        json_object_object_add(entryObj, "book", bookObj);
        json_object_object_add(entryObj, "last_used", json_object_new_string(entry.second.lastUsed.c_str()));
        
        json_object_object_add(root, jsonKey.c_str(), entryObj);
    }
    
    const char* jsonStr = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    
    FILE* f = fopen(cacheFilePath.c_str(), "w");
    if (!f) {
        LOG_CACHE("Failed to open cache file for writing");
        json_object_put(root);
        return false;
    }
    
    fwrite(jsonStr, 1, strlen(jsonStr), f);
    fclose(f);
    
    json_object_put(root);
    
    LOG_CACHE("Cache saved successfully");
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
    
    auto it = cacheData.find(metadata.lpath);
    if (it != cacheData.end()) {
        if (uuidToStore.empty()) {
            uuidToStore = it->second.metadata.uuid;
        }
    }
    
    BookMetadata newMeta = metadata;
    newMeta.uuid = uuidToStore;
    
    std::string timestamp = getCurrentTimestamp();
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
