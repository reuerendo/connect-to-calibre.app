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

#define LOG_MSG(fmt, ...) { FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a"); if(f) { fprintf(f, "[DB] " fmt "\n", ##__VA_ARGS__); fclose(f); } }

BookManager::BookManager() {
    booksDir = "/mnt/ext1"; 
}

BookManager::~BookManager() {
}

bool BookManager::initialize(const std::string& dbPath) {
    return true;
}

sqlite3* BookManager::openDB() {
    sqlite3* db;
    int rc = sqlite3_open_v2(SYSTEM_DB_PATH.c_str(), &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        LOG_MSG("Failed to open DB: %s", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

void BookManager::closeDB(sqlite3* db) {
    if (db) sqlite3_close(db);
}

int BookManager::getStorageId(const std::string& filename) {
    if (filename.find("/mnt/ext1") == 0) return 1;
    return 2;
}

std::string BookManager::getFirstLetter(const std::string& str) {
    if (str.empty()) return "";
    
    unsigned char first = (unsigned char)str[0];
    
    // Проверка на буквы (isalnum) и пунктуацию (ispunct)
    // Это приблизительный аналог Lua [%w%p] для ASCII.
    // Для кириллицы toupper работает корректно только с setlocale, 
    // но на устройстве обычно локаль utf8.
    if (isalnum(first) || ispunct(first)) {
        char upper = toupper(first);
        return std::string(1, upper);
    }
    
    // Fallback: str:sub(1,2):upper()
    std::string res = str.substr(0, 2);
    // Простой toupper для строки
    for (char & c : res) c = toupper((unsigned char)c);
    return res;
}

int BookManager::getCurrentProfileId(sqlite3* db) {
    char* profileName = GetCurrentProfile(); // InkView API
    if (!profileName) return 1; // Default profile ID is usually 1

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM profiles WHERE name = ?";
    int id = 1;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, profileName, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    if (profileName) free(profileName);
    return id;
}

int BookManager::getOrCreateFolder(sqlite3* db, const std::string& folderPath, int storageId) {
    int folderId = -1;
    const char* insertSql = "INSERT INTO folders (storageid, name) VALUES (?, ?) "
                            "ON CONFLICT(storageid, name) DO NOTHING";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, storageId);
        sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    const char* selectSql = "SELECT id FROM folders WHERE storageid = ? AND name = ?";
    if (sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, storageId);
        sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            folderId = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return folderId;
}

std::string BookManager::getBookFilePath(const std::string& lpath) {
    if (lpath.empty()) return "";
    std::string cleanLpath = (lpath[0] == '/') ? lpath.substr(1) : lpath;
    if (booksDir.back() == '/') return booksDir + cleanLpath;
    else return booksDir + "/" + cleanLpath;
}

// Основной метод добавления/обновления книги (порт saveBookToDatabase)
bool BookManager::addBook(const BookMetadata& metadata) {
    std::string fullPath = getBookFilePath(metadata.lpath);
    
    struct stat st;
    if (stat(fullPath.c_str(), &st) != 0) return false;

    sqlite3* db = openDB();
    if (!db) return false;

    std::string folderName, fileName;
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) {
        folderName = "";
        fileName = fullPath;
    } else {
        folderName = fullPath.substr(0, lastSlash);
        fileName = fullPath.substr(lastSlash + 1);
    }
    
    std::string fileExt = "";
    size_t lastDot = fileName.find_last_of('.');
    if (lastDot != std::string::npos) fileExt = fileName.substr(lastDot + 1);

    int storageId = getStorageId(fullPath);
    time_t now = time(NULL);

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    int folderId = getOrCreateFolder(db, folderName, storageId);
    if (folderId == -1) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        closeDB(db);
        return false;
    }

    const char* checkFileSql = "SELECT id, book_id FROM files WHERE filename = ? AND folder_id = ?";
    sqlite3_stmt* stmt;
    int fileId = -1;
    int bookId = -1;
    
    if (sqlite3_prepare_v2(db, checkFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, folderId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int(stmt, 0);
            bookId = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    // Используем authorSort если есть, иначе обычного автора
    std::string sortAuthor = metadata.authorSort.empty() ? metadata.authors : metadata.authorSort;
    std::string firstAuthorLetter = getFirstLetter(sortAuthor);
    std::string firstTitleLetter = getFirstLetter(metadata.title);

    if (fileId != -1) {
        // UPDATE
        const char* updateFileSql = "UPDATE files SET size = ?, modification_time = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db, updateFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (long long)st.st_size);
            sqlite3_bind_int64(stmt, 2, (long long)st.st_mtime);
            sqlite3_bind_int(stmt, 3, fileId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // При полном апдейте (файл перезаписан) мы обновляем ts_added как в pb-db.lua
        const char* updateBookSql = 
            "UPDATE books_impl SET title=?, first_title_letter=?, author=?, firstauthor=?, "
            "first_author_letter=?, series=?, numinseries=?, size=?, isbn=?, sort_title=?, "
            "updated=?, ts_added=? WHERE id=?";
            
        if (sqlite3_prepare_v2(db, updateBookSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, firstTitleLetter.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, metadata.authors.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, sortAuthor.c_str(), -1, SQLITE_TRANSIENT); // CORRECTED: Using sort author
            sqlite3_bind_text(stmt, 5, firstAuthorLetter.c_str(), -1, SQLITE_TRANSIENT); // CORRECTED: Using sort author letter
            sqlite3_bind_text(stmt, 6, metadata.series.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, metadata.seriesIndex);
            sqlite3_bind_int64(stmt, 8, metadata.size);
            sqlite3_bind_text(stmt, 9, metadata.isbn.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 11, now);
            sqlite3_bind_int64(stmt, 12, now); // Обновляем дату добавления при перезаписи файла
            sqlite3_bind_int(stmt, 13, bookId);
            
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        // INSERT
        const char* insertBookSql = 
            "INSERT INTO books_impl (title, first_title_letter, author, firstauthor, "
            "first_author_letter, series, numinseries, size, isbn, sort_title, creationtime, "
            "updated, ts_added, hidden) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
            
        if (sqlite3_prepare_v2(db, insertBookSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, firstTitleLetter.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, metadata.authors.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, sortAuthor.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, firstAuthorLetter.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, metadata.series.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, metadata.seriesIndex);
            sqlite3_bind_int64(stmt, 8, metadata.size);
            sqlite3_bind_text(stmt, 9, metadata.isbn.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 11, 0);
            sqlite3_bind_int(stmt, 12, 0);
            sqlite3_bind_int64(stmt, 13, now);
            sqlite3_bind_int(stmt, 14, 0);
            
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                bookId = sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(stmt);
        }

        if (bookId != -1) {
            const char* insertFileSql = 
                "INSERT INTO files (storageid, folder_id, book_id, filename, size, modification_time, ext) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)";
                
            if (sqlite3_prepare_v2(db, insertFileSql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, storageId);
                sqlite3_bind_int(stmt, 2, folderId);
                sqlite3_bind_int(stmt, 3, bookId);
                sqlite3_bind_text(stmt, 4, fileName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 5, (long long)st.st_size);
                sqlite3_bind_int64(stmt, 6, (long long)st.st_mtime);
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
    sqlite3_exec(db, "PRAGMA wal_checkpoint(FULL)", NULL, NULL, NULL);
    closeDB(db);
    return true;
}

// Метод для СИНХРОНИЗАЦИИ (вызывается при SEND_BOOK_METADATA)
// Полностью повторяет логику PocketBookDBHandler:updateBookMetadata из pb-db.lua
// НЕ обновляет books_impl (автора, название, дату добавления)
bool BookManager::updateBookSync(const BookMetadata& metadata) {
    sqlite3* db = openDB();
    if (!db) return false;

    // Находим ID книги
    int bookId = findBookIdByPath(db, metadata.lpath);
    
    if (bookId == -1) {
        LOG_MSG("Sync: Book not found in DB: %s", metadata.lpath.c_str());
        closeDB(db);
        return false;
    }

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // Обновляем ТОЛЬКО настройки (прочитано, избранное)
    int profileId = getCurrentProfileId(db);
    bool res = processBookSettings(db, bookId, metadata, profileId);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(FULL)", NULL, NULL, NULL);
    
    closeDB(db);
    return res;
}

bool BookManager::updateBook(const BookMetadata& metadata) {
    // Для SQLite Insert/Update логика одинакова (проверка на существование есть внутри addBook)
    return addBook(metadata);
}

// Реализация processBookSettings из pb-db.lua
bool BookManager::processBookSettings(sqlite3* db, int bookId, const BookMetadata& metadata, int profileId) {
    // В pb-db.lua логика: если нет статусов для обновления, выходим.
    // Но при sync metadata нам, возможно, прислали сброс статуса?
    // В Lua: if not read_lookup_name ... return. 
    // Мы полагаемся на то, что metadata.isRead/isFavorite установлены верно вызывающим кодом.
    
    int completed = metadata.isRead ? 1 : 0;
    int favorite = metadata.isFavorite ? 1 : 0;
    int cpage = metadata.isRead ? 100 : 0;
    time_t completedTs = metadata.isRead ? time(NULL) : 0;

    sqlite3_stmt* stmt;
    const char* checkSql = "SELECT bookid FROM books_settings WHERE bookid = ? AND profileid = ?";
    bool exists = false;
    
    if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, bookId);
        sqlite3_bind_int(stmt, 2, profileId);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        const char* updateSql = "UPDATE books_settings SET completed=?, favorite=?, completed_ts=?, cpage=? WHERE bookid=? AND profileid=?";
        if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, completed);
            sqlite3_bind_int(stmt, 2, favorite);
            sqlite3_bind_int64(stmt, 3, completedTs);
            sqlite3_bind_int(stmt, 4, cpage);
            sqlite3_bind_int(stmt, 5, bookId);
            sqlite3_bind_int(stmt, 6, profileId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        const char* insertSql = "INSERT INTO books_settings (bookid, profileid, completed, favorite, completed_ts, cpage) VALUES (?, ?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_bind_int(stmt, 2, profileId);
            sqlite3_bind_int(stmt, 3, completed);
            sqlite3_bind_int(stmt, 4, favorite);
            sqlite3_bind_int64(stmt, 5, completedTs);
            sqlite3_bind_int(stmt, 6, cpage);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return true;
}

bool BookManager::deleteBook(const std::string& lpath) {
    std::string filePath = getBookFilePath(lpath);
    LOG_MSG("Deleting book: %s", filePath.c_str());
    
    // 1. Удаляем физически
    remove(filePath.c_str());

    // 2. Чистим БД
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

    // Ищем ID по имени файла и папке
    const char* findSql = 
        "SELECT f.id, f.book_id FROM files f "
        "JOIN folders fo ON f.folder_id = fo.id "
        "WHERE f.filename = ? AND fo.name = ? AND f.storageid = ?";
        
    sqlite3_stmt* stmt;
    int fileId = -1;
    int bookId = -1;

    if (sqlite3_prepare_v2(db, findSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, folderName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, storageId);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int(stmt, 0);
            bookId = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    if (fileId != -1) {
        // Удаляем из files
        const char* sql1 = "DELETE FROM files WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql1, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, fileId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        // Удаляем из books_settings
        const char* sql2 = "DELETE FROM books_settings WHERE bookid = ?";
        if (sqlite3_prepare_v2(db, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        // Удаляем из books_impl
        const char* sql3 = "DELETE FROM books_impl WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql3, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db, "VACUUM", NULL, NULL, NULL);
    
    closeDB(db);
    return true;
}

// Реализация чтения всех книг для кэша (GET_BOOK_COUNT)
// Используется для первичной синхронизации
std::vector<BookMetadata> BookManager::getAllBooks() {
    std::vector<BookMetadata> books;
    sqlite3* db = openDB();
    if (!db) return books;

    // Выбираем данные из books_impl и соединяем с files/folders для получения полного пути
    // Также берем статус прочтения для текущего профиля
    int profileId = getCurrentProfileId(db);
    
    std::string sql = 
        "SELECT b.id, b.title, b.author, b.series, b.numinseries, b.size, b.updated, "
        "f.filename, fo.name, bs.completed, bs.favorite "
        "FROM books_impl b "
        "JOIN files f ON b.id = f.book_id "
        "JOIN folders fo ON f.folder_id = fo.id "
        "LEFT JOIN books_settings bs ON b.id = bs.bookid AND bs.profileid = ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, profileId);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            BookMetadata meta;
            meta.dbBookId = sqlite3_column_int(stmt, 0);
            
            const char* title = (const char*)sqlite3_column_text(stmt, 1);
            const char* author = (const char*)sqlite3_column_text(stmt, 2);
            const char* series = (const char*)sqlite3_column_text(stmt, 3);
            
            meta.title = title ? title : "";
            meta.authors = author ? author : "";
            meta.series = series ? series : "";
            meta.seriesIndex = sqlite3_column_int(stmt, 4);
            meta.size = sqlite3_column_int64(stmt, 5);
            
            // Формируем LPATH
            const char* filename = (const char*)sqlite3_column_text(stmt, 7);
            const char* folder = (const char*)sqlite3_column_text(stmt, 8);
            
            // В БД PocketBook папка хранится как полный путь /mnt/ext1/...
            // Нам нужен относительный путь для Calibre (lpath)
            std::string fullFolder = folder ? folder : "";
            std::string fName = filename ? filename : "";
            
            std::string fullPath = fullFolder + "/" + fName;
            
            // Превращаем /mnt/ext1/Books/Title.epub -> Books/Title.epub
            // так как booksDir = /mnt/ext1
            if (fullPath.find(booksDir) == 0) {
                meta.lpath = fullPath.substr(booksDir.length());
                if (!meta.lpath.empty() && meta.lpath[0] == '/') {
                    meta.lpath = meta.lpath.substr(1);
                }
            } else {
                meta.lpath = fName; // Fallback
            }
            
            // Статусы
            meta.isRead = (sqlite3_column_int(stmt, 9) == 1);
            meta.isFavorite = (sqlite3_column_int(stmt, 10) == 1);
            
            // UUID: В PocketBook нет нативного UUID поля для Calibre. 
            // Обычно используется lpath или генерируется хэш. 
            // Оставим пустым или равным lpath для сверки.
            meta.uuid = ""; 
            
            // Время обновления
            time_t updated = (time_t)sqlite3_column_int64(stmt, 6);
            char timeBuf[32];
            strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S+00:00", gmtime(&updated));
            meta.lastModified = timeBuf;

            books.push_back(meta);
        }
        sqlite3_finalize(stmt);
    }
    
    closeDB(db);
    return books;
}

int BookManager::getBookCount() {
    return getAllBooks().size(); // Неоптимально, но надежно для начала
}

// ================= КОЛЛЕКЦИИ (BOOKSHELVES) =================

// Поиск ID книги по пути (lpath)
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
    
    // ВАЖНО: folder.name в БД PocketBook - это абсолютный путь без имени файла
    // Например: /mnt/ext1/Books
    
    const char* sql = "SELECT f.book_id FROM files f JOIN folders fo ON f.folder_id = fo.id WHERE f.filename = ? AND fo.name = ?";
    sqlite3_stmt* stmt;
    int bookId = -1;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, folderName.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            bookId = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return bookId;
}

// Получить или создать полку
int BookManager::getOrCreateBookshelf(sqlite3* db, const std::string& name) {
    int shelfId = -1;
    time_t now = time(NULL);
    
    // Проверяем существование
    const char* findSql = "SELECT id FROM bookshelfs WHERE name = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, findSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            shelfId = sqlite3_column_int(stmt, 0);
            // Если была удалена, восстанавливаем
            const char* restoreSql = "UPDATE bookshelfs SET is_deleted = 0, ts = ? WHERE id = ?";
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
        const char* insertSql = "INSERT INTO bookshelfs (name, is_deleted, ts) VALUES (?, 0, ?)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, now);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                shelfId = sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(stmt);
        }
    }
    return shelfId;
}

// Привязка книги к полке
void BookManager::linkBookToShelf(sqlite3* db, int shelfId, int bookId) {
    time_t now = time(NULL);
    
    // Проверяем, есть ли связь
    const char* checkSql = "SELECT 1 FROM bookshelfs_books WHERE bookshelfid = ? AND bookid = ?";
    bool exists = false;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, shelfId);
        sqlite3_bind_int(stmt, 2, bookId);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = true;
        sqlite3_finalize(stmt);
    }
    
    if (exists) {
        // Обновляем (восстанавливаем если удалена)
        const char* updateSql = "UPDATE bookshelfs_books SET is_deleted = 0, ts = ? WHERE bookshelfid = ? AND bookid = ?";
        if (sqlite3_prepare_v2(db, updateSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_int(stmt, 2, shelfId);
            sqlite3_bind_int(stmt, 3, bookId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        // Создаем
        const char* insertSql = "INSERT INTO bookshelfs_books (bookshelfid, bookid, ts, is_deleted) VALUES (?, ?, ?, 0)";
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, shelfId);
            sqlite3_bind_int(stmt, 2, bookId);
            sqlite3_bind_int64(stmt, 3, now);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

// Основной метод синхронизации коллекций
void BookManager::updateCollections(const std::map<std::string, std::vector<std::string>>& collections) {
    sqlite3* db = openDB();
    if (!db) return;
    
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    
    // C++11 loop (fix for structured bindings)
    for (auto const& entry : collections) {
        const std::string& colName = entry.first;
        const std::vector<std::string>& lpaths = entry.second;
        
        LOG_MSG("Processing collection: %s with %d books", colName.c_str(), (int)lpaths.size());
        
        int shelfId = getOrCreateBookshelf(db, colName);
        if (shelfId == -1) continue;
        
        for (const std::string& lpath : lpaths) {
            int bookId = findBookIdByPath(db, lpath);
            if (bookId != -1) {
                linkBookToShelf(db, shelfId, bookId);
            } else {
                LOG_MSG("Book not found for collection: %s", lpath.c_str());
            }
        }
    }
    
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    closeDB(db);
}
