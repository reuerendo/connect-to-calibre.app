#include "book_manager.h"
#include "inkview.h"
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <unordered_map>

#define LOG_MSG(fmt, ...) { FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a"); if(f) { fprintf(f, "[DB] " fmt "\n", ##__VA_ARGS__); fclose(f); } }

// --- Cache & Helpers ---

// Кэш для папок и профиля, чтобы не дергать БД на каждой книге
static int g_cachedProfileId = -1;
static std::unordered_map<std::string, int> g_folderCache;

// Сброс кэша (можно вызывать при initialize, если нужно)
static void resetInternalCache() {
    g_cachedProfileId = -1;
    g_folderCache.clear();
}

// Быстрый парсинг ISO даты без оверхеда sscanf/strptime
// Формат: YYYY-MM-DDTHH:MM:SS...
static time_t fastParseIsoTime(const std::string& isoTime) {
    if (isoTime.size() < 19) return 0;
    
    const char* s = isoTime.c_str();
    
    struct tm tm = {0};
    // Простой парсинг atoi-style для фиксированных позиций
    auto parseInt = [](const char* p, int len) {
        int val = 0;
        for(int i=0; i<len; ++i) val = val*10 + (p[i] - '0');
        return val;
    };

    tm.tm_year = parseInt(s, 4) - 1900;
    tm.tm_mon  = parseInt(s + 5, 2) - 1;
    tm.tm_mday = parseInt(s + 8, 2);
    tm.tm_hour = parseInt(s + 11, 2);
    tm.tm_min  = parseInt(s + 14, 2);
    tm.tm_sec  = parseInt(s + 17, 2);
    
    return timegm(&tm);
}

static std::string formatIsoTime(time_t timestamp) {
    if (timestamp == 0) return "1970-01-01T00:00:00+00:00";
    char buffer[32];
    struct tm* tm_info = gmtime(&timestamp);
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", tm_info);
    return std::string(buffer);
}

static time_t roundToDay(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    tm_info->tm_hour = 23;
    tm_info->tm_min = 59;
    tm_info->tm_sec = 59;
    return mktime(tm_info);
}

// --- Implementation ---

BookManager::BookManager() {
    booksDir = "/mnt/ext1"; 
}

BookManager::~BookManager() {
}

bool BookManager::initialize(const std::string& dbPath) {
    resetInternalCache();
    return true;
}

sqlite3* BookManager::openDB() {
    sqlite3* db;
    // Используем SQLITE_OPEN_READWRITE без CREATE, системная БД должна существовать
    int rc = sqlite3_open_v2(SYSTEM_DB_PATH.c_str(), &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        LOG_MSG("Failed to open DB: %s", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    
    // Ускорение работы с БД
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL);
    
    return db;
}

void BookManager::closeDB(sqlite3* db) {
    if (db) sqlite3_close(db);
}

int BookManager::getStorageId(const std::string& filename) {
    // Простая проверка без создания лишних string объектов
    if (filename.compare(0, 10, "/mnt/ext1/") == 0 || filename == "/mnt/ext1") return 1;
    return 2;
}

std::string BookManager::getFirstLetter(const std::string& str) {
    if (str.empty()) return "";
    
    unsigned char first = (unsigned char)str[0];
    
    if (isalnum(first) || ispunct(first)) {
        char upper = toupper(first);
        return std::string(1, upper);
    }
    
    // Для UTF-8 берем первые 2 байта (обычно кириллица попадает сюда)
    // Оптимизация: избегаем полного копирования строки
    std::string res;
    res.reserve(2);
    res.push_back(toupper((unsigned char)str[0]));
    if (str.size() > 1) res.push_back(toupper((unsigned char)str[1]));
    return res;
}

int BookManager::getCurrentProfileId(sqlite3* db) {
    // Возвращаем закэшированное значение, если есть
    if (g_cachedProfileId != -1) return g_cachedProfileId;

    char* profileName = GetCurrentProfile();
    if (!profileName) {
        g_cachedProfileId = 1;
        return 1; 
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM profiles WHERE name = ?";
    int id = 1; // Default

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, profileName, -1, SQLITE_STATIC); // Имя профиля не меняется пока мы тут
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    if (profileName) free(profileName);
    
    g_cachedProfileId = id;
    return id;
}

int BookManager::getOrCreateFolder(sqlite3* db, const std::string& folderPath, int storageId) {
    // Ключ для кэша: storageId + path
    std::string cacheKey = std::to_string(storageId) + ":" + folderPath;
    auto it = g_folderCache.find(cacheKey);
    if (it != g_folderCache.end()) {
        return it->second;
    }

    int folderId = -1;

    // Оптимизация: Сначала пробуем найти (SELECT), это дешевле чем INSERT с обработкой конфликта
    const char* selectSql = "SELECT id FROM folders WHERE storageid = ? AND name = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, storageId);
        sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            folderId = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (folderId == -1) {
        const char* insertSql = "INSERT INTO folders (storageid, name) VALUES (?, ?)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, storageId);
            sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                folderId = (int)sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(stmt);
        }
    }

    if (folderId != -1) {
        g_folderCache[cacheKey] = folderId;
    }
    
    return folderId;
}

std::string BookManager::getBookFilePath(const std::string& lpath) {
    if (lpath.empty()) return "";
    if (lpath[0] == '/') {
        return (booksDir.back() == '/') ? booksDir + lpath.substr(1) : booksDir + lpath;
    }
    return (booksDir.back() == '/') ? booksDir + lpath : booksDir + "/" + lpath;
}

// В book_manager.cpp

bool BookManager::processBookSettings(sqlite3* db, int bookId, const BookMetadata& metadata, int profileId) {
    int completed = metadata.isRead ? 1 : 0;
    int favorite = metadata.isFavorite ? 1 : 0;
    
    time_t completedTs = 0;
    if (metadata.isRead && !metadata.lastReadDate.empty()) {
        completedTs = fastParseIsoTime(metadata.lastReadDate);
    }

    // Check if record exists
    const char* checkSql = "SELECT 1 FROM books_settings WHERE bookid = ? AND profileid = ?";
    bool exists = false;
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, bookId);
        sqlite3_bind_int(stmt, 2, profileId);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        if (metadata.isRead) {
            // Book is READ: set progress to 100%
            static const char* updateSqlRead = 
                "UPDATE books_settings "
                "SET completed = ?, favorite = ?, completed_ts = ?, cpage = 100, npage = 100 "
                "WHERE bookid = ? AND profileid = ?";
                
            if (sqlite3_prepare_v2(db, updateSqlRead, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, completed);
                sqlite3_bind_int(stmt, 2, favorite);
                sqlite3_bind_int64(stmt, 3, completedTs);
                sqlite3_bind_int(stmt, 4, bookId);
                sqlite3_bind_int(stmt, 5, profileId);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        } else {
            // Book is NOT read: preserve existing reading progress, don't force reset
            static const char* updateSqlUnread = 
                "UPDATE books_settings "
                "SET completed = 0, favorite = ?, completed_ts = 0 "
                "WHERE bookid = ? AND profileid = ?";
                
            if (sqlite3_prepare_v2(db, updateSqlUnread, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, favorite);
                sqlite3_bind_int(stmt, 2, bookId);
                sqlite3_bind_int(stmt, 3, profileId);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    } else {
        // New record
        int initialCpage = metadata.isRead ? 100 : 0;
        int initialNpage = metadata.isRead ? 100 : 0;

        static const char* insertSql = 
            "INSERT INTO books_settings (bookid, profileid, completed, favorite, completed_ts, cpage, npage) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)";
            
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_bind_int(stmt, 2, profileId);
            sqlite3_bind_int(stmt, 3, completed);
            sqlite3_bind_int(stmt, 4, favorite);
            sqlite3_bind_int64(stmt, 5, completedTs);
            sqlite3_bind_int(stmt, 6, initialCpage);
            sqlite3_bind_int(stmt, 7, initialNpage);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return true;
}

bool BookManager::addBook(const BookMetadata& metadata) {
    // Оптимизация: не вызываем stat(), так как metadata уже содержит size и lastModified из Calibre
    std::string fullPath = getBookFilePath(metadata.lpath);
    
    // Парсинг пути
    std::string folderName, fileName;
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) {
        folderName = "";
        fileName = fullPath;
    } else {
        folderName = fullPath.substr(0, lastSlash);
        fileName = fullPath.substr(lastSlash + 1);
    }
    
    std::string fileExt;
    size_t lastDot = fileName.find_last_of('.');
    if (lastDot != std::string::npos) fileExt = fileName.substr(lastDot + 1);

    // Используем переданные метаданные вместо stat()
    long long fileSize = metadata.size;
    time_t fileMtime = fastParseIsoTime(metadata.lastModified);
    if (fileMtime == 0) fileMtime = time(NULL); // Fallback

    sqlite3* db = openDB();
    if (!db) return false;

    int storageId = getStorageId(fullPath);
    time_t now = time(NULL);
    time_t dayRounded = roundToDay(now);

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // Используем кэшированный поиск папки
    int folderId = getOrCreateFolder(db, folderName, storageId);
    if (folderId == -1) {
        LOG_MSG("Error: Failed to get folder ID");
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        closeDB(db);
        return false;
    }

    static const char* checkFileSql = "SELECT id, book_id FROM files WHERE filename = ? AND folder_id = ?";
    sqlite3_stmt* stmt;
    int fileId = -1;
    int bookId = -1;
    
    if (sqlite3_prepare_v2(db, checkFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, folderId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int(stmt, 0);
            bookId = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    std::string sortAuthor = metadata.authorSort.empty() ? metadata.authors : metadata.authorSort;
    std::string firstAuthorLetter = getFirstLetter(sortAuthor);
    std::string firstTitleLetter = getFirstLetter(metadata.title);

    if (fileId != -1) {
        static const char* updateFileSql = "UPDATE files SET size = ?, modification_time = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db, updateFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, fileSize);
            sqlite3_bind_int64(stmt, 2, (long long)fileMtime);
            sqlite3_bind_int(stmt, 3, fileId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        static const char* updateBookSql = 
            "UPDATE books_impl SET title=?, first_title_letter=?, author=?, firstauthor=?, "
            "first_author_letter=?, series=?, numinseries=?, size=?, isbn=?, sort_title=?, "
            "updated=?, ts_added=? WHERE id=?";
            
        if (sqlite3_prepare_v2(db, updateBookSql, -1, &stmt, nullptr) == SQLITE_OK) {
            // Используем SQLITE_STATIC для всех строк из metadata, они живы до конца функции
            sqlite3_bind_text(stmt, 1, metadata.title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, firstTitleLetter.c_str(), -1, SQLITE_TRANSIENT); // Локальная переменная
            sqlite3_bind_text(stmt, 3, metadata.authors.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, sortAuthor.c_str(), -1, SQLITE_TRANSIENT); // Локальная переменная
            sqlite3_bind_text(stmt, 5, firstAuthorLetter.c_str(), -1, SQLITE_TRANSIENT); // Локальная переменная
            sqlite3_bind_text(stmt, 6, metadata.series.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 7, metadata.seriesIndex);
            sqlite3_bind_int64(stmt, 8, metadata.size);
            sqlite3_bind_text(stmt, 9, metadata.isbn.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 10, metadata.title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 11, now);
            sqlite3_bind_int64(stmt, 12, dayRounded);
            sqlite3_bind_int(stmt, 13, bookId);
            
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        static const char* insertBookSql = 
            "INSERT INTO books_impl (title, first_title_letter, author, firstauthor, "
            "first_author_letter, series, numinseries, size, isbn, sort_title, creationtime, "
            "updated, ts_added, hidden) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
            
        if (sqlite3_prepare_v2(db, insertBookSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, metadata.title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, firstTitleLetter.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, metadata.authors.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, sortAuthor.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, firstAuthorLetter.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, metadata.series.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 7, metadata.seriesIndex);
            sqlite3_bind_int64(stmt, 8, metadata.size);
            sqlite3_bind_text(stmt, 9, metadata.isbn.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 10, metadata.title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 11, 0);
            sqlite3_bind_int(stmt, 12, 0);
            sqlite3_bind_int64(stmt, 13, dayRounded);
            sqlite3_bind_int(stmt, 14, 0);
            
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                bookId = (int)sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(stmt);
        }

        if (bookId != -1) {
            static const char* insertFileSql = 
                "INSERT INTO files (storageid, folder_id, book_id, filename, size, modification_time, ext) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)";
                
            if (sqlite3_prepare_v2(db, insertFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, storageId);
                sqlite3_bind_int(stmt, 2, folderId);
                sqlite3_bind_int(stmt, 3, bookId);
                sqlite3_bind_text(stmt, 4, fileName.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 5, fileSize);
                sqlite3_bind_int64(stmt, 6, (long long)fileMtime);
                sqlite3_bind_text(stmt, 7, fileExt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    if (bookId != -1) {
        int profileId = getCurrentProfileId(db);
        processBookSettings(db, bookId, metadata, profileId);
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    
    // VACUUM здесь избыточен для каждой книги, лучше делать это реже
    // sqlite3_exec(db, "VACUUM", NULL, NULL, NULL); 
    
    closeDB(db);
    return true;
}

bool BookManager::updateBookSync(const BookMetadata& metadata) {
    sqlite3* db = openDB();
    if (!db) return false;

    // Пытаемся найти ID без транзакции для скорости
    int bookId = findBookIdByPath(db, metadata.lpath);
    
    if (bookId == -1) {
        LOG_MSG("Sync: Book not found in DB: %s", metadata.lpath.c_str());
        closeDB(db);
        return false;
    }

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    int profileId = getCurrentProfileId(db);
    bool res = processBookSettings(db, bookId, metadata, profileId);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    // PRAGMA wal_checkpoint(FULL) тоже тяжелая операция, лучше доверить SQLite
    
    closeDB(db);
    return res;
}

bool BookManager::updateBook(const BookMetadata& metadata) {
    return addBook(metadata);
}

bool BookManager::deleteBook(const std::string& lpath) {
    std::string filePath = getBookFilePath(lpath);
    LOG_MSG("Deleting book: %s", filePath.c_str());
    
    remove(filePath.c_str());

    sqlite3* db = openDB();
    if (!db) return false;

    std::string folderName, fileName;
    size_t lastSlash = filePath.find_last_of('/');
    if (lastSlash == std::string::npos) {
        folderName = "";
        fileName = filePath;
    } else {
        folderName = filePath.substr(0, lastSlash);
        fileName = filePath.substr(lastSlash + 1);
    }
    
    int storageId = getStorageId(filePath);

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    static const char* findSql = 
        "SELECT f.id, f.book_id FROM files f "
        "JOIN folders fo ON f.folder_id = fo.id "
        "WHERE f.filename = ? AND fo.name = ? AND f.storageid = ?";
        
    sqlite3_stmt* stmt;
    int fileId = -1;
    int bookId = -1;

    if (sqlite3_prepare_v2(db, findSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, folderName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, storageId);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int(stmt, 0);
            bookId = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    if (fileId != -1) {
        // Кэширование statement здесь менее критично (удаление редкое), 
        // но static const char* полезен
        static const char* sql1 = "DELETE FROM files WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql1, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, fileId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        static const char* sql2 = "DELETE FROM books_settings WHERE bookid = ?";
        if (sqlite3_prepare_v2(db, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        static const char* sql3 = "DELETE FROM books_impl WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql3, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    
    closeDB(db);
    return true;
}

std::vector<BookMetadata> BookManager::getAllBooks() {
    std::vector<BookMetadata> books;
    books.reserve(2048);

    sqlite3* db = openDB();
    if (!db) return books;

    int profileId = getCurrentProfileId(db);
    
    static const char* sql = 
		"SELECT b.id, b.title, b.author, b.series, b.numinseries, b.size, f.modification_time, "
		"f.filename, fo.name, bs.completed, bs.favorite, bs.completed_ts "
		"FROM books_impl b "
		"JOIN files f ON b.id = f.book_id "
		"JOIN folders fo ON f.folder_id = fo.id "
		"LEFT JOIN books_settings bs ON b.id = bs.bookid AND bs.profileid = ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, profileId);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            BookMetadata meta;
            meta.dbBookId = sqlite3_column_int(stmt, 0);
            
            const char* title = (const char*)sqlite3_column_text(stmt, 1);
            const char* author = (const char*)sqlite3_column_text(stmt, 2);
            const char* series = (const char*)sqlite3_column_text(stmt, 3);
            
            if (title) meta.title = title;
            if (author) meta.authors = author;
            if (series) meta.series = series;
            
            meta.seriesIndex = sqlite3_column_int(stmt, 4);
            meta.size = sqlite3_column_int64(stmt, 5);
            
            const char* filename = (const char*)sqlite3_column_text(stmt, 7);
            const char* folder = (const char*)sqlite3_column_text(stmt, 8);
            
            if (filename && folder) {
                std::string fullPath = std::string(folder) + "/" + filename;
                if (fullPath.compare(0, booksDir.length(), booksDir) == 0) {
                     // Оптимизированный substr
                    meta.lpath = fullPath.substr(booksDir.length());
                    if (!meta.lpath.empty() && meta.lpath[0] == '/') {
                        meta.lpath.erase(0, 1);
                    }
                } else {
                    meta.lpath = filename;
                }
            }
            
            meta.isRead = (sqlite3_column_int(stmt, 9) != 0);
            meta.isFavorite = (sqlite3_column_int(stmt, 10) != 0);
            
            time_t readTs = (time_t)sqlite3_column_int64(stmt, 11);
            if (meta.isRead && readTs > 0) {
                meta.lastReadDate = formatIsoTime(readTs);
            }
            
            time_t fileMtime = (time_t)sqlite3_column_int64(stmt, 6);
			meta.lastModified = formatIsoTime(fileMtime);

            books.push_back(std::move(meta));
        }
        sqlite3_finalize(stmt);
    }
    
    closeDB(db);
    return books;
}

int BookManager::getBookCount() {
    return getAllBooks().size();
}

int BookManager::findBookIdByPath(sqlite3* db, const std::string& lpath) {
    std::string fullPath = getBookFilePath(lpath);
    std::string folderName, fileName;
    
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) {
        folderName = "";
        fileName = fullPath;
    } else {
        folderName = fullPath.substr(0, lastSlash);
        fileName = fullPath.substr(lastSlash + 1);
    }
    
    static const char* sql = "SELECT f.book_id FROM files f JOIN folders fo ON f.folder_id = fo.id WHERE f.filename = ? AND fo.name = ?";
    sqlite3_stmt* stmt;
    int bookId = -1;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, folderName.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            bookId = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return bookId;
}

int BookManager::getOrCreateBookshelf(sqlite3* db, const std::string& name) {
    int shelfId = -1;
    time_t now = time(NULL);
    
    static const char* findSql = "SELECT id FROM bookshelfs WHERE name = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, findSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            shelfId = sqlite3_column_int(stmt, 0);
            static const char* restoreSql = "UPDATE bookshelfs SET is_deleted = 0, ts = ? WHERE id = ?";
            sqlite3_stmt* stmt2;
            if (sqlite3_prepare_v2(db, restoreSql, -1, &stmt2, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt2, 1, now);
                sqlite3_bind_int(stmt2, 2, shelfId);
                sqlite3_step(stmt2);
                sqlite3_finalize(stmt2);
            }
        }
        sqlite3_finalize(stmt);
    }
    
    if (shelfId == -1) {
        static const char* insertSql = "INSERT INTO bookshelfs (name, is_deleted, ts) VALUES (?, 0, ?)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, now);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                shelfId = (int)sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(stmt);
        }
    }
    return shelfId;
}

void BookManager::linkBookToShelf(sqlite3* db, int shelfId, int bookId) {
    time_t now = time(NULL);
    
    static const char* checkSql = "SELECT 1 FROM bookshelfs_books WHERE bookshelfid = ? AND bookid = ?";
    bool exists = false;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, shelfId);
        sqlite3_bind_int(stmt, 2, bookId);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }
    
    if (exists) {
        static const char* updateSql = "UPDATE bookshelfs_books SET is_deleted = 0, ts = ? WHERE bookshelfid = ? AND bookid = ?";
        if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_int(stmt, 2, shelfId);
            sqlite3_bind_int(stmt, 3, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        static const char* insertSql = "INSERT INTO bookshelfs_books (bookshelfid, bookid, ts, is_deleted) VALUES (?, ?, ?, 0)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, shelfId);
            sqlite3_bind_int(stmt, 2, bookId);
            sqlite3_bind_int64(stmt, 3, now);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}
