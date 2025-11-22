#include "cache_manager.h"
#include <json-c/json.h>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>
#include <algorithm>

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

std::string CacheManager::makeCacheKey(const std::string& uuid, 
                                      const std::string& lpath) const {
    if (uuid.empty() || lpath.empty()) return "";
    return uuid + "|" + lpath;
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
    
    // Iterate through cache entries
    json_object_object_foreach(root, key, val) {
        json_object* bookObj = NULL;
        json_object* lastUsedObj = NULL;
        
        if (!json_object_object_get_ex(val, "book", &bookObj) ||
            !json_object_object_get_ex(val, "last_used", &lastUsedObj)) {
            continue;
        }
        
        BookMetadata metadata;
        
        // Parse book metadata
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
        if (json_object_object_get_ex(bookObj, "author_sort", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.authorSort = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "lpath", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.lpath = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "series", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.series = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "series_index", &tmp)) {
            metadata.seriesIndex = json_object_get_int(tmp);
        }
        if (json_object_object_get_ex(bookObj, "size", &tmp)) {
            metadata.size = json_object_get_int64(tmp);
        }
        if (json_object_object_get_ex(bookObj, "last_modified", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.lastModified = str ? str : "";
        }
        if (json_object_object_get_ex(bookObj, "isbn", &tmp)) {
            const char* str = json_object_get_string(tmp);
            metadata.isbn = str ? str : "";
        }
        
        // Parse sync fields
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
        
        const char* lastUsedStr = json_object_get_string(lastUsedObj);
        std::string lastUsed = lastUsedStr ? lastUsedStr : "";
        
        if (!metadata.uuid.empty() && !metadata.lpath.empty()) {
            std::string cacheKey = makeCacheKey(metadata.uuid, metadata.lpath);
            cacheData[cacheKey] = CacheEntry(metadata, lastUsed);
            loaded++;
        }
    }
    
    json_object_put(root);
    
    LOG_CACHE("Loaded %d entries from cache", loaded);
    return true;
}

bool CacheManager::saveCache() {
    LOG_CACHE("Saving cache with %d entries", (int)cacheData.size());
    
    // Purge old entries before saving
    purgeOldEntries(30);
    
    json_object* root = json_object_new_object();
    
    for (const auto& entry : cacheData) {
        json_object* entryObj = json_object_new_object();
        json_object* bookObj = json_object_new_object();
        
        const BookMetadata& meta = entry.second.metadata;
        
        // Add book fields
        json_object_object_add(bookObj, "uuid", 
            json_object_new_string(meta.uuid.c_str()));
        json_object_object_add(bookObj, "title", 
            json_object_new_string(meta.title.c_str()));
        json_object_object_add(bookObj, "authors", 
            json_object_new_string(meta.authors.c_str()));
        json_object_object_add(bookObj, "author_sort", 
            json_object_new_string(meta.authorSort.c_str()));
        json_object_object_add(bookObj, "lpath", 
            json_object_new_string(meta.lpath.c_str()));
        json_object_object_add(bookObj, "series", 
            json_object_new_string(meta.series.c_str()));
        json_object_object_add(bookObj, "series_index", 
            json_object_new_int(meta.seriesIndex));
        json_object_object_add(bookObj, "size", 
            json_object_new_int64(meta.size));
        json_object_object_add(bookObj, "last_modified", 
            json_object_new_string(meta.lastModified.c_str()));
        json_object_object_add(bookObj, "isbn", 
            json_object_new_string(meta.isbn.c_str()));
        
        // Add sync fields
        json_object_object_add(bookObj, "_is_read_", 
            json_object_new_boolean(meta.isRead));
        if (!meta.lastReadDate.empty()) {
            json_object_object_add(bookObj, "_last_read_date_", 
                json_object_new_string(meta.lastReadDate.c_str()));
        }
        json_object_object_add(bookObj, "_is_favorite_", 
            json_object_new_boolean(meta.isFavorite));
        
        json_object_object_add(entryObj, "book", bookObj);
        json_object_object_add(entryObj, "last_used", 
            json_object_new_string(entry.second.lastUsed.c_str()));
        
        json_object_object_add(root, entry.first.c_str(), entryObj);
    }
    
    const char* jsonStr = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PRETTY);
    
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

bool CacheManager::isInCache(const std::string& uuid, 
                             const std::string& lpath,
                             const std::string& lastModified) const {
    std::string key = makeCacheKey(uuid, lpath);
    
    auto it = cacheData.find(key);
    if (it == cacheData.end()) {
        return false;
    }
    
    // Check if last_modified matches
    return it->second.metadata.lastModified == lastModified;
}

BookMetadata CacheManager::getCachedMetadata(const std::string& uuid,
                                            const std::string& lpath) const {
    std::string key = makeCacheKey(uuid, lpath);
    
    auto it = cacheData.find(key);
    if (it != cacheData.end()) {
        return it->second.metadata;
    }
    
    return BookMetadata();
}

void CacheManager::updateCache(const BookMetadata& metadata) {
    if (metadata.uuid.empty() || metadata.lpath.empty()) {
        LOG_CACHE("Cannot update cache: missing uuid or lpath");
        return;
    }
    
    std::string key = makeCacheKey(metadata.uuid, metadata.lpath);
    std::string timestamp = getCurrentTimestamp();
    
    cacheData[key] = CacheEntry(metadata, timestamp);
}

void CacheManager::removeFromCache(const std::string& uuid, 
                                   const std::string& lpath) {
    std::string key = makeCacheKey(uuid, lpath);
    cacheData.erase(key);
    
    LOG_CACHE("Removed from cache: %s", key.c_str());
}

void CacheManager::purgeOldEntries(int days) {
    time_t now = time(NULL);
    time_t threshold = now - (days * 24 * 60 * 60);
    
    std::vector<std::string> toRemove;
    
    for (const auto& entry : cacheData) {
        time_t lastUsed = parseTimestamp(entry.second.lastUsed);
        if (lastUsed > 0 && lastUsed < threshold) {
            toRemove.push_back(entry.first);
        }
    }
    
    for (const auto& key : toRemove) {
        cacheData.erase(key);
    }
    
    if (!toRemove.empty()) {
        LOG_CACHE("Purged %d old cache entries", (int)toRemove.size());
    }
}

void CacheManager::clearCache() {
    cacheData.clear();
    LOG_CACHE("Cache cleared");
}