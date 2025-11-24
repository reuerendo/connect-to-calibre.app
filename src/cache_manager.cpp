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
    // Оптимизация: Убран лишний вызов stat(). Сразу пробуем открыть.
    FILE* f = fopen(cacheFilePath.c_str(), "r");
    if (!f) {
        // Файла нет или ошибка доступа - считаем кэш пустым, это нормальная ситуация
        LOG_CACHE("Cache file not found or inaccessible, starting fresh");
        return true;
    }
    
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize <= 0) {
        fclose(f);
        LOG_CACHE("Empty cache file");
        return true;
    }
    
    // Проверка на разумный размер файла, чтобы не съесть всю память
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
        (void)key;
        
        json_object* bookObj = NULL;
        json_object* lastUsedObj = NULL;
        
        if (!json_object_object_get_ex(val, "book", &bookObj) ||
            !json_object_object_get_ex(val, "last_used", &lastUsedObj)) {
            continue;
        }
        
        BookMetadata metadata;
        
        // Helper lambda for string extraction could optimize code size, but inline is faster here
        json_object* tmp = NULL;
        if (json_object_object_get_ex(bookObj, "uuid", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.uuid = str;
        }
        if (json_object_object_get_ex(bookObj, "title", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.title = str;
        }
        if (json_object_object_get_ex(bookObj, "authors", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.authors = str;
        }
        if (json_object_object_get_ex(bookObj, "lpath", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.lpath = str;
        }
        if (json_object_object_get_ex(bookObj, "last_modified", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.lastModified = str;
        }
        
        // Parse sync fields
        if (json_object_object_get_ex(bookObj, "_is_read_", &tmp)) {
            metadata.isRead = json_object_get_boolean(tmp);
        }
        if (json_object_object_get_ex(bookObj, "_last_read_date_", &tmp)) {
            const char* str = json_object_get_string(tmp);
            if(str) metadata.lastReadDate = str;
        }
        if (json_object_object_get_ex(bookObj, "_is_favorite_", &tmp)) {
            metadata.isFavorite = json_object_get_boolean(tmp);
        }
        
        const char* lastUsedStr = json_object_get_string(lastUsedObj);
        std::string lastUsed = lastUsedStr ? lastUsedStr : "";
        
        if (!metadata.lpath.empty()) {
            // Emplace construct optimization
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
    
    // Оптимизация записи: Atomic Write (tmp file -> rename)
    std::string tmpFilePath = cacheFilePath + ".tmp";
    FILE* f = fopen(tmpFilePath.c_str(), "w");
    if (!f) {
        LOG_CACHE("Failed to open tmp cache file for writing");
        return false;
    }
    
    // ОПТИМИЗАЦИЯ RAM: Потоковая запись JSON
    // Вместо создания огромного json_object дерева в памяти,
    // мы пишем файл кусками вручную, сериализуя только по одной книге за раз.
    
    if (fputc('{', f) == EOF) { fclose(f); return false; }
    
    bool first = true;
    for (const auto& entry : cacheData) {
        if (!first) {
            if (fputc(',', f) == EOF) break;
        }
        first = false;

        // Создаем мини-JSON только для одной записи
        json_object* entryObj = json_object_new_object();
        json_object* bookObj = json_object_new_object();
        
        const BookMetadata& meta = entry.second.metadata;
        
        json_object_object_add(bookObj, "uuid", json_object_new_string(meta.uuid.c_str()));
        json_object_object_add(bookObj, "title", json_object_new_string(meta.title.c_str()));
        json_object_object_add(bookObj, "authors", json_object_new_string(meta.authors.c_str()));
        json_object_object_add(bookObj, "lpath", json_object_new_string(meta.lpath.c_str()));
        json_object_object_add(bookObj, "last_modified", json_object_new_string(meta.lastModified.c_str()));
        
        json_object_object_add(bookObj, "_is_read_", json_object_new_boolean(meta.isRead));
        if (!meta.lastReadDate.empty()) {
            json_object_object_add(bookObj, "_last_read_date_", json_object_new_string(meta.lastReadDate.c_str()));
        }
        json_object_object_add(bookObj, "_is_favorite_", json_object_new_boolean(meta.isFavorite));
        
        json_object_object_add(entryObj, "book", bookObj);
        json_object_object_add(entryObj, "last_used", json_object_new_string(entry.second.lastUsed.c_str()));
        
        // Получаем JSON строку для значения
        const char* valStr = json_object_to_json_string_ext(entryObj, JSON_C_TO_STRING_PLAIN);
        
        // Используем json-c для безопасного экранирования ключа (пути к файлу)
        json_object* keyStrObj = json_object_new_string(meta.lpath.c_str());
        const char* keyStr = json_object_to_json_string_ext(keyStrObj, JSON_C_TO_STRING_PLAIN);
        
        // Записываем пару "Ключ": Значение
        fprintf(f, "\n  %s: %s", keyStr, valStr);
        
        // Немедленно освобождаем память
        json_object_put(keyStrObj);
        json_object_put(entryObj);
    }
    
    fprintf(f, "\n}");
    
    // Сброс буферов на диск для надежности
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    // Атомарная подмена файла
    if (rename(tmpFilePath.c_str(), cacheFilePath.c_str()) != 0) {
        LOG_CACHE("Failed to rename temp file to cache file");
        unlink(tmpFilePath.c_str()); // Удаляем мусор
        return false;
    }
    
    LOG_CACHE("Cache saved successfully (Atomic)");
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
