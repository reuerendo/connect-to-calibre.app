#include "network.h"
#include <cstring>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <vector>
#include <algorithm>

// Use the thread-safe logger from main.cpp
extern void logMsg(const char* format, ...);

// RAII Wrapper for socket file descriptors to ensure they are closed
class SocketGuard {
    int& fd_;
public:
    explicit SocketGuard(int& fd) : fd_(fd) {}
    ~SocketGuard() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    // Prevent copying
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
};

NetworkManager::NetworkManager() 
    : socketFd(-1), udpSocketFd(-1) {
}

NetworkManager::~NetworkManager() {
    disconnect();
    // UDP socket is closed by closeUDPSocket() or SocketGuard where used
    if (udpSocketFd >= 0) {
        close(udpSocketFd);
    }
}

bool NetworkManager::createUDPSocket() {
    if (udpSocketFd >= 0) close(udpSocketFd);
    
    udpSocketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocketFd < 0) {
        logMsg("Failed to create UDP socket: %s", strerror(errno));
        return false;
    }
    
    // Enable broadcast
    int broadcastEnable = 1;
    if (setsockopt(udpSocketFd, SOL_SOCKET, SO_BROADCAST, 
                   &broadcastEnable, sizeof(broadcastEnable)) < 0) {
        logMsg("Failed to enable broadcast: %s", strerror(errno));
        close(udpSocketFd);
        udpSocketFd = -1;
        return false;
    }
    
    // Bind to any address on port 8134 (companion port)
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(8134);
    
    if (bind(udpSocketFd, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
        logMsg("Failed to bind UDP socket: %s", strerror(errno));
        close(udpSocketFd);
        udpSocketFd = -1;
        return false;
    }
    
    return true;
}

void NetworkManager::closeUDPSocket() {
    if (udpSocketFd >= 0) {
        close(udpSocketFd);
        udpSocketFd = -1;
    }
}

bool NetworkManager::sendUDPBroadcast(int port) {
    struct sockaddr_in broadcastAddr;
    memset(&broadcastAddr, 0, sizeof(broadcastAddr));
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcastAddr.sin_port = htons(port);
    
    const char* message = "hello";
    ssize_t result;
    do {
        result = sendto(udpSocketFd, message, strlen(message), 0,
                        reinterpret_cast<struct sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));
    } while (result < 0 && errno == EINTR);
    
    if (result < 0) {
        logMsg("UDP broadcast failed: %s", strerror(errno));
        return false;
    }
    
    return true;
}

bool NetworkManager::receiveUDPResponse(std::string& host, int& port, int timeoutMs) {
    struct timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(udpSocketFd, &readfds);
    
    int result;
    do {
        result = select(udpSocketFd + 1, &readfds, NULL, NULL, &timeout);
    } while (result < 0 && errno == EINTR);

    if (result <= 0) {
        if (result < 0) {
            logMsg("UDP select error: %s", strerror(errno));
        }
        return false;
    }
    
    char buffer[1024];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    
    ssize_t received;
    do {
        received = recvfrom(udpSocketFd, buffer, sizeof(buffer) - 1, 0,
                            reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
    } while (received < 0 && errno == EINTR);
    
    if (received <= 0) {
        logMsg("UDP recvfrom failed: %s", strerror(errno));
        return false;
    }
    
    buffer[received] = '\0';
    
    // Parse response: "calibre wireless device client (on hostname);content_port,socket_port"
    std::string response(buffer);
    size_t portPos = response.rfind(',');
    if (portPos == std::string::npos) {
        return false;
    }
    
    port = atoi(response.substr(portPos + 1).c_str());
    host = inet_ntoa(fromAddr.sin_addr);
    
    return port > 0;
}

bool NetworkManager::discoverCalibreServer(std::string& host, int& port,
                                           std::function<bool()> cancelCallback) {
    
    // Optimization: Create socket once, assume RAII guard handles closing
    if (!createUDPSocket()) {
        return false;
    }
    
    // RAII guard to ensure UDP socket is closed when this function exits
    SocketGuard udpGuard(udpSocketFd);
    
    // Try each broadcast port
    for (int i = 0; i < BROADCAST_PORT_COUNT; i++) {
        if (cancelCallback && cancelCallback()) {
            return false;
        }
        
        if (!sendUDPBroadcast(BROADCAST_PORTS[i])) {
            continue;
        }
        
        // Wait shorter time for each port to speed up scanning
        if (receiveUDPResponse(host, port, 2000)) {
            return true;
        }
    }
    
    return false;
}

bool NetworkManager::connectToServer(const std::string& host, int port) {
    if (socketFd >= 0) close(socketFd);

    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        logMsg("Failed to create TCP socket: %s", strerror(errno));
        return false;
    }
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
        logMsg("Invalid IP address: %s", host.c_str());
        close(socketFd);
        socketFd = -1;
        return false;
    }
    
    // Non-blocking connect
    int flags = fcntl(socketFd, F_GETFL, 0);
    if (flags < 0 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(socketFd);
        socketFd = -1;
        return false;
    }
    
    int connectResult = connect(socketFd, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr));
    
    if (connectResult < 0) {
        if (errno != EINPROGRESS) {
            logMsg("Connection failed immediately: %s", strerror(errno));
            close(socketFd);
            socketFd = -1;
            return false;
        }
        
        // Wait for connection
        fd_set writefds;
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        
        FD_ZERO(&writefds);
        FD_SET(socketFd, &writefds);
        
        int selectResult;
        do {
            selectResult = select(socketFd + 1, NULL, &writefds, NULL, &timeout);
        } while (selectResult < 0 && errno == EINTR);
        
        if (selectResult <= 0) {
            logMsg("Connection timeout or select error");
            close(socketFd);
            socketFd = -1;
            return false;
        }
        
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            logMsg("Connection failed: %s", error != 0 ? strerror(error) : strerror(errno));
            close(socketFd);
            socketFd = -1;
            return false;
        }
    }
    
    // Restore blocking mode
    if (fcntl(socketFd, F_SETFL, flags) < 0) {
        close(socketFd);
        socketFd = -1;
        return false;
    }
    
    // Set 5 minute timeout
    struct timeval timeout;
    timeout.tv_sec = 300;
    timeout.tv_usec = 0;
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    return true;
}

void NetworkManager::disconnect() {
    if (socketFd >= 0) {
        // Shutdown transmission and reception to unblock any thread waiting in recv()
        shutdown(socketFd, SHUT_RDWR);
        close(socketFd);
        socketFd = -1;
    }
}

bool NetworkManager::sendAll(const void* data, size_t length) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = length;
    
    while (remaining > 0) {
        ssize_t sent = send(socketFd, ptr, remaining, 0);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            logMsg("Send failed: %s", strerror(errno));
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool NetworkManager::receiveAll(void* buffer, size_t length) {
    char* ptr = static_cast<char*>(buffer);
    size_t remaining = length;
    
    while (remaining > 0) {
        ssize_t received = recv(socketFd, ptr, remaining, 0);
        if (received <= 0) {
            if (received < 0 && errno == EINTR) continue;
            if (received == 0) {
                // Orderly connection shutdown by remote peer
                return false;
            }
            // Only log as receive failure if socket is still open
            if (socketFd >= 0) {
                logMsg("Receive failed: %s", strerror(errno));
            }
            return false;
        }
        ptr += received;
        remaining -= received;
    }
    return true;
}

std::string NetworkManager::receiveString() {
    // Read length prefix byte by byte (e.g. "1234[")
    // This is not performance critical as it's just a few bytes
    char lengthBuf[32];
    size_t lengthPos = 0;
    
    while (lengthPos < sizeof(lengthBuf) - 1) {
        char c;
        if (!receiveAll(&c, 1)) {
            return "";
        }
        
        if (c == '[') {
            lengthBuf[lengthPos] = '\0';
            break;
        }
        
        lengthBuf[lengthPos++] = c;
    }
    
    int dataLength = atoi(lengthBuf);
    
    if (dataLength <= 0 || dataLength > 10 * 1024 * 1024) { // 10MB limit check
        logMsg("Invalid string length: %d", dataLength);
        return "";
    }
    
    // Optimization: Read directly into string to avoid double allocation/copy
    std::string str;
    try {
        str.resize(dataLength + 1); // +1 for the initial '['
    } catch (const std::bad_alloc&) {
        logMsg("Failed to allocate memory for string of size %d", dataLength);
        return "";
    }
    
    str[0] = '['; // Restore the bracket we consumed
    
    // Read the rest of the JSON directly into the string buffer
    // &str[1] points to the second character in the string's internal buffer
    if (!receiveAll(&str[1], dataLength - 1)) {
        return "";
    }
    
    return str;
}

bool NetworkManager::sendJSON(CalibreOpcode opcode, const char* jsonData) {
    if (socketFd < 0) {
        logMsg("Cannot send JSON: socket not connected");
        return false;
    }
    
    // Protocol: length_of_message + [opcode, json_body]
    std::string message = "[" + std::to_string((int)opcode) + "," + jsonData + "]";
    std::string packet = std::to_string(message.length()) + message;
    
    return sendAll(packet.c_str(), packet.length());
}

bool NetworkManager::receiveJSON(CalibreOpcode& opcode, std::string& jsonData) {
    if (socketFd < 0) {
        logMsg("Cannot receive JSON: socket not connected");
        return false;
    }
    
    std::string message = receiveString();
    if (message.empty()) {
        return false;
    }
    
    jsonData = message;
    
    // Parse opcode from JSON array: [opcode, {...}]
    // We expect at least "[0,"
    if (message.length() < 3 || message[0] != '[') {
        logMsg("Failed to parse JSON message format");
        return false;
    }

    size_t opcodeEnd = message.find(',');
    if (opcodeEnd == std::string::npos) {
        logMsg("Invalid JSON structure (no comma)");
        return false;
    }
    
    int opcodeValue = atoi(message.substr(1, opcodeEnd - 1).c_str());
    opcode = static_cast<CalibreOpcode>(opcodeValue);
    
    return true;
}

bool NetworkManager::sendBinaryData(const void* data, size_t length) {
    if (socketFd < 0) {
        logMsg("Cannot send binary data: socket not connected");
        return false;
    }
    return sendAll(data, length);
}

bool NetworkManager::receiveBinaryData(void* buffer, size_t length) {
    if (socketFd < 0) {
        logMsg("Cannot receive binary data: socket not connected");
        return false;
    }
    return receiveAll(buffer, length);
}
