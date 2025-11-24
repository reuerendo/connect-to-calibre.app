#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include "book_manager.h"
#include <string>
#include <unordered_map> // Оптимизация: HashMap вместо дерева
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
    
    // Get cached UUID for a specific file path
    // Returns empty string if not found
    std::string getUuidForLpath(const std::string& lpath) const;

    // Get full cached metadata
    // Returns true if found and fills outMetadata
    bool getCachedMetadata(const std::string& lpath, BookMetadata& outMetadata) const;
    
    // Update or add to cache
    void updateCache(const BookMetadata& metadata);
    
    // Remove from cache
    void removeFromCache(const std::string& lpath);
    
    // Clear old entries (called during save)
    void purgeOldEntries(int days = 30);
    
    // Get cache statistics
    int getCacheSize() const { return cacheData.size(); }
    
    // Clear all cache
    void clearCache();
    
private:
    std::string deviceUuid;
    std::string cacheFilePath;
    
    // Оптимизация: unordered_map для доступа O(1)
    // Key: lpath (file path relative to root), Value: CacheEntry
    std::unordered_map<std::string, CacheEntry> cacheData; 
    
    // Helper to get current ISO timestamp
    std::string getCurrentTimestamp() const;
    
    // Helper to parse ISO timestamp
    time_t parseTimestamp(const std::string& isoTime) const;
};

#endif // CACHE_MANAGER_H
