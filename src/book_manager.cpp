#include "book_manager.h"
#include "inkview.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

BookManager::BookManager() : db(nullptr) {
    booksDir = "/mnt/ext1/Digital Editions/Calibre";
}

BookManager::~BookManager() {
    if (db) {
        sqlite3_close((sqlite3*)db);
    }
}

bool BookManager::initialize(const std::string& dbPath) {
    // Create books directory
    iv_buildpath(booksDir.c_str());
    
    // Open database
    int rc = sqlite3_open(dbPath.c_str(), (sqlite3**)&db);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    return createTables();
}

bool BookManager::createTables() {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS books ("
        "  uuid TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  authors TEXT,"
        "  lpath TEXT NOT NULL UNIQUE,"
        "  series TEXT,"
        "  series_index INTEGER,"
        "  publisher TEXT,"
        "  pubdate TEXT,"
        "  last_modified TEXT,"
        "  tags TEXT,"
        "  comments TEXT,"
        "  size INTEGER,"
        "  thumbnail TEXT,"
        "  thumbnail_height INTEGER,"
        "  thumbnail_width INTEGER,"
        "  is_read INTEGER DEFAULT 0,"
        "  last_read_date TEXT,"
        "  is_favorite INTEGER DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_lpath ON books(lpath);"
        "CREATE INDEX IF NOT EXISTS idx_title ON books(title);";
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec((sqlite3*)db, sql, nullptr, nullptr, &errMsg);
    
    if (rc != SQLITE_OK) {
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        return false;
    }
    
    return true;
}

bool BookManager::addBook(const BookMetadata& metadata) {
    const char* sql = 
        "INSERT OR REPLACE INTO books "
        "(uuid, title, authors, lpath, series, series_index, publisher, "
        "pubdate, last_modified, tags, comments, size, thumbnail, "
        "thumbnail_height, thumbnail_width, is_read, last_read_date, is_favorite) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, metadata.uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, metadata.authors.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata.lpath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, metadata.series.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, metadata.seriesIndex);
    sqlite3_bind_text(stmt, 7, metadata.publisher.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, metadata.pubdate.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, metadata.lastModified.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, metadata.tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, metadata.comments.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 12, metadata.size);
    sqlite3_bind_text(stmt, 13, metadata.thumbnail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 14, metadata.thumbnailHeight);
    sqlite3_bind_int(stmt, 15, metadata.thumbnailWidth);
    sqlite3_bind_int(stmt, 16, metadata.isRead ? 1 : 0);
    sqlite3_bind_text(stmt, 17, metadata.lastReadDate.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 18, metadata.isFavorite ? 1 : 0);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool BookManager::updateBook(const BookMetadata& metadata) {
    return addBook(metadata);  // INSERT OR REPLACE handles updates
}

bool BookManager::deleteBook(const std::string& uuid) {
    // Get lpath first for file deletion
    BookMetadata metadata;
    if (getBook(uuid, metadata)) {
        deleteBookFile(metadata.lpath);
    }
    
    const char* sql = "DELETE FROM books WHERE uuid = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool BookManager::getBook(const std::string& uuid, BookMetadata& metadata) {
    const char* sql = 
        "SELECT uuid, title, authors, lpath, series, series_index, publisher, "
        "pubdate, last_modified, tags, comments, size, thumbnail, "
        "thumbnail_height, thumbnail_width, is_read, last_read_date, is_favorite "
        "FROM books WHERE uuid = ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        metadata.uuid = (const char*)sqlite3_column_text(stmt, 0);
        metadata.title = (const char*)sqlite3_column_text(stmt, 1);
        metadata.authors = (const char*)sqlite3_column_text(stmt, 2);
        metadata.lpath = (const char*)sqlite3_column_text(stmt, 3);
        metadata.series = (const char*)sqlite3_column_text(stmt, 4);
        metadata.seriesIndex = sqlite3_column_int(stmt, 5);
        metadata.publisher = (const char*)sqlite3_column_text(stmt, 6);
        metadata.pubdate = (const char*)sqlite3_column_text(stmt, 7);
        metadata.lastModified = (const char*)sqlite3_column_text(stmt, 8);
        metadata.tags = (const char*)sqlite3_column_text(stmt, 9);
        metadata.comments = (const char*)sqlite3_column_text(stmt, 10);
        metadata.size = sqlite3_column_int64(stmt, 11);
        metadata.thumbnail = (const char*)sqlite3_column_text(stmt, 12);
        metadata.thumbnailHeight = sqlite3_column_int(stmt, 13);
        metadata.thumbnailWidth = sqlite3_column_int(stmt, 14);
        metadata.isRead = sqlite3_column_int(stmt, 15) != 0;
        metadata.lastReadDate = (const char*)sqlite3_column_text(stmt, 16);
        metadata.isFavorite = sqlite3_column_int(stmt, 17) != 0;
        
        sqlite3_finalize(stmt);
        return true;
    }
    
    sqlite3_finalize(stmt);
    return false;
}

bool BookManager::bookExists(const std::string& uuid) {
    const char* sql = "SELECT 1 FROM books WHERE uuid = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_ROW;
}

std::vector<BookMetadata> BookManager::getAllBooks() {
    std::vector<BookMetadata> books;
    
    const char* sql = 
        "SELECT uuid, title, authors, lpath, series, series_index, publisher, "
        "pubdate, last_modified, tags, comments, size, thumbnail, "
        "thumbnail_height, thumbnail_width, is_read, last_read_date, is_favorite "
        "FROM books ORDER BY title";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return books;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookMetadata metadata;
        metadata.uuid = (const char*)sqlite3_column_text(stmt, 0);
        metadata.title = (const char*)sqlite3_column_text(stmt, 1);
        metadata.authors = (const char*)sqlite3_column_text(stmt, 2);
        metadata.lpath = (const char*)sqlite3_column_text(stmt, 3);
        metadata.series = (const char*)sqlite3_column_text(stmt, 4);
        metadata.seriesIndex = sqlite3_column_int(stmt, 5);
        metadata.publisher = (const char*)sqlite3_column_text(stmt, 6);
        metadata.pubdate = (const char*)sqlite3_column_text(stmt, 7);
        metadata.lastModified = (const char*)sqlite3_column_text(stmt, 8);
        metadata.tags = (const char*)sqlite3_column_text(stmt, 9);
        metadata.comments = (const char*)sqlite3_column_text(stmt, 10);
        metadata.size = sqlite3_column_int64(stmt, 11);
        metadata.thumbnail = (const char*)sqlite3_column_text(stmt, 12);
        metadata.thumbnailHeight = sqlite3_column_int(stmt, 13);
        metadata.thumbnailWidth = sqlite3_column_int(stmt, 14);
        metadata.isRead = sqlite3_column_int(stmt, 15) != 0;
        metadata.lastReadDate = (const char*)sqlite3_column_text(stmt, 16);
        metadata.isFavorite = sqlite3_column_int(stmt, 17) != 0;
        
        books.push_back(metadata);
    }
    
    sqlite3_finalize(stmt);
    return books;
}

int BookManager::getBookCount() {
    const char* sql = "SELECT COUNT(*) FROM books";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return count;
}

bool BookManager::hasMetadataChanged(const BookMetadata& metadata) {
    BookMetadata existing;
    if (!getBook(metadata.uuid, existing)) {
        return true;  // Book doesn't exist, metadata has "changed"
    }
    
    // Compare last_modified dates
    return metadata.lastModified != existing.lastModified;
}

void BookManager::updateMetadataCache(const BookMetadata& metadata) {
    updateBook(metadata);
}

std::string BookManager::sanitizeFileName(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || 
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            result += '_';
        } else {
            result += c;
        }
    }
    return result;
}

std::string BookManager::getBookFilePath(const std::string& lpath) {
    return booksDir + "/" + lpath;
}

bool BookManager::saveBookFile(const std::string& lpath, const void* data, size_t length) {
    std::string filePath = getBookFilePath(lpath);
    
    // Create subdirectories if needed
    size_t pos = filePath.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = filePath.substr(0, pos);
        iv_buildpath(dir.c_str());
    }
    
    FILE* file = iv_fopen(filePath.c_str(), "wb");
    if (!file) {
        return false;
    }
    
    size_t written = fwrite(data, 1, length, file);
    iv_fclose(file);
    
    return written == length;
}

bool BookManager::deleteBookFile(const std::string& lpath) {
    std::string filePath = getBookFilePath(lpath);
    return unlink(filePath.c_str()) == 0;
}

void BookManager::updateCollections(const std::map<std::string, std::vector<std::string>>& collections) {
    // TODO: Implement collection support
    // For now, collections can be stored as tags
}