#include "calibre_protocol.h"
#include "inkview.h"
#include <json-c/json.h>
#include <openssl/sha.h>
#include <sys/statvfs.h>
#include <cstring>
#include <sstream>
#include <iomanip>

// Helper for logging
static void logProto(const char* fmt, ...) {
    FILE* f = fopen("/mnt/ext1/system/calibre-connect.log", "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

CalibreProtocol::CalibreProtocol(NetworkManager* net, BookManager* bookMgr) 
    : network(net), bookManager(bookMgr), connected(false),
      currentBookLength(0), currentBookReceived(0), currentBookFile(nullptr) {
    deviceName = "PocketBook InkPad 4";
    appVersion = "1.0.0";
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
                              json_object_new_int(37));
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
    json_object_object_add(info, "ccVersionNumber", json_object_new_string(appVersion.c_str()));
    json_object_object_add(info, "coverHeight", json_object_new_int(240));
    json_object_object_add(info, "deviceKind", json_object_new_string("PocketBook"));
    json_object_object_add(info, "deviceName", json_object_new_string(deviceName.c_str()));
    json_object_object_add(info, "extensionPathLengths", pathLengths);
    json_object_object_add(info, "maxBookContentPacketLen", json_object_new_int(4096));
    json_object_object_add(info, "useUuidFileNames", json_object_new_boolean(false));
    json_object_object_add(info, "versionOK", json_object_new_boolean(true));
    
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
        
        logProto("[PROTOCOL] Received opcode %d", (int)opcode);
        
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
                statusCallback("Receiving book");
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
                sendErrorResponse("Unexpected opcode");
                break;
        }
        
        freeJSON(args);
        
        if (shouldDisconnect) {
            connected = false;
            logProto("[PROTOCOL] Clean disconnect");
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
    int count = bookManager->getBookCount();
    
    json_object* response = json_object_new_object();
    json_object_object_add(response, "willStream", json_object_new_boolean(true));
    json_object_object_add(response, "willScan", json_object_new_boolean(true));
    json_object_object_add(response, "count", json_object_new_int(count));
    
    bool result = sendOKResponse(response);
    freeJSON(response);
    
    if (count > 0) {
        std::vector<BookMetadata> books = bookManager->getAllBooks();
        for (const auto& book : books) {
            json_object* bookJson = metadataToJson(book);
            std::string bookStr = jsonToString(bookJson);
            network->sendJSON(OK, bookStr.c_str());
            freeJSON(bookJson);
        }
    }
    
    return result;
}

bool CalibreProtocol::handleSendBooklists(json_object* args) {
    json_object* collectionsObj = NULL;
    if (json_object_object_get_ex(args, "collections", &collectionsObj)) {
        std::map<std::string, std::vector<std::string>> collections;
        json_object_object_foreach(collectionsObj, key, val) {
            std::vector<std::string> lpaths;
            int arrayLen = json_object_array_length(val);
            for (int i = 0; i < arrayLen; i++) {
                json_object* lpathObj = json_object_array_get_idx(val, i);
                lpaths.push_back(json_object_get_string(lpathObj));
            }
            collections[key] = lpaths;
        }
        bookManager->updateCollections(collections);
    }
    return true;
}

// Helper to safely convert JSON array or string to string
std::string CalibreProtocol::parseJsonStringOrArray(json_object* val) {
    if (!val || json_object_get_type(val) == json_type_null) return "";
    
    enum json_type type = json_object_get_type(val);
    
    if (type == json_type_string) {
        return safeGetJsonString(val);
    } 
    else if (type == json_type_array) {
        std::string result;
        int len = json_object_array_length(val);
        for (int i = 0; i < len; i++) {
            json_object* item = json_object_array_get_idx(val, i);
            if (i > 0) result += ", ";
            // Безопасное получение строки, даже если это число или null
            const char* str = json_object_get_string(item);
            if (str) {
                result += str;
            }
        }
        return result;
    }
    
    return "";
}

static std::string safeGetJsonString(json_object* val) {
    if (!val) return "";
    if (json_object_get_type(val) == json_type_null) return "";
    const char* str = json_object_get_string(val);
    return str ? std::string(str) : "";
}

BookMetadata CalibreProtocol::jsonToMetadata(json_object* obj) {
    BookMetadata metadata;
    json_object* val = NULL;
    
    if (json_object_object_get_ex(obj, "uuid", &val))
        metadata.uuid = safeGetJsonString(val);
    
    if (json_object_object_get_ex(obj, "title", &val))
        metadata.title = safeGetJsonString(val);
    
    if (json_object_object_get_ex(obj, "authors", &val))
        metadata.authors = parseJsonStringOrArray(val);
        
    if (json_object_object_get_ex(obj, "lpath", &val))
        metadata.lpath = safeGetJsonString(val);
        
    if (json_object_object_get_ex(obj, "series", &val))
        metadata.series = safeGetJsonString(val); // <-- Здесь был вылет
        
    if (json_object_object_get_ex(obj, "series_index", &val))
        metadata.seriesIndex = json_object_get_int(val); // int обычно безопасен, вернет 0 для null
        
    if (json_object_object_get_ex(obj, "publisher", &val))
        metadata.publisher = safeGetJsonString(val); // <-- И здесь был вылет
        
    if (json_object_object_get_ex(obj, "pubdate", &val))
        metadata.pubdate = safeGetJsonString(val);
        
    if (json_object_object_get_ex(obj, "last_modified", &val))
        metadata.lastModified = safeGetJsonString(val);
        
    if (json_object_object_get_ex(obj, "tags", &val))
        metadata.tags = parseJsonStringOrArray(val);
        
    if (json_object_object_get_ex(obj, "comments", &val))
        metadata.comments = safeGetJsonString(val);
        
    if (json_object_object_get_ex(obj, "size", &val))
        metadata.size = json_object_get_int64(val);
        
    // Обработка обложки (без изменений, тут массив)
    if (json_object_object_get_ex(obj, "thumbnail", &val)) {
        if (json_object_get_type(val) == json_type_array && json_object_array_length(val) >= 3) {
            json_object* w = json_object_array_get_idx(val, 0);
            json_object* h = json_object_array_get_idx(val, 1);
            json_object* data = json_object_array_get_idx(val, 2);
            
            if (w) metadata.thumbnailWidth = json_object_get_int(w);
            if (h) metadata.thumbnailHeight = json_object_get_int(h);
            if (data) metadata.thumbnail = safeGetJsonString(data);
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
    
    return obj;
}

bool CalibreProtocol::handleSendBook(json_object* args) {
    logProto("Starting handleSendBook");
    
    // Parse book metadata
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
    
    logProto("Receiving book: %s (%lld bytes)", currentBookLpath.c_str(), currentBookLength);
    
    BookMetadata metadata = jsonToMetadata(metadataObj);
    metadata.lpath = currentBookLpath;
    metadata.size = currentBookLength;
    
    // 1. Prepare File System FIRST (Critical Fix)
    std::string filePath = bookManager->getBookFilePath(currentBookLpath);
    logProto("Target path: %s", filePath.c_str());
    
    // Create directories
    size_t pos = filePath.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = filePath.substr(0, pos);
        iv_buildpath(dir.c_str());
    }
    
    currentBookFile = iv_fopen(filePath.c_str(), "wb");
    if (!currentBookFile) {
        logProto("Failed to open file for writing!");
        return sendErrorResponse("Failed to create book file");
    }
    
    // 2. Send OK response to start receiving
    json_object* response = json_object_new_object();
    json_object_object_add(response, "lpath", json_object_new_string(currentBookLpath.c_str()));
    
    if (!sendOKResponse(response)) {
        logProto("Failed to send OK response");
        freeJSON(response);
        iv_fclose(currentBookFile);
        currentBookFile = nullptr;
        return false;
    }
    freeJSON(response);
    
    // 3. Receive binary data
    const size_t CHUNK_SIZE = 4096;
    std::vector<char> buffer(CHUNK_SIZE);
    
    logProto("Starting binary transfer...");
    
    while (currentBookReceived < currentBookLength) {
        size_t toRead = std::min((size_t)(currentBookLength - currentBookReceived), CHUNK_SIZE);
        
        if (!network->receiveBinaryData(buffer.data(), toRead)) {
            logProto("Network error during file transfer");
            iv_fclose(currentBookFile);
            currentBookFile = nullptr;
            return false;
        }
        
        size_t written = fwrite(buffer.data(), 1, toRead, currentBookFile);
        if (written != toRead) {
            logProto("Disk write error");
            iv_fclose(currentBookFile);
            currentBookFile = nullptr;
            return sendErrorResponse("Failed to write book data");
        }
        
        currentBookReceived += toRead;
    }
    
    logProto("Transfer complete.");
    iv_fclose(currentBookFile);
    currentBookFile = nullptr;
    
    // Add book to database
    bookManager->addBook(metadata);
    logProto("Book added to DB.");
    
    return true;
}

bool CalibreProtocol::handleSendBookMetadata(json_object* args) {
    json_object* dataObj = NULL;
    if (!json_object_object_get_ex(args, "data", &dataObj)) {
        return sendErrorResponse("Missing metadata");
    }
    
    BookMetadata metadata = jsonToMetadata(dataObj);
    
    if (bookManager->hasMetadataChanged(metadata)) {
        bookManager->updateMetadataCache(metadata);
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
        
        BookMetadata metadata;
        if (bookManager->getBook("", metadata)) {
            bookManager->deleteBook(metadata.uuid);
            
            json_object* response = json_object_new_object();
            json_object_object_add(response, "uuid", 
                                  json_object_new_string(metadata.uuid.c_str()));
            sendOKResponse(response);
            freeJSON(response);
        }
    }
    
    return true;
}

bool CalibreProtocol::handleGetBookFileSegment(json_object* args) {
    json_object* lpathObj = NULL;
    if (!json_object_object_get_ex(args, "lpath", &lpathObj)) {
        return sendErrorResponse("Missing lpath");
    }
    
    std::string lpath = json_object_get_string(lpathObj);
    std::string filePath = bookManager->getBookFilePath(lpath);
    
    FILE* file = iv_fopen(filePath.c_str(), "rb");
    if (!file) {
        return sendErrorResponse("Failed to open book file");
    }
    
    fseek(file, 0, SEEK_END);
    long fileLength = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    json_object* response = json_object_new_object();
    json_object_object_add(response, "fileLength", json_object_new_int64(fileLength));
    
    if (!sendOKResponse(response)) {
        freeJSON(response);
        iv_fclose(file);
        return false;
    }
    freeJSON(response);
    
    // Send file data
    const size_t CHUNK_SIZE = 4096;
    std::vector<char> buffer(CHUNK_SIZE);
    
    while (!feof(file)) {
        size_t read = fread(buffer.data(), 1, CHUNK_SIZE, file);
        if (read > 0) {
            if (!network->sendBinaryData(buffer.data(), read)) {
                iv_fclose(file);
                return false;
            }
        }
    }
    
    iv_fclose(file);
    return true;
}

bool CalibreProtocol::handleDisplayMessage(json_object* args) {
    json_object* messageObj = NULL;
    
    if (json_object_object_get_ex(args, "message", &messageObj)) {
        Message(ICON_INFORMATION, "Calibre", 
                json_object_get_string(messageObj), 3000);
    }
    
    return true;
}

bool CalibreProtocol::handleNoop(json_object* args) {
    json_object* val = NULL;
    if (json_object_object_get_ex(args, "count", &val) || 
        json_object_object_get_ex(args, "priKey", &val)) {
        return true;
    }

    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::sendOKResponse(json_object* data) {
    std::string jsonStr = jsonToString(data);
    return network->sendJSON(OK, jsonStr.c_str());
}

bool CalibreProtocol::sendErrorResponse(const std::string& message) {
    json_object* error = json_object_new_object();
    json_object_object_add(error, "message", json_object_new_string(message.c_str()));
    
    std::string jsonStr = jsonToString(error);
    bool result = network->sendJSON(ERROR_OPCODE, jsonStr.c_str());
    
    freeJSON(error);
    return result;
}

std::string CalibreProtocol::jsonToString(json_object* obj) {
    const char* str = json_object_to_json_string(obj);
    return str ? str : "{}";
}

json_object* CalibreProtocol::parseJSON(const std::string& jsonStr) {
    size_t dataStart = jsonStr.find(',');
    if (dataStart == std::string::npos) {
        return NULL;
    }
    
    size_t dataEnd = jsonStr.rfind(']');
    if (dataEnd == std::string::npos) {
        return NULL;
    }
    
    std::string dataStr = jsonStr.substr(dataStart + 1, dataEnd - dataStart - 1);
    return json_tokener_parse(dataStr.c_str());
}

void CalibreProtocol::freeJSON(json_object* obj) {
    if (obj) {
        json_object_put(obj);
    }
}
