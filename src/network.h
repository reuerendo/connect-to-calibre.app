#ifndef NETWORK_H
#define NETWORK_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <functional>

// Calibre protocol opcodes
enum CalibreOpcode {
    OK                        = 0,
    SET_CALIBRE_DEVICE_INFO   = 1,
    SET_CALIBRE_DEVICE_NAME   = 2,
    GET_DEVICE_INFORMATION    = 3,
    TOTAL_SPACE               = 4,
    FREE_SPACE                = 5,
    GET_BOOK_COUNT            = 6,
    SEND_BOOKLISTS            = 7,
    SEND_BOOK                 = 8,
    GET_INITIALIZATION_INFO   = 9,
    BOOK_DONE                 = 11,
    NOOP                      = 12,
    DELETE_BOOK               = 13,
    GET_BOOK_FILE_SEGMENT     = 14,
    GET_BOOK_METADATA         = 15,
    SEND_BOOK_METADATA        = 16,
    DISPLAY_MESSAGE           = 17,
    CALIBRE_BUSY              = 18,
    SET_LIBRARY_INFO          = 19,
    ERROR_OPCODE              = 20
};

// Broadcast ports for Calibre discovery
const int BROADCAST_PORTS[] = {54982, 48123, 39001, 44044, 59678};
const int BROADCAST_PORT_COUNT = 5;

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();
    
    // Discovery methods
    bool discoverCalibreServer(std::string& host, int& port, 
                              std::function<bool()> cancelCallback);
    bool connectToServer(const std::string& host, int port);
    void disconnect();
    
    // Communication methods
    bool sendJSON(CalibreOpcode opcode, const char* jsonData);
    bool receiveJSON(CalibreOpcode& opcode, std::string& jsonData);
    bool sendBinaryData(const void* data, size_t length);
    bool receiveBinaryData(void* buffer, size_t length);
    
    // Connection status
    bool isConnected() const { return socketFd >= 0; }
    
private:
    int socketFd;
    int udpSocketFd;
    
    // Helper methods
    bool createUDPSocket();
    void closeUDPSocket();
    bool sendUDPBroadcast(int port);
    bool receiveUDPResponse(std::string& host, int& port, int timeoutMs);
    
    bool sendAll(const void* data, size_t length);
    bool receiveAll(void* buffer, size_t length);
    
    std::string receiveString();
};


#endif // NETWORK_H
