#ifndef CALIBRE_PROTOCOL_H
#define CALIBRE_PROTOCOL_H

#include "network.h"
#include "book_manager.h"
#include "cache_manager.h"
#include <string>
#include <functional>
#include <cstdio> 

// Forward declarations
struct json_object;

class CalibreProtocol {
public:
    CalibreProtocol(NetworkManager* network, BookManager* bookManager,
                   CacheManager* cacheManager,
                   const std::string& readCol, 
                   const std::string& readDateCol, 
                   const std::string& favCol);
    ~CalibreProtocol();
    
    bool performHandshake(const std::string& password);
    void handleMessages(std::function<void(const std::string&)> statusCallback);
    void disconnect();
    
    bool isConnected() const { return connected; }
    const std::string& getErrorMessage() const { return errorMessage; }
    int getBooksReceivedCount() const { return booksReceivedInSession; }
    
private:
    NetworkManager* network;
    BookManager* bookManager;
    CacheManager* cacheManager;
    bool connected;
    std::string errorMessage;
    std::vector<BookMetadata> sessionBooks;
    
    // Настройки столбцов Calibre
    std::string readColumn;
    std::string readDateColumn;
    std::string favoriteColumn;
    std::string deviceUuid;
    std::string deviceName;
    std::string appVersion;

    // State for receiving files
    std::string currentBookLpath;
    long long currentBookLength;
    long long currentBookReceived;
    FILE* currentBookFile;
    int booksReceivedInSession;
    
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
    json_object* cachedMetadataToJson(const BookMetadata& metadata, int index);
    
    // JSON helpers
    std::string jsonToString(json_object* obj);
    json_object* parseJSON(const std::string& jsonStr);
    void freeJSON(json_object* obj);
    std::string parseJsonStringOrArray(json_object* val);
    
    // Metadata conversion
    BookMetadata jsonToMetadata(json_object* obj);
    json_object* metadataToJson(const BookMetadata& metadata);
};

#endif // CALIBRE_PROTOCOL_H