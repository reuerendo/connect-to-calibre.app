#include "calibre_protocol.h"
#include <sys/stat.h>
#include <errno.h>
#include <vector>
#include <algorithm>
#include "inkview.h"
#include <json-c/json.h>
#include <openssl/sha.h>
#include <sys/statvfs.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <memory>

// Constants synchronized with driver.py
static const int BASE_PACKET_LEN = 4096;
static const int COVER_HEIGHT = 240;
static const int DEFAULT_PATH_LENGTH = 37;
static const int PROTOCOL_VERSION = 1;

// Helper for logging with levels
enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_ERROR };

static void logProto(LogLevel level, const char* fmt, ...) {
    FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a");
    if (f) {
        const char* prefix[] = {"[DEBUG]", "[INFO]", "[ERROR]"};
        fprintf(f, "%s ", prefix[level]);
        
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

// RAII wrapper for FILE*
class FileHandle {
    FILE* file;
public:
    explicit FileHandle(const char* path, const char* mode) : file(iv_fopen(path, mode)) {}
    ~FileHandle() { if (file) iv_fclose(file); }
    FILE* get() const { return file; }
    FILE* release() { FILE* f = file; file = nullptr; return f; }
    operator bool() const { return file != nullptr; }
};

// RAII wrapper for sqlite3_stmt
class StmtHandle {
    sqlite3_stmt* stmt;
public:
    explicit StmtHandle(sqlite3_stmt* s = nullptr) : stmt(s) {}
    ~StmtHandle() { if (stmt) sqlite3_finalize(stmt); }
    sqlite3_stmt* get() const { return stmt; }
    sqlite3_stmt** ptr() { return &stmt; }
    operator bool() const { return stmt != nullptr; }
};

static int recursiveMkdir(const std::string& path) {
    std::string current_path;
    std::string path_copy = path;
    
    if (!path_copy.empty() && path_copy.back() == '/') {
        path_copy.pop_back();
    }

    size_t pos = 0;
    if (!path_copy.empty() && path_copy[0] == '/') {
        current_path = "/";
        pos = 1;
    }

    while (pos < path_copy.length()) {
        size_t next_slash = path_copy.find('/', pos);
        std::string part;
        
        if (next_slash == std::string::npos) {
            part = path_copy.substr(pos);
            pos = path_copy.length();
        } else {
            part = path_copy.substr(pos, next_slash - pos);
            pos = next_slash + 1;
        }
        
        if (part.empty()) continue;

        if (current_path.length() > 0 && current_path.back() != '/') {
            current_path += "/";
        }
        current_path += part;

        if (mkdir(current_path.c_str(), 0755) != 0) {
            if (errno != EEXIST) {
                logProto(LOG_ERROR, "Failed to create directory %s: %s", 
                        current_path.c_str(), strerror(errno));
                return -1;
            }
        }
    }
    return 0;
}

static std::string safeGetJsonString(json_object* val) {
    if (!val) return "";
    if (json_object_get_type(val) == json_type_null) return "";
    const char* str = json_object_get_string(val);
    return str ? std::string(str) : "";
}

CalibreProtocol::CalibreProtocol(NetworkManager* net, BookManager* bookMgr,
                                 CacheManager* cacheMgr,
                                 const std::string& readCol, 
                                 const std::string& readDateCol, 
                                 const std::string& favCol) 
    : network(net), bookManager(bookMgr), cacheManager(cacheMgr),
      connected(false),
      readColumn(readCol), readDateColumn(readDateCol), favoriteColumn(favCol),
      currentBookLength(0), currentBookReceived(0), currentBookFile(nullptr),
      booksReceivedInSession(0) {
    
    const char* model = GetDeviceModel();
    if (model && strlen(model) > 0) {
        deviceName = std::string("PocketBook ") + model;
    } else {
        deviceName = "PocketBook Device";
    }
    
    appVersion = "1.0.0";
    
    logProto(LOG_INFO, "Device name: %s", deviceName.c_str());
}

CalibreProtocol::~CalibreProtocol() {
    disconnect();
}

std::string CalibreProtocol::getPasswordHash(const std::string& password, 
                                             const std::string& challenge) {
    if (challenge.empty()) {
        return "";
    }
    
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, password.c_str(), password.length());
    SHA1_Update(&ctx, challenge.c_str(), challenge.length());
    
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1_Final(hash, &ctx);
    
    std::stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    return ss.str();
}

json_object* CalibreProtocol::createDeviceInfo() {
    json_object* info = json_object_new_object();
    
    json_object* extensions = json_object_new_array();
    const char* supportedFormats[] = {
        "epub", "pdf", "mobi", "azw3", "fb2", "txt", "djvu", "cbz", "cbr"
    };
    
    for (size_t i = 0; i < sizeof(supportedFormats) / sizeof(supportedFormats[0]); i++) {
        json_object_array_add(extensions, json_object_new_string(supportedFormats[i]));
    }
    
    json_object* pathLengths = json_object_new_object();
    for (size_t i = 0; i < sizeof(supportedFormats) / sizeof(supportedFormats[0]); i++) {
        json_object_object_add(pathLengths, supportedFormats[i], 
                              json_object_new_int(DEFAULT_PATH_LENGTH));
    }
    
    json_object_object_add(info, "appName", json_object_new_string("PocketBook Calibre Companion"));
    json_object_object_add(info, "acceptedExtensions", extensions);
    json_object_object_add(info, "cacheUsesLpaths", json_object_new_boolean(true));
    json_object_object_add(info, "canAcceptLibraryInfo", json_object_new_boolean(true));
    json_object_object_add(info, "canDeleteMultipleBooks", json_object_new_boolean(true));
    json_object_object_add(info, "canReceiveBookBinary", json_object_new_boolean(true));
    json_object_object_add(info, "canSendOkToSendbook", json_object_new_boolean(true));
    json_object_object_add(info, "canStreamBooks", json_object_new_boolean(true));
    json_object_object_add(info, "canStreamMetadata", json_object_new_boolean(true));
    json_object_object_add(info, "canUseCachedMetadata", json_object_new_boolean(true));
    json_object_object_add(info, "canSupportLpathChanges", json_object_new_boolean(true));
    json_object_object_add(info, "willAskForUpdateBooks", json_object_new_boolean(false));
    json_object_object_add(info, "setTempMarkWhenReadInfoSynced", json_object_new_boolean(false));
    json_object_object_add(info, "ccVersionNumber", json_object_new_string(appVersion.c_str()));
    json_object_object_add(info, "coverHeight", json_object_new_int(COVER_HEIGHT));
    json_object_object_add(info, "deviceKind", json_object_new_string("PocketBook"));
    json_object_object_add(info, "deviceName", json_object_new_string(deviceName.c_str()));
    json_object_object_add(info, "extensionPathLengths", pathLengths);
    json_object_object_add(info, "maxBookContentPacketLen", json_object_new_int(BASE_PACKET_LEN));
    json_object_object_add(info, "useUuidFileNames", json_object_new_boolean(false));
    json_object_object_add(info, "versionOK", json_object_new_boolean(true));
    
    if (!readColumn.empty()) {
        json_object_object_add(info, "isReadSyncCol", 
                              json_object_new_string(readColumn.c_str()));
    }
    
    if (!readDateColumn.empty()) {
        json_object_object_add(info, "isReadDateSyncCol", 
                              json_object_new_string(readDateColumn.c_str()));
    }
    
    return info;
}

bool CalibreProtocol::performHandshake(const std::string& password) {
    CalibreOpcode opcode;
    std::string jsonData;
    
    if (!network->receiveJSON(opcode, jsonData)) {
        errorMessage = "Failed to receive initialization request";
        return false;
    }
    
    if (opcode != GET_INITIALIZATION_INFO) {
        errorMessage = "Unexpected opcode during handshake";
        return false;
    }
    
    json_object* request = parseJSON(jsonData);
    if (!request) {
        errorMessage = "Failed to parse initialization request";
        return false;
    }
    
    json_object* challengeObj = NULL;
    json_object_object_get_ex(request, "passwordChallenge", &challengeObj);
    std::string challenge = challengeObj ? json_object_get_string(challengeObj) : "";
    
    json_object* response = createDeviceInfo();
    
    if (!challenge.empty()) {
        std::string hash = getPasswordHash(password, challenge);
        json_object_object_add(response, "passwordHash", 
                              json_object_new_string(hash.c_str()));
    }
    
    std::string responseStr = jsonToString(response);
    bool success = network->sendJSON(OK, responseStr.c_str());
    
    freeJSON(request);
    freeJSON(response);
    
    if (!success) {
        errorMessage = "Failed to send initialization response";
        return false;
    }
    
    if (!network->receiveJSON(opcode, jsonData)) {
        errorMessage = "Failed to receive response after initialization";
        return false;
    }
    
    if (opcode == DISPLAY_MESSAGE) {
        json_object* msg = parseJSON(jsonData);
        if (msg) {
            json_object* kindObj = NULL;
            json_object_object_get_ex(msg, "messageKind", &kindObj);
            if (kindObj && json_object_get_int(kindObj) == 1) {
                errorMessage = "Invalid password";
                freeJSON(msg);
                return false;
            }
            freeJSON(msg);
        }
        errorMessage = "Received unexpected message from Calibre";
        return false;
    }
    
    if (opcode != GET_DEVICE_INFORMATION) {
        errorMessage = "Unexpected opcode after initialization";
        return false;
    }
    
    json_object* deviceInfo = json_object_new_object();
    json_object* deviceData = json_object_new_object();
    
    const char* uuid = ReadString(GetGlobalConfig(), "calibre_device_uuid", "");
    if (strlen(uuid) == 0) {
        char uuidBuf[64];
        srand(time(NULL));
        snprintf(uuidBuf, sizeof(uuidBuf), "%08x-%04x-%04x-%04x-%012llx",
                (unsigned int)rand(), rand() & 0xFFFF, rand() & 0xFFFF, 
                rand() & 0xFFFF, (unsigned long long)rand() * rand());
        WriteString(GetGlobalConfig(), "calibre_device_uuid", uuidBuf);
        SaveConfig(GetGlobalConfig());
        uuid = ReadString(GetGlobalConfig(), "calibre_device_uuid", "");
    }
    
    deviceUuid = uuid;
    
    if (cacheManager) {
        cacheManager->initialize(deviceUuid);
    }
    
    json_object_object_add(deviceData, "device_store_uuid", 
                          json_object_new_string(uuid));
    json_object_object_add(deviceData, "device_name", 
                          json_object_new_string(deviceName.c_str()));
    
    json_object_object_add(deviceInfo, "device_info", deviceData);
    json_object_object_add(deviceInfo, "version", 
                          json_object_new_string(appVersion.c_str()));
    json_object_object_add(deviceInfo, "device_version", 
                          json_object_new_string(appVersion.c_str()));
    
    responseStr = jsonToString(deviceInfo);
    success = network->sendJSON(OK, responseStr.c_str());
    
    freeJSON(deviceInfo);
    
    if (!success) {
        errorMessage = "Failed to send device information";
        return false;
    }
    
    connected = true;
    return true;
}

void CalibreProtocol::handleMessages(std::function<void(const std::string&)> statusCallback) {
    while (connected && network->isConnected()) {
        CalibreOpcode opcode;
        std::string jsonData;
        
        if (!network->receiveJSON(opcode, jsonData)) {
            if (network->isConnected()) {
                errorMessage = "Connection lost";
            }
            connected = false;
            break;
        }
        
        logProto(LOG_DEBUG, "Received opcode %d", (int)opcode);
        
        json_object* args = parseJSON(jsonData);
        if (!args) {
            sendErrorResponse("Failed to parse request");
            continue;
        }
        
        bool shouldDisconnect = false;
        
        switch (opcode) {
            case SET_CALIBRE_DEVICE_INFO:
                handleSetCalibreInfo(args);
                statusCallback("Received device info");
                break;
                
            case FREE_SPACE:
                handleFreeSpace(args);
                statusCallback("Sent free space info");
                break;
                
            case TOTAL_SPACE:
                handleTotalSpace(args);
                statusCallback("Sent total space info");
                break;
                
            case SET_LIBRARY_INFO:
                handleSetLibraryInfo(args);
                statusCallback("Received library info");
                break;
                
            case GET_BOOK_COUNT:
                handleGetBookCount(args);
                statusCallback("Sent book count");
                break;
                
            case SEND_BOOKLISTS:
                handleSendBooklists(args);
                statusCallback("Processing booklists");
                break;
                
            case SEND_BOOK:
                handleSendBook(args);
                statusCallback("BOOK_SAVED");
                break;
                
            case SEND_BOOK_METADATA:
                handleSendBookMetadata(args);
                statusCallback("Received book metadata");
                break;
                
            case DELETE_BOOK:
                handleDeleteBook(args);
                statusCallback("Deleted book");
                break;
                
            case GET_BOOK_FILE_SEGMENT:
                handleGetBookFileSegment(args);
                statusCallback("Sent book file");
                break;
                
            case DISPLAY_MESSAGE:
                handleDisplayMessage(args);
                break;
                
            case BOOK_DONE:
                logProto(LOG_INFO, "Received BOOK_DONE");
                break;
                
            case NOOP: {
                handleNoop(args);
                json_object* ejectingObj = NULL;
                json_object_object_get_ex(args, "ejecting", &ejectingObj);
                if (ejectingObj && json_object_get_boolean(ejectingObj)) {
                    shouldDisconnect = true;
                }
                break;
            }
                
            default:
                logProto(LOG_ERROR, "Unexpected opcode: %d", (int)opcode);
                sendErrorResponse("Unexpected opcode");
                break;
        }
        
        freeJSON(args);
        
        if (shouldDisconnect) {
            connected = false;
            logProto(LOG_INFO, "Clean disconnect");
            return;
        }
    }
}

void CalibreProtocol::disconnect() {
    if (connected) {
        json_object* noopData = json_object_new_object();
        std::string noopStr = jsonToString(noopData);
        network->sendJSON(OK, noopStr.c_str());
        freeJSON(noopData);
        connected = false;
    }
    
    if (currentBookFile) {
        iv_fclose(currentBookFile);
        currentBookFile = nullptr;
    }
    
    if (cacheManager) {
        cacheManager->saveCache();
    }
}

bool CalibreProtocol::handleSetCalibreInfo(json_object* args) {
    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleTotalSpace(json_object* args) {
    struct statvfs stat;
    if (statvfs("/mnt/ext1", &stat) != 0) {
        return sendErrorResponse("Failed to get total space");
    }
    
    unsigned long long totalSpace = (unsigned long long)stat.f_blocks * stat.f_frsize;
    
    json_object* response = json_object_new_object();
    json_object_object_add(response, "total_space_on_device", 
                          json_object_new_int64(totalSpace));
    
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleFreeSpace(json_object* args) {
    struct statvfs stat;
    if (statvfs("/mnt/ext1", &stat) != 0) {
        return sendErrorResponse("Failed to get free space");
    }
    
    unsigned long long freeSpace = (unsigned long long)stat.f_bavail * stat.f_frsize;
    
    json_object* response = json_object_new_object();
    json_object_object_add(response, "free_space_on_device", 
                          json_object_new_int64(freeSpace));
    
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleSetLibraryInfo(json_object* args) {
    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleGetBookCount(json_object* args) {
    sessionBooks = bookManager->getAllBooks();
    int count = sessionBooks.size();
    
    bool useCache = false;
    json_object* cacheObj = NULL;
    if (json_object_object_get_ex(args, "willUseCachedMetadata", &cacheObj)) {
        useCache = json_object_get_boolean(cacheObj);
    }
    
    if (cacheManager) {
        int matched = 0;
        for (auto& book : sessionBooks) {
            std::string cachedUuid = cacheManager->getUuidForLpath(book.lpath);
            if (!cachedUuid.empty()) {
                book.uuid = cachedUuid;
                matched++;
            }
        }
        logProto(LOG_INFO, "UUID Patching: %d/%d books matched in cache", matched, count);
    }
    
    logProto(LOG_INFO, "GetBookCount: %d books, useCache=%d", count, useCache);

    json_object* response = json_object_new_object();
    json_object_object_add(response, "count", json_object_new_int(count));
    json_object_object_add(response, "willStream", json_object_new_boolean(true));
    json_object_object_add(response, "willScan", json_object_new_boolean(true));
    
    if (!sendOKResponse(response)) {
        freeJSON(response);
        return false;
    }
    freeJSON(response);
    
    for (int i = 0; i < count; i++) {
        json_object* bookJson = NULL;
        
        if (useCache) {
            bookJson = cachedMetadataToJson(sessionBooks[i], i);
        } else {
            bookJson = metadataToJson(sessionBooks[i]);
            json_object_object_add(bookJson, "priKey", json_object_new_int(i));
        }
        
        std::string bookStr = jsonToString(bookJson);
        if (!network->sendJSON(OK, bookStr.c_str())) {
            freeJSON(bookJson);
            return false;
        }
        freeJSON(bookJson);
    }
    
    return true;
}

static std::string cleanCollectionName(const std::string& rawName) {
    if (rawName.empty() || rawName.back() != ')') {
        return rawName;
    }

    size_t lastOpen = rawName.rfind('(');
    
    if (lastOpen != std::string::npos && lastOpen > 0 && rawName[lastOpen - 1] == ' ') {
        return rawName.substr(0, lastOpen - 1);
    }
    
    return rawName;
}

bool CalibreProtocol::handleSendBooklists(json_object* args) {
    json_object* collectionsObj = NULL;
    if (!json_object_object_get_ex(args, "collections", &collectionsObj)) {
        return true;
    }
    
    logProto(LOG_INFO, "Starting collection sync");
    
    std::map<std::string, std::set<std::string>> calibreCollections;
    
    json_object_object_foreach(collectionsObj, key, val) {
        std::string cleanName = cleanCollectionName(key);
        
        std::set<std::string> lpaths;
        int arrayLen = json_object_array_length(val);
        for (int i = 0; i < arrayLen; i++) {
            json_object* lpathObj = json_object_array_get_idx(val, i);
            lpaths.insert(json_object_get_string(lpathObj));
        }
        
        calibreCollections[cleanName] = lpaths;
        logProto(LOG_DEBUG, "Calibre collection '%s' has %d books", 
                cleanName.c_str(), (int)lpaths.size());
    }
    
    std::map<std::string, std::set<std::string>> deviceCollections;
    sqlite3* db = bookManager->openDB();
    if (!db) {
        logProto(LOG_ERROR, "Failed to open DB for collection sync");
        return false;
    }
    
    const char* sql = 
        "SELECT bs.name, f.filename, fo.name "
        "FROM bookshelfs bs "
        "JOIN bookshelfs_books bb ON bs.id = bb.bookshelfid "
        "JOIN books_impl b ON bb.bookid = b.id "
        "JOIN files f ON b.id = f.book_id "
        "JOIN folders fo ON f.folder_id = fo.id "
        "WHERE bs.is_deleted = 0 AND bb.is_deleted = 0";
    
    StmtHandle stmt;
    if (sqlite3_prepare_v2(db, sql, -1, stmt.ptr(), nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const char* shelfName = (const char*)sqlite3_column_text(stmt.get(), 0);
            const char* fileName = (const char*)sqlite3_column_text(stmt.get(), 1);
            const char* folderName = (const char*)sqlite3_column_text(stmt.get(), 2);
            
            if (shelfName && fileName && folderName) {
                std::string fullPath = std::string(folderName) + "/" + fileName;
                
                std::string lpath = fullPath;
                if (lpath.find("/mnt/ext1/") == 0) {
                    lpath = lpath.substr(10);
                }
                
                deviceCollections[shelfName].insert(lpath);
            }
        }
    }
    
    logProto(LOG_INFO, "Found %d collections on device", (int)deviceCollections.size());
    
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    time_t now = time(NULL);
    
    for (const auto& calibreEntry : calibreCollections) {
        const std::string& collectionName = calibreEntry.first;
        const std::set<std::string>& calibreFiles = calibreEntry.second;
        
        int shelfId = bookManager->getOrCreateBookshelf(db, collectionName);
        if (shelfId == -1) {
            logProto(LOG_ERROR, "Failed to get/create shelf: %s", collectionName.c_str());
            continue;
        }
        
        auto deviceIt = deviceCollections.find(collectionName);
        
        if (deviceIt != deviceCollections.end()) {
            const std::set<std::string>& deviceFiles = deviceIt->second;
            
            std::vector<std::string> toAdd;
            std::set_difference(calibreFiles.begin(), calibreFiles.end(),
                              deviceFiles.begin(), deviceFiles.end(),
                              std::back_inserter(toAdd));
            
            std::vector<std::string> toRemove;
            std::set_difference(deviceFiles.begin(), deviceFiles.end(),
                              calibreFiles.begin(), calibreFiles.end(),
                              std::back_inserter(toRemove));
            
            logProto(LOG_DEBUG, "Collection '%s': %d to add, %d to remove", 
                    collectionName.c_str(), (int)toAdd.size(), (int)toRemove.size());
            
            // Batch insert
            if (!toAdd.empty()) {
                const char* insertSql = 
                    "INSERT OR IGNORE INTO bookshelfs_books (bookshelfid, bookid, is_deleted, ts) "
                    "VALUES (?, ?, 0, ?)";
                StmtHandle insertStmt;
                if (sqlite3_prepare_v2(db, insertSql, -1, insertStmt.ptr(), nullptr) == SQLITE_OK) {
                    for (const std::string& lpath : toAdd) {
                        int bookId = bookManager->findBookIdByPath(db, lpath);
                        if (bookId != -1) {
                            sqlite3_reset(insertStmt.get());
                            sqlite3_bind_int(insertStmt.get(), 1, shelfId);
                            sqlite3_bind_int(insertStmt.get(), 2, bookId);
                            sqlite3_bind_int64(insertStmt.get(), 3, now);
                            sqlite3_step(insertStmt.get());
                        }
                    }
                }
            }
            
            // Batch delete
            if (!toRemove.empty()) {
                const char* deleteSql = 
                    "UPDATE bookshelfs_books SET is_deleted = 1, ts = ? "
                    "WHERE bookshelfid = ? AND bookid = ?";
                StmtHandle deleteStmt;
                if (sqlite3_prepare_v2(db, deleteSql, -1, deleteStmt.ptr(), nullptr) == SQLITE_OK) {
                    for (const std::string& lpath : toRemove) {
                        int bookId = bookManager->findBookIdByPath(db, lpath);
                        if (bookId != -1) {
                            sqlite3_reset(deleteStmt.get());
                            sqlite3_bind_int64(deleteStmt.get(), 1, now);
                            sqlite3_bind_int(deleteStmt.get(), 2, shelfId);
                            sqlite3_bind_int(deleteStmt.get(), 3, bookId);
                            sqlite3_step(deleteStmt.get());
                        }
                    }
                }
            }
            
            deviceCollections.erase(deviceIt);
            
        } else {
            logProto(LOG_INFO, "Creating new collection: %s with %d books", 
                    collectionName.c_str(), (int)calibreFiles.size());
            
            const char* insertSql = 
                "INSERT OR IGNORE INTO bookshelfs_books (bookshelfid, bookid, is_deleted, ts) "
                "VALUES (?, ?, 0, ?)";
            StmtHandle insertStmt;
            if (sqlite3_prepare_v2(db, insertSql, -1, insertStmt.ptr(), nullptr) == SQLITE_OK) {
                for (const std::string& lpath : calibreFiles) {
                    int bookId = bookManager->findBookIdByPath(db, lpath);
                    if (bookId != -1) {
                        sqlite3_reset(insertStmt.get());
                        sqlite3_bind_int(insertStmt.get(), 1, shelfId);
                        sqlite3_bind_int(insertStmt.get(), 2, bookId);
                        sqlite3_bind_int64(insertStmt.get(), 3, now);
                        sqlite3_step(insertStmt.get());
                    }
                }
            }
        }
    }
    
    for (const auto& deviceEntry : deviceCollections) {
        const std::string& collectionName = deviceEntry.first;
        
        logProto(LOG_INFO, "Removing collection no longer in Calibre: %s", 
                collectionName.c_str());
        
        const char* deleteSql = "UPDATE bookshelfs SET is_deleted = 1, ts = ? WHERE name = ?";
        StmtHandle deleteStmt;
        if (sqlite3_prepare_v2(db, deleteSql, -1, deleteStmt.ptr(), nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(deleteStmt.get(), 1, now);
            sqlite3_bind_text(deleteStmt.get(), 2, collectionName.c_str
			(), -1, SQLITE_TRANSIENT);
sqlite3_step(deleteStmt.get());
}
}

sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
sqlite3_exec(db, "PRAGMA wal_checkpoint(FULL)", NULL, NULL, NULL);

bookManager->closeDB(db);

logProto(LOG_INFO, "Collection sync completed");
return true;
}

std::string CalibreProtocol::parseJsonStringOrArray(json_object* val) {
if (!val || json_object_get_type(val) == json_type_null) return "";

enum json_type type = json_object_get_type(val);

if (type == json_type_string) {
    return safeGetJsonString(val);
} 
else if (type == json_type_array) {
    std::string result;
    result.reserve(256);
    int len = json_object_array_length(val);
    for (int i = 0; i < len; i++) {
        json_object* item = json_object_array_get_idx(val, i);
        if (i > 0) result += ", ";
        const char* str = json_object_get_string(item);
        if (str) {
            result += str;
        }
    }
    return result;
}

return "";

}

static bool getUserMetadataBool(json_object* userMeta, const std::string& colName) {
if (!userMeta || colName.empty()) return false;

json_object* colObj = NULL;
if (json_object_object_get_ex(userMeta, colName.c_str(), &colObj)) {
    json_object* valObj = NULL;
    if (json_object_object_get_ex(colObj, "#value#", &valObj)) {
        return json_object_get_boolean(valObj);
    }
}
return false;

}

static std::string getUserMetadataString(json_object* userMeta, const std::string& colName) {
if (!userMeta || colName.empty()) return "";

json_object* colObj = NULL;
if (json_object_object_get_ex(userMeta, colName.c_str(), &colObj)) {
    json_object* valObj = NULL;
    if (json_object_object_get_ex(colObj, "#value#", &valObj)) {
        const char* str = json_object_get_string(valObj);
        return str ? std::string(str) : "";
    }
}
return "";

}

BookMetadata CalibreProtocol::jsonToMetadata(json_object* obj) {
BookMetadata metadata;
json_object* val = NULL;

if (json_object_object_get_ex(obj, "uuid", &val)) metadata.uuid = safeGetJsonString(val);
if (json_object_object_get_ex(obj, "title", &val)) metadata.title = safeGetJsonString(val);
if (json_object_object_get_ex(obj, "authors", &val)) metadata.authors = parseJsonStringOrArray(val);
if (json_object_object_get_ex(obj, "author_sort", &val)) metadata.authorSort = safeGetJsonString(val);
if (json_object_object_get_ex(obj, "lpath", &val)) metadata.lpath = safeGetJsonString(val);
if (json_object_object_get_ex(obj, "series", &val)) metadata.series = safeGetJsonString(val);
if (json_object_object_get_ex(obj, "series_index", &val)) metadata.seriesIndex = json_object_get_int(val);
if (json_object_object_get_ex(obj, "size", &val)) metadata.size = json_object_get_int64(val);
if (json_object_object_get_ex(obj, "last_modified", &val)) metadata.lastModified = safeGetJsonString(val);

json_object* userMeta = NULL;
if (json_object_object_get_ex(obj, "user_metadata", &userMeta)) {
    if (!readColumn.empty()) {
        metadata.isRead = getUserMetadataBool(userMeta, readColumn);
    }
    
    if (!readDateColumn.empty()) {
        metadata.lastReadDate = getUserMetadataString(userMeta, readDateColumn);
    }
    
    if (!favoriteColumn.empty()) {
        metadata.isFavorite = getUserMetadataBool(userMeta, favoriteColumn);
    }
}

return metadata;

}

json_object* CalibreProtocol::metadataToJson(const BookMetadata& metadata) {
json_object* obj = json_object_new_object();

json_object_object_add(obj, "uuid", json_object_new_string(metadata.uuid.c_str()));
json_object_object_add(obj, "title", json_object_new_string(metadata.title.c_str()));
json_object_object_add(obj, "authors", json_object_new_string(metadata.authors.c_str()));
json_object_object_add(obj, "lpath", json_object_new_string(metadata.lpath.c_str()));
json_object_object_add(obj, "last_modified", json_object_new_string(metadata.lastModified.c_str()));
json_object_object_add(obj, "size", json_object_new_int64(metadata.size));

if (!metadata.series.empty()) {
    json_object_object_add(obj, "series", json_object_new_string(metadata.series.c_str()));
    json_object_object_add(obj, "series_index", json_object_new_int(metadata.seriesIndex));
}

json_object_object_add(obj, "_is_read_", json_object_new_boolean(metadata.isRead));
json_object_object_add(obj, "_sync_type_", json_object_new_int(1));

if (!metadata.lastReadDate.empty()) {
    json_object_object_add(obj, "_last_read_date_", 
                          json_object_new_string(metadata.lastReadDate.c_str()));
}

return obj;

}

bool CalibreProtocol::handleSendBook(json_object* args) {
logProto(LOG_INFO, "Starting handleSendBook");

json_object* metadataObj = NULL;
json_object* lpathObj = NULL;
json_object* lengthObj = NULL;

if (!json_object_object_get_ex(args, "lpath", &lpathObj) ||
    !json_object_object_get_ex(args, "length", &lengthObj) ||
    !json_object_object_get_ex(args, "metadata", &metadataObj)) {
    return sendErrorResponse("Missing required fields");
}

currentBookLpath = json_object_get_string(lpathObj);
currentBookLength = json_object_get_int64(lengthObj);
currentBookReceived = 0;

logProto(LOG_INFO, "Receiving book: %s (%lld bytes)", 
        currentBookLpath.c_str(), currentBookLength);

BookMetadata metadata = jsonToMetadata(metadataObj);
metadata.lpath = currentBookLpath;
metadata.size = currentBookLength;

std::string filePath = bookManager->getBookFilePath(currentBookLpath);
logProto(LOG_DEBUG, "Target path: %s", filePath.c_str());

size_t pos = filePath.rfind('/');
if (pos != std::string::npos) {
    std::string dir = filePath.substr(0, pos);
    if (recursiveMkdir(dir) != 0) {
        logProto(LOG_ERROR, "Failed to create directory structure for book");
        return sendErrorResponse("Failed to create directory");
    }
}

currentBookFile = iv_fopen(filePath.c_str(), "wb");
if (!currentBookFile) {
    logProto(LOG_ERROR, "Failed to open file for writing!");
    return sendErrorResponse("Failed to create book file");
}

json_object* response = json_object_new_object();
json_object_object_add(response, "lpath", json_object_new_string(currentBookLpath.c_str()));

if (!sendOKResponse(response)) {
    logProto(LOG_ERROR, "Failed to send OK response");
    freeJSON(response);
    if (currentBookFile) {
        iv_fclose(currentBookFile);
        currentBookFile = nullptr;
    }
    return false;
}
freeJSON(response);

std::vector<char> buffer(BASE_PACKET_LEN);

logProto(LOG_DEBUG, "Starting binary transfer...");

while (currentBookReceived < currentBookLength) {
    size_t toRead = std::min((size_t)(currentBookLength - currentBookReceived), 
                            (size_t)BASE_PACKET_LEN);
    
    if (!network->receiveBinaryData(buffer.data(), toRead)) {
        logProto(LOG_ERROR, "Network error during file transfer");
        if (currentBookFile) {
            iv_fclose(currentBookFile);
            currentBookFile = nullptr;
        }
        return false;
    }
    
    size_t written = fwrite(buffer.data(), 1, toRead, currentBookFile);
    if (written != toRead) {
        logProto(LOG_ERROR, "Disk write error");
        if (currentBookFile) {
            iv_fclose(currentBookFile);
            currentBookFile = nullptr;
        }
        return sendErrorResponse("Failed to write book data");
    }
    
    currentBookReceived += toRead;
}

logProto(LOG_INFO, "Transfer complete.");
iv_fclose(currentBookFile);
currentBookFile = nullptr;

bookManager->addBook(metadata);

if (cacheManager) {
    cacheManager->updateCache(metadata);
}

booksReceivedInSession++;
logProto(LOG_INFO, "Book added to DB and cache.");

return true;

}

bool CalibreProtocol::handleSendBookMetadata(json_object* args) {
json_object* dataObj = NULL;
if (!json_object_object_get_ex(args, "data", &dataObj)) {
return sendErrorResponse("Missing metadata");
}

BookMetadata metadata = jsonToMetadata(dataObj);

logProto(LOG_INFO, "Syncing metadata for: %s (Read: %d, Date: %s)", 
         metadata.title.c_str(), metadata.isRead, metadata.lastReadDate.c_str());

if (bookManager->updateBookSync(metadata)) {
    for(auto& b : sessionBooks) {
        if (b.lpath == metadata.lpath) { 
            b.isRead = metadata.isRead;
            b.isFavorite = metadata.isFavorite;
            b.lastReadDate = metadata.lastReadDate;
            b.series = metadata.series;
            b.seriesIndex = metadata.seriesIndex;
            break;
        }
    }
    
    if (cacheManager) {
        cacheManager->updateCache(metadata);
    }
} else {
    logProto(LOG_ERROR, "Warning: Attempted to sync metadata for non-existent book");
}

return true;

}

bool CalibreProtocol::handleDeleteBook(json_object* args) {
json_object* lpathsObj = NULL;
if (!json_object_object_get_ex(args, "lpaths", &lpathsObj)) {
return sendErrorResponse("Missing lpaths");
}
int count = json_object_array_length(lpathsObj);
for (int i = 0; i < count; i++) {
    json_object* lpathObj = json_object_array_get_idx(lpathsObj, i);
    std::string lpath = json_object_get_string(lpathObj);
    
    bookManager->deleteBook(lpath);
    
    if (cacheManager) {
        cacheManager->removeFromCache(lpath);
    }
        
    json_object* response = json_object_new_object();
    json_object_object_add(response, "uuid", json_object_new_string("")); 
    sendOKResponse(response);
    freeJSON(response);
}

return true;
}
bool CalibreProtocol::handleGetBookFileSegment(json_object* args) {
json_object* lpathObj = NULL;
if (!json_object_object_get_ex(args, "lpath", &lpathObj)) {
return sendErrorResponse("Missing lpath");
}
