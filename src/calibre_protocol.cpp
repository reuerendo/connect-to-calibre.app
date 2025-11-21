#include "calibre_protocol.h"
#include "inkview.h"
#include <json-c/json.h>
#include <openssl/sha.h>
#include <sys/statvfs.h>
#include <cstring>
#include <sstream>
#include <iomanip>

CalibreProtocol::CalibreProtocol(NetworkManager* net) 
    : network(net), connected(false) {
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
    
    // Calibre does: SHA1(password + challenge) as UTF-8 strings
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    
    // First add password bytes
    SHA1_Update(&ctx, password.c_str(), password.length());
    
    // Then add challenge bytes
    SHA1_Update(&ctx, challenge.c_str(), challenge.length());
    
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1_Final(hash, &ctx);
    
    // Convert to hex string
    std::stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    std::string result = ss.str();
    
    // Debug output
    FILE* logFile = fopen("/mnt/ext1/system/calibre-connect.log", "a");
    if (logFile) {
        fprintf(logFile, "[HASH] Password length: %zu, Challenge: '%s', Hash: '%s'\n",
                password.length(), challenge.c_str(), result.c_str());
        fflush(logFile);
        fclose(logFile);
    }
    
    return result;
}

json_object* CalibreProtocol::createDeviceInfo() {
    json_object* info = json_object_new_object();
    
    // Get supported extensions
    json_object* extensions = json_object_new_array();
    const char* supportedFormats[] = {
        "epub", "pdf", "mobi", "azw3", "fb2", "txt", "djvu", "cbz", "cbr"
    };
    
    for (size_t i = 0; i < sizeof(supportedFormats) / sizeof(supportedFormats[0]); i++) {
        json_object_array_add(extensions, json_object_new_string(supportedFormats[i]));
    }
    
    // Extension path lengths
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
    
    // Wait for GET_INITIALIZATION_INFO
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
    
    // Get password challenge if present
    json_object* challengeObj = NULL;
    json_object_object_get_ex(request, "passwordChallenge", &challengeObj);
    std::string challenge = challengeObj ? json_object_get_string(challengeObj) : "";
    
    // Create response
    json_object* response = createDeviceInfo();
    
    // Add password hash if challenge provided
    if (!challenge.empty()) {
        std::string hash = getPasswordHash(password, challenge);
        json_object_object_add(response, "passwordHash", 
                              json_object_new_string(hash.c_str()));
    }
    
    // Send response
    std::string responseStr = jsonToString(response);
    bool success = network->sendJSON(OK, responseStr.c_str());
    
    freeJSON(request);
    freeJSON(response);
    
    if (!success) {
        errorMessage = "Failed to send initialization response";
        return false;
    }
    
    // Wait for next message - could be GET_DEVICE_INFORMATION or DISPLAY_MESSAGE (error)
    if (!network->receiveJSON(opcode, jsonData)) {
        errorMessage = "Failed to receive response after initialization";
        return false;
    }
    
    // Check for password error message
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
        errorMessage = "Unexpected opcode after initialization: " + std::to_string((int)opcode);
        return false;
    }
    
    // Send device information
    json_object* deviceInfo = json_object_new_object();
    json_object* deviceData = json_object_new_object();
    
    // Generate or load device UUID
    const char* uuid = ReadString(GetGlobalConfig(), "calibre_device_uuid", "");
    if (strlen(uuid) == 0) {
        // Generate new UUID
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
    FILE* logFile = fopen("/mnt/ext1/system/calibre-connect.log", "a");
    
    while (connected && network->isConnected()) {
        CalibreOpcode opcode;
        std::string jsonData;
        
        if (!network->receiveJSON(opcode, jsonData)) {
            if (logFile) {
                fprintf(logFile, "[PROTOCOL] Failed to receive message\n");
                fflush(logFile);
            }
            errorMessage = "Connection lost";
            connected = false;
            break;
        }
        
        if (logFile) {
            fprintf(logFile, "[PROTOCOL] Received opcode %d\n", (int)opcode);
            fflush(logFile);
        }
        
        json_object* args = parseJSON(jsonData);
        if (!args) {
            if (logFile) {
                fprintf(logFile, "[PROTOCOL] Failed to parse JSON\n");
                fflush(logFile);
            }
            errorMessage = "Failed to parse message";
            
            // Send error response and continue
            sendErrorResponse("Failed to parse request");
            continue;
        }
        
        bool handled = false;
        
        switch (opcode) {
            case SET_CALIBRE_DEVICE_INFO:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling SET_CALIBRE_DEVICE_INFO\n");
                handled = handleSetCalibreInfo(args);
                statusCallback("Received device info");
                break;
                
            case FREE_SPACE:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling FREE_SPACE\n");
                handled = handleFreeSpace(args);
                statusCallback("Sent free space info");
                break;
                
            case TOTAL_SPACE:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling TOTAL_SPACE\n");
                handled = handleTotalSpace(args);
                statusCallback("Sent total space info");
                break;
                
            case SET_LIBRARY_INFO:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling SET_LIBRARY_INFO\n");
                handled = handleSetLibraryInfo(args);
                statusCallback("Received library info");
                break;
                
            case GET_BOOK_COUNT:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling GET_BOOK_COUNT\n");
                handled = handleGetBookCount(args);
                statusCallback("Sent book count");
                break;
                
            case SEND_BOOKLISTS:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling SEND_BOOKLISTS\n");
                handled = handleSendBooklists(args);
                statusCallback("Processing booklists");
                break;
                
            case SEND_BOOK:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling SEND_BOOK\n");
                handled = handleSendBook(args);
                statusCallback("Receiving book");
                break;
                
            case SEND_BOOK_METADATA:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling SEND_BOOK_METADATA\n");
                handled = handleSendBookMetadata(args);
                statusCallback("Received book metadata");
                break;
                
            case DELETE_BOOK:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling DELETE_BOOK\n");
                handled = handleDeleteBook(args);
                statusCallback("Deleted book");
                break;
                
            case GET_BOOK_FILE_SEGMENT:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling GET_BOOK_FILE_SEGMENT\n");
                handled = handleGetBookFileSegment(args);
                statusCallback("Sent book file");
                break;
                
            case DISPLAY_MESSAGE:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling DISPLAY_MESSAGE\n");
                handled = handleDisplayMessage(args);
                break;
                
            case NOOP:
                if (logFile) fprintf(logFile, "[PROTOCOL] Handling NOOP\n");
                handled = handleNoop(args);
                break;
                
            default:
                if (logFile) {
                    fprintf(logFile, "[PROTOCOL] Unknown opcode: %d\n", (int)opcode);
                    fflush(logFile);
                }
                errorMessage = "Unknown opcode: " + std::to_string((int)opcode);
                // Send error response but don't disconnect
                sendErrorResponse("Unknown opcode");
                handled = true;  // Mark as handled to avoid error message
                break;
        }
        
        freeJSON(args);
        
        if (!handled) {
            if (logFile) {
                fprintf(logFile, "[PROTOCOL] Handler failed: %s\n", errorMessage.c_str());
                fflush(logFile);
            }
            statusCallback("Error: " + errorMessage);
            // Don't disconnect on handler errors, Calibre will retry
        }
        
        if (logFile) {
            fprintf(logFile, "[PROTOCOL] Message processed\n");
            fflush(logFile);
        }
    }
    
    if (logFile) {
        fprintf(logFile, "[PROTOCOL] Message loop ended\n");
        fflush(logFile);
        fclose(logFile);
    }
}

void CalibreProtocol::disconnect() {
    if (connected) {
        // Send final NOOP with ejecting flag
        json_object* noopData = json_object_new_object();
        std::string noopStr = jsonToString(noopData);
        network->sendJSON(OK, noopStr.c_str());
        freeJSON(noopData);
        
        connected = false;
    }
}

bool CalibreProtocol::handleSetCalibreInfo(json_object* args) {
    // Just acknowledge - we don't need to do anything with this
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
    // Just acknowledge
    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleGetBookCount(json_object* args) {
    // TODO: Get actual book count from database
    json_object* response = json_object_new_object();
    json_object_object_add(response, "willStream", json_object_new_boolean(true));
    json_object_object_add(response, "willScan", json_object_new_boolean(true));
    json_object_object_add(response, "count", json_object_new_int(0));
    
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleSendBooklists(json_object* args) {
    // TODO: Implement collection sync
    return true;
}

bool CalibreProtocol::handleSendBook(json_object* args) {
    // TODO: Implement book receiving
    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleSendBookMetadata(json_object* args) {
    // TODO: Implement metadata sync
    return true;
}

bool CalibreProtocol::handleDeleteBook(json_object* args) {
    // TODO: Implement book deletion
    json_object* response = json_object_new_object();
    json_object_object_add(response, "uuid", json_object_new_string(""));
    
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleGetBookFileSegment(json_object* args) {
    // TODO: Implement sending books to calibre
    json_object* response = json_object_new_object();
    bool result = sendOKResponse(response);
    freeJSON(response);
    return result;
}

bool CalibreProtocol::handleDisplayMessage(json_object* args) {
    json_object* kindObj = NULL;
    json_object* messageObj = NULL;
    
    json_object_object_get_ex(args, "messageKind", &kindObj);
    json_object_object_get_ex(args, "message", &messageObj);
    
    if (messageObj) {
        Message(ICON_INFORMATION, "Calibre", 
                json_object_get_string(messageObj), 3000);
    }
    
    return true;
}

bool CalibreProtocol::handleNoop(json_object* args) {
    json_object* ejectingObj = NULL;
    json_object_object_get_ex(args, "ejecting", &ejectingObj);
    
    if (ejectingObj && json_object_get_boolean(ejectingObj)) {
        connected = false;
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
    // Skip opcode part and get the data object
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
