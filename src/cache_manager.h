#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include "book_manager.h"
#include <string>
#include <map>
#include <vector>

// Cache entry structure matching Calibre's expectations
struct CacheEntry {
    BookMetadata metadata;
    std::string lastUsed; // ISO 8601 format
    
    CacheEntry() {}
    CacheEntry(const BookMetadata& meta, const std::string& used) 
        : metadata(meta), lastUsed(used) {}
};

class CacheManager {
public:
    CacheManager();
    ~CacheManager();
    
    // Initialize cache for device UUID
    bool initialize(const std::string& deviceUuid);
    
    // Cache operations
    bool loadCache();
    bool saveCache();
    
    // Check if book metadata is in cache and up-to-date
    bool isInCache(const std::string& uuid, const std::string& lpath, 
                   const std::string& lastModified) const;
    
    // Get cached metadata
    BookMetadata getCachedMetadata(const std::string& uuid, 
                                   const std::string& lpath) const;
    
    // Update or add to cache
    void updateCache(const BookMetadata& metadata);
    
    // Remove from cache
    void removeFromCache(const std::string& uuid, const std::string& lpath);
    
    // Clear old entries (called during save)
    void purgeOldEntries(int days = 30);
    
    // Get cache statistics
    int getCacheSize() const { return cacheData.size(); }
    
    // Clear all cache
    void clearCache();
    
private:
    std::string deviceUuid;
    std::string cacheFilePath;
    std::map<std::string, CacheEntry> cacheData; // Key: uuid + lpath
    
    // Helper to generate cache key
    std::string makeCacheKey(const std::string& uuid, 
                            const std::string& lpath) const;
    
    // Helper to get current ISO timestamp
    std::string getCurrentTimestamp() const;
    
    // Helper to parse ISO timestamp
    time_t parseTimestamp(const std::string& isoTime) const;
};

#endif // CACHE_MANAGER_H