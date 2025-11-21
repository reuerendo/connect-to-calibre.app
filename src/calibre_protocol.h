#ifndef CALIBRE_PROTOCOL_H
#define CALIBRE_PROTOCOL_H

#include "network.h"
#include <string>
#include <functional>

// Forward declarations
struct json_object;

class CalibreProtocol {
public:
    CalibreProtocol(NetworkManager* network);
    ~CalibreProtocol();
    
    // Connection lifecycle
    bool performHandshake(const std::string& password);
    void handleMessages(std::function<void(const std::string&)> statusCallback);
    void disconnect();
    
    // Status
    bool isConnected() const { return connected; }
    const std::string& getErrorMessage() const { return errorMessage; }
    
private:
    NetworkManager* network;
    bool connected;
    std::string errorMessage;
    
    // Device information
    std::string deviceUuid;
    std::string deviceName;
    std::string appVersion;
    
    // Protocol handlers
    bool handleGetInitializationInfo(json_object* args);
    bool handleGetDeviceInformation(json_object* args);
    bool handleSetCalibreInfo(json_object* args);
    bool handleFreeSpace(json_object* args);
    bool handleTotalSpace(json_object* args);
    bool handleSetLibraryInfo(json_object* args);
    bool handleGetBookCount(json_object* args);
    bool handleSendBooklists(json_object* args);
    bool handleSendBook(json_object* args);
    bool handleSendBookMetadata(json_object* args);
    bool handleDeleteBook(json_object* args);
    bool handleGetBookFileSegment(json_object* args);
    bool handleDisplayMessage(json_object* args);
    bool handleNoop(json_object* args);
    
    // Helper methods
    bool sendOKResponse(json_object* data);
    bool sendErrorResponse(const std::string& message);
    json_object* createDeviceInfo();
    std::string getPasswordHash(const std::string& password, 
                               const std::string& challenge);
    
    // JSON helpers
    std::string jsonToString(json_object* obj);
    json_object* parseJSON(const std::string& jsonStr);
    void freeJSON(json_object* obj);
};

#endif // CALIBRE_PROTOCOL_H