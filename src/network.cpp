#include "network.h"
#include <cstring>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <cstdio>
#include <ctime>
#include <cstdarg>

// Debug logging
static FILE* netLogFile = NULL;

static void initNetLog() {
    const char* logPath = "/mnt/ext1/system/calibre-connect.log";
    netLogFile = fopen(logPath, "a");
    if (netLogFile) {
        time_t now = time(NULL);
        fprintf(netLogFile, "\n=== Network Module Initialized [%s] ===\n", ctime(&now));
        fflush(netLogFile);
    }
}

static void logNetMsg(const char* format, ...) {
    if (!netLogFile) {
        initNetLog(); // Try to reopen if closed
        if (!netLogFile) return;
    }
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    fprintf(netLogFile, "[NET %02d:%02d:%02d] ", 
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    va_list args;
    va_start(args, format);
    vfprintf(netLogFile, format, args);
    va_end(args);
    fprintf(netLogFile, "\n");
    fflush(netLogFile);
}

NetworkManager::NetworkManager() 
    : socketFd(-1), udpSocketFd(-1) {
    initNetLog();
    logNetMsg("NetworkManager created");
}

NetworkManager::~NetworkManager() {
    logNetMsg("NetworkManager destructor called");
    disconnect();
    closeUDPSocket();
    
    if (netLogFile) {
        fclose(netLogFile);
        netLogFile = NULL;
    }
}

bool NetworkManager::createUDPSocket() {
    logNetMsg("Creating UDP socket");
    
    udpSocketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocketFd < 0) {
        logNetMsg("Failed to create UDP socket: %s", strerror(errno));
        return false;
    }
    
    logNetMsg("UDP socket created: fd=%d", udpSocketFd);
    
    // Enable broadcast
    int broadcastEnable = 1;
    if (setsockopt(udpSocketFd, SOL_SOCKET, SO_BROADCAST, 
                   &broadcastEnable, sizeof(broadcastEnable)) < 0) {
        logNetMsg("Failed to enable broadcast: %s", strerror(errno));
        close(udpSocketFd);
        udpSocketFd = -1;
        return false;
    }
    
    logNetMsg("Broadcast enabled");
    
    // Bind to any address on port 8134 (companion port)
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(8134);
    
    if (bind(udpSocketFd, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        logNetMsg("Failed to bind UDP socket: %s", strerror(errno));
        close(udpSocketFd);
        udpSocketFd = -1;
        return false;
    }
    
    logNetMsg("UDP socket bound to port 8134");
    return true;
}

void NetworkManager::closeUDPSocket() {
    if (udpSocketFd >= 0) {
        logNetMsg("Closing UDP socket: fd=%d", udpSocketFd);
        close(udpSocketFd);
        udpSocketFd = -1;
    }
}

bool NetworkManager::sendUDPBroadcast(int port) {
    logNetMsg("Sending UDP broadcast to port %d", port);
    
    struct sockaddr_in broadcastAddr;
    memset(&broadcastAddr, 0, sizeof(broadcastAddr));
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcastAddr.sin_port = htons(port);
    
    const char* message = "hello";
    int result = sendto(udpSocketFd, message, strlen(message), 0,
                       (struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
    
    if (result < 0) {
        logNetMsg("UDP broadcast failed: %s", strerror(errno));
        return false;
    }
    
    logNetMsg("UDP broadcast sent successfully: %d bytes", result);
    return true;
}

bool NetworkManager::receiveUDPResponse(std::string& host, int& port, int timeoutMs) {
    logNetMsg("Waiting for UDP response (timeout: %dms)", timeoutMs);
    
    struct timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(udpSocketFd, &readfds);
    
    int result = select(udpSocketFd + 1, &readfds, NULL, NULL, &timeout);
    if (result <= 0) {
        if (result == 0) {
            logNetMsg("UDP receive timeout");
        } else {
            logNetMsg("UDP select error: %s", strerror(errno));
        }
        return false;
    }
    
    char buffer[1024];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    
    int received = recvfrom(udpSocketFd, buffer, sizeof(buffer) - 1, 0,
                           (struct sockaddr*)&fromAddr, &fromLen);
    
    if (received <= 0) {
        logNetMsg("UDP recvfrom failed: %s", strerror(errno));
        return false;
    }
    
    buffer[received] = '\0';
    logNetMsg("UDP response received: %d bytes, data: %s", received, buffer);
    
    // Parse response: "calibre wireless device client (on hostname);content_port,socket_port"
    std::string response(buffer);
    size_t portPos = response.rfind(',');
    if (portPos == std::string::npos) {
        logNetMsg("Failed to parse UDP response: no comma found");
        return false;
    }
    
    port = atoi(response.substr(portPos + 1).c_str());
    host = inet_ntoa(fromAddr.sin_addr);
    
    logNetMsg("Parsed server info: host=%s, port=%d", host.c_str(), port);
    return port > 0;
}

bool NetworkManager::discoverCalibreServer(std::string& host, int& port,
                                           std::function<bool()> cancelCallback) {
    logNetMsg("Starting Calibre server discovery");
    
    if (!createUDPSocket()) {
        logNetMsg("Failed to create UDP socket for discovery");
        return false;
    }
    
    // Try each broadcast port
    for (int i = 0; i < BROADCAST_PORT_COUNT; i++) {
        if (cancelCallback && cancelCallback()) {
            logNetMsg("Discovery cancelled by callback");
            closeUDPSocket();
            return false;
        }
        
        logNetMsg("Trying broadcast port %d/%d: %d", i+1, BROADCAST_PORT_COUNT, BROADCAST_PORTS[i]);
        
        if (!sendUDPBroadcast(BROADCAST_PORTS[i])) {
            continue;
        }
        
        if (receiveUDPResponse(host, port, 3000)) {
            logNetMsg("Server discovered successfully!");
            closeUDPSocket();
            return true;
        }
    }
    
    logNetMsg("Server discovery failed: no response from any port");
    closeUDPSocket();
    return false;
}

bool NetworkManager::connectToServer(const std::string& host, int port) {
    logNetMsg("Connecting to server: %s:%d", host.c_str(), port);
    
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        logNetMsg("Failed to create TCP socket: %s", strerror(errno));
        return false;
    }
    
    logNetMsg("TCP socket created: fd=%d", socketFd);
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
        logNetMsg("Invalid IP address: %s", host.c_str());
        close(socketFd);
        socketFd = -1;
        return false;
    }
    
    // Set connection timeout
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    logNetMsg("Attempting connection...");
    
    if (connect(socketFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        logNetMsg("Connection failed: %s", strerror(errno));
        close(socketFd);
        socketFd = -1;
        return false;
    }
    
    logNetMsg("Connected successfully to %s:%d", host.c_str(), port);
    return true;
}

void NetworkManager::disconnect() {
    if (socketFd >= 0) {
        logNetMsg("Disconnecting TCP socket: fd=%d", socketFd);
        close(socketFd);
        socketFd = -1;
    }
}

bool NetworkManager::sendAll(const void* data, size_t length) {
    logNetMsg("Sending %zu bytes", length);
    
    const char* ptr = (const char*)data;
    size_t remaining = length;
    size_t totalSent = 0;
    
    while (remaining > 0) {
        ssize_t sent = send(socketFd, ptr, remaining, 0);
        if (sent <= 0) {
            if (errno == EINTR) {
                logNetMsg("Send interrupted, retrying");
                continue;
            }
            logNetMsg("Send failed: %s (sent %zu/%zu bytes)", strerror(errno), totalSent, length);
            return false;
        }
        ptr += sent;
        remaining -= sent;
        totalSent += sent;
    }
    
    logNetMsg("Sent %zu bytes successfully", totalSent);
    return true;
}

bool NetworkManager::receiveAll(void* buffer, size_t length) {
    logNetMsg("Receiving %zu bytes", length);
    
    char* ptr = (char*)buffer;
    size_t remaining = length;
    size_t totalReceived = 0;
    
    while (remaining > 0) {
        ssize_t received = recv(socketFd, ptr, remaining, 0);
        if (received <= 0) {
            if (errno == EINTR) {
                logNetMsg("Receive interrupted, retrying");
                continue;
            }
            logNetMsg("Receive failed: %s (received %zu/%zu bytes)", strerror(errno), totalReceived, length);
            return false;
        }
        ptr += received;
        remaining -= received;
        totalReceived += received;
    }
    
    logNetMsg("Received %zu bytes successfully", totalReceived);
    return true;
}

std::string NetworkManager::receiveString() {
    logNetMsg("Receiving string with length prefix");
    
    // Read length prefix (format: "1234[...")
    char lengthBuf[32];
    int lengthPos = 0;
    
    while (lengthPos < sizeof(lengthBuf) - 1) {
        char c;
        if (!receiveAll(&c, 1)) {
            logNetMsg("Failed to read length prefix");
            return "";
        }
        
        if (c == '[') {
            lengthBuf[lengthPos] = '\0';
            break;
        }
        
        lengthBuf[lengthPos++] = c;
    }
    
    int dataLength = atoi(lengthBuf);
    logNetMsg("String length: %d", dataLength);
    
    if (dataLength <= 0 || dataLength > 10 * 1024 * 1024) { // 10MB max
        logNetMsg("Invalid string length: %d", dataLength);
        return "";
    }
    
    // Read JSON data (including the '[' we already saw)
    std::vector<char> buffer(dataLength + 1);
    buffer[0] = '[';
    
    if (!receiveAll(&buffer[1], dataLength - 1)) {
        logNetMsg("Failed to receive string data");
        return "";
    }
    
    buffer[dataLength] = '\0';
    std::string result(buffer.data());
    logNetMsg("Received string: %s", result.c_str());
    return result;
}

bool NetworkManager::sendJSON(CalibreOpcode opcode, const char* jsonData) {
    if (socketFd < 0) {
        logNetMsg("Cannot send JSON: socket not connected");
        return false;
    }
    
    // Format: length + JSON data
    // JSON data format: [opcode, {data}]
    std::string message = "[" + std::to_string((int)opcode) + "," + jsonData + "]";
    std::string packet = std::to_string(message.length()) + message;
    
    logNetMsg("Sending JSON: opcode=%d, message=%s", (int)opcode, message.c_str());
    
    return sendAll(packet.c_str(), packet.length());
}

bool NetworkManager::receiveJSON(CalibreOpcode& opcode, std::string& jsonData) {
    if (socketFd < 0) {
        logNetMsg("Cannot receive JSON: socket not connected");
        return false;
    }
    
    std::string message = receiveString();
    if (message.empty()) {
        logNetMsg("Received empty message");
        return false;
    }
    
    jsonData = message;
    
    // Parse opcode from JSON array: [opcode, {...}]
    size_t opcodeEnd = message.find(',');
    if (opcodeEnd == std::string::npos || message[0] != '[') {
        logNetMsg("Failed to parse JSON message format");
        return false;
    }
    
    int opcodeValue = atoi(message.substr(1, opcodeEnd - 1).c_str());
    opcode = (CalibreOpcode)opcodeValue;
    
    logNetMsg("Received JSON: opcode=%d, data=%s", (int)opcode, jsonData.c_str());
    return true;
}

bool NetworkManager::sendBinaryData(const void* data, size_t length) {
    if (socketFd < 0) {
        logNetMsg("Cannot send binary data: socket not connected");
        return false;
    }
    
    logNetMsg("Sending binary data: %zu bytes", length);
    return sendAll(data, length);
}

bool NetworkManager::receiveBinaryData(void* buffer, size_t length) {
    if (socketFd < 0) {
        logNetMsg("Cannot receive binary data: socket not connected");
        return false;
    }
    
    logNetMsg("Receiving binary data: %zu bytes", length);
    return receiveAll(buffer, length);
}
