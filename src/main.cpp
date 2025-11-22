#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include "inkview.h"

// Application constants
#define APP_NAME "Calibre Connect"
#define VERSION "1.0.0"
#define CONFIG_FILE CONFIGPATH "/calibre_connect.cfg"
#define LOG_FILE USERDATA "/calibre_connect.log"
#define CACHE_DIR USERDATA "/calibre_cache"

// Network constants
#define DEFAULT_PORT 9090
#define BUFFER_SIZE 65536
#define CONNECT_TIMEOUT 30
#define WIFI_TIMEOUT 15000  // 15 seconds
#define WIFI_RETRY_DELAY 3000  // 3 seconds
#define MAX_RETRY_COUNT 3

// Protocol opcodes (from driver.py)
#define OP_NOOP 12
#define OP_OK 0
#define OP_ERROR 20
#define OP_GET_INITIALIZATION_INFO 9
#define OP_GET_DEVICE_INFORMATION 3
#define OP_FREE_SPACE 5
#define OP_TOTAL_SPACE 4
#define OP_GET_BOOK_COUNT 6
#define OP_SEND_BOOKLISTS 7
#define OP_SEND_BOOK 8
#define OP_DELETE_BOOK 13
#define OP_SET_CALIBRE_DEVICE_INFO 1

// Application state
typedef struct {
    int socketFd;
    int connected;
    int wifiConnected;
    pthread_t connectionThread;
    pthread_t wifiEnableThread;
    char statusMessage[256];
    iconfig* config;
    int retryCount;
    int exitRequested;
} AppState;

static AppState g_state = {
    .socketFd = -1,
    .connected = 0,
    .wifiConnected = 0,
    .connectionThread = 0,
    .wifiEnableThread = 0,
    .statusMessage = "Initializing...",
    .config = NULL,
    .retryCount = 0,
    .exitRequested = 0
};

// Configuration structure
typedef struct {
    char serverIp[64];
    int serverPort;
    char deviceName[128];
    int autoConnect;
} Config;

static Config g_config = {
    .serverIp = "",
    .serverPort = DEFAULT_PORT,
    .deviceName = "PocketBook",
    .autoConnect = 1
};

// Function declarations
static void loadConfig(void);
static void saveConfig(void);
static void initializeApp(void);
static void cleanupApp(void);
static void updateStatus(const char* message);
static void startConnection(void);
static void stopConnection(void);
static int mainHandler(int type, int par1, int par2);
static void openConfigEditor(void);
static void drawMainScreen(void);

// Logging
static FILE* g_logFile = NULL;

static void logMessage(const char* format, ...) {
    if (!g_logFile) {
        g_logFile = iv_fopen(LOG_FILE, "a");
        if (!g_logFile) return;
        
        time_t now = time(NULL);
        fprintf(g_logFile, "\n=== Calibre Connect Started [%s] ===\n", ctime(&now));
        fflush(g_logFile);
    }
    
    char timestamp[32];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    fprintf(g_logFile, "[%s] ", timestamp);
    
    va_list args;
    va_start(args, format);
    vfprintf(g_logFile, format, args);
    va_end(args);
    
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

// Configuration management
static void loadConfig(void) {
    g_state.config = OpenConfig(CONFIG_FILE, NULL);
    if (!g_state.config) {
        logMessage("Failed to open config, using defaults");
        return;
    }
    
    const char* serverIp = ReadString(g_state.config, "server_ip", "");
    strncpy(g_config.serverIp, serverIp, sizeof(g_config.serverIp) - 1);
    
    g_config.serverPort = ReadInt(g_state.config, "server_port", DEFAULT_PORT);
    
    const char* deviceName = ReadString(g_state.config, "device_name", "PocketBook");
    strncpy(g_config.deviceName, deviceName, sizeof(g_config.deviceName) - 1);
    
    g_config.autoConnect = ReadInt(g_state.config, "auto_connect", 1);
    
    logMessage("Config loaded: IP=%s, Port=%d, Device=%s, AutoConnect=%d",
               g_config.serverIp, g_config.serverPort, g_config.deviceName, g_config.autoConnect);
}

static void saveConfig(void) {
    if (!g_state.config) {
        g_state.config = OpenConfig(CONFIG_FILE, NULL);
        if (!g_state.config) {
            logMessage("Failed to create config for saving");
            return;
        }
    }
    
    WriteString(g_state.config, "server_ip", g_config.serverIp);
    WriteInt(g_state.config, "server_port", g_config.serverPort);
    WriteString(g_state.config, "device_name", g_config.deviceName);
    WriteInt(g_state.config, "auto_connect", g_config.autoConnect);
    
    SaveConfig(g_state.config);
    logMessage("Config saved");
}

static void updateStatus(const char* message) {
    strncpy(g_state.statusMessage, message, sizeof(g_state.statusMessage) - 1);
    g_state.statusMessage[sizeof(g_state.statusMessage) - 1] = '\0';
    
    logMessage("Status update: %s", message);
    
    drawMainScreen();
    FullUpdate();
}

// WiFi management - using new Network Manager API
static void* wifiEnableThread(void* arg) {
    (void)arg;
    logMessage("WiFi enable thread started");
    
    updateStatus("Enabling WiFi...");
    
    // Ensure Network Manager is running
    int netmgrStatus = NetMgrStatus();
    logMessage("NetMgrStatus() = %d", netmgrStatus);
    
    if (netmgrStatus <= 0) {
        logMessage("Starting Network Manager");
        int result = NetMgr(1);
        logMessage("NetMgr(1) result: %d", result);
        
        if (result != 0) {
            logMessage("Failed to start Network Manager");
            updateStatus("Network error");
            g_state.wifiEnableThread = 0;
            return NULL;
        }
        
        usleep(1000000); // Wait 1 second for NetMgr to initialize
    }
    
    // Enable WiFi hardware
    logMessage("Calling WiFiPower(1)");
    int wifiResult = WiFiPower(1);
    logMessage("WiFiPower(1) result: %d", wifiResult);
    
    if (wifiResult != 0) {
        logMessage("WiFiPower failed");
        updateStatus("WiFi hardware error");
        g_state.wifiEnableThread = 0;
        return NULL;
    }
    
    // Wait for WiFi to initialize
    usleep(2000000); // 2 seconds
    
    int netStatus = QueryNetwork();
    logMessage("QueryNetwork after WiFiPower: 0x%x", netStatus);
    
    if (!(netStatus & NET_WIFIREADY)) {
        logMessage("WiFi not ready after enabling");
        updateStatus("WiFi not ready");
        g_state.wifiEnableThread = 0;
        return NULL;
    }
    
    updateStatus("Connecting to WiFi...");
    
    // Get list of configured networks using NetConnect with empty name
    // This will try to connect to best available known network
    logMessage("Attempting auto-connect to known network");
    int connectResult = NetConnectSilent("");
    logMessage("NetConnectSilent result: %d", connectResult);
    
    if (connectResult == NET_OK || connectResult == NET_CONNECT) {
        logMessage("Connection initiated successfully");
        // Wait for connection event (EVT_NET_CONNECTED)
        // Connection status will be handled in event handler
    } else {
        logMessage("Auto-connect failed with code: %d", connectResult);
        updateStatus("No known WiFi networks");
    }
    
    g_state.wifiEnableThread = 0;
    return NULL;
}

// Calibre protocol handling
static int sendMessage(int opcode, const char* jsonData) {
    if (g_state.socketFd < 0) {
        logMessage("Socket not connected");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    int dataLen = jsonData ? strlen(jsonData) : 0;
    
    snprintf(buffer, sizeof(buffer), "%d[%d,{}]", dataLen + 10, opcode);
    if (jsonData && dataLen > 0) {
        // Replace empty {} with actual JSON data
        char* jsonPos = strstr(buffer, "{}");
        if (jsonPos) {
            snprintf(jsonPos, sizeof(buffer) - (jsonPos - buffer), "%s]", jsonData);
        }
    }
    
    int len = strlen(buffer);
    int sent = send(g_state.socketFd, buffer, len, 0);
    
    if (sent < 0) {
        logMessage("Send failed: %s", strerror(errno));
        return -1;
    }
    
    logMessage("Sent message: opcode=%d, len=%d", opcode, sent);
    return 0;
}

static int receiveMessage(int* opcode, char* jsonData, int maxLen) {
    if (g_state.socketFd < 0) return -1;
    
    char lenBuf[16] = {0};
    int pos = 0;
    
    // Read length prefix
    while (pos < sizeof(lenBuf) - 1) {
        char c;
        int n = recv(g_state.socketFd, &c, 1, 0);
        if (n <= 0) {
            logMessage("Receive failed reading length: %s", strerror(errno));
            return -1;
        }
        
        if (c == '[') break;
        lenBuf[pos++] = c;
    }
    
    int totalLen = atoi(lenBuf);
    if (totalLen <= 0 || totalLen > BUFFER_SIZE) {
        logMessage("Invalid message length: %d", totalLen);
        return -1;
    }
    
    // Read JSON data
    char buffer[BUFFER_SIZE];
    buffer[0] = '[';
    int received = 1;
    
    while (received < totalLen) {
        int n = recv(g_state.socketFd, buffer + received, totalLen - received, 0);
        if (n <= 0) {
            logMessage("Receive failed reading data: %s", strerror(errno));
            return -1;
        }
        received += n;
    }
    
    buffer[received] = '\0';
    
    // Parse opcode from JSON array [opcode, {...}]
    if (sscanf(buffer, "[%d,", opcode) != 1) {
        logMessage("Failed to parse opcode from: %s", buffer);
        return -1;
    }
    
    // Extract JSON data if needed
    if (jsonData) {
        char* dataStart = strchr(buffer, '{');
        if (dataStart) {
            char* dataEnd = strrchr(buffer, '}');
            if (dataEnd) {
                int dataLen = dataEnd - dataStart + 1;
                if (dataLen < maxLen) {
                    strncpy(jsonData, dataStart, dataLen);
                    jsonData[dataLen] = '\0';
                }
            }
        }
    }
    
    logMessage("Received message: opcode=%d, len=%d", *opcode, received);
    return 0;
}

static void* calibreConnectThread(void* arg) {
    (void)arg;
    
    updateStatus("Connecting to Calibre...");
    
    struct sockaddr_in serverAddr;
    g_state.socketFd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (g_state.socketFd < 0) {
        logMessage("Failed to create socket: %s", strerror(errno));
        updateStatus("Socket error");
        return NULL;
    }
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = CONNECT_TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt(g_state.socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(g_state.socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_config.serverPort);
    
    if (inet_pton(AF_INET, g_config.serverIp, &serverAddr.sin_addr) <= 0) {
        logMessage("Invalid server IP: %s", g_config.serverIp);
        updateStatus("Invalid server IP");
        close(g_state.socketFd);
        g_state.socketFd = -1;
        return NULL;
    }
    
    logMessage("Connecting to %s:%d", g_config.serverIp, g_config.serverPort);
    
    if (connect(g_state.socketFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        logMessage("Connection failed: %s", strerror(errno));
        updateStatus("Connection failed");
        close(g_state.socketFd);
        g_state.socketFd = -1;
        return NULL;
    }
    
    g_state.connected = 1;
    updateStatus("Connected to Calibre");
    logMessage("Connected successfully");
    
    // Send initialization info
    char initJson[512];
    snprintf(initJson, sizeof(initJson),
             "{\"appVersion\":\"%s\","
             "\"deviceName\":\"%s\","
             "\"canStreamBooks\":true,"
             "\"canStreamMetadata\":true,"
             "\"canReceiveBookBinary\":true,"
             "\"canDeleteMultipleBooks\":true,"
             "\"canUseCachedMetadata\":true,"
             "\"acceptedExtensions\":[\"epub\",\"pdf\",\"mobi\",\"azw3\",\"fb2\",\"txt\"]}",
             VERSION, g_config.deviceName);
    
    // Handle Calibre protocol
    int running = 1;
    while (running && !g_state.exitRequested) {
        int opcode;
        char jsonData[BUFFER_SIZE];
        
        if (receiveMessage(&opcode, jsonData, sizeof(jsonData)) < 0) {
            logMessage("Connection lost");
            break;
        }
        
        switch (opcode) {
            case OP_GET_INITIALIZATION_INFO:
                sendMessage(OP_OK, initJson);
                break;
                
            case OP_GET_DEVICE_INFORMATION:
                {
                    char devInfo[512];
                    snprintf(devInfo, sizeof(devInfo),
                             "{\"device_info\":{\"device_name\":\"%s\","
                             "\"device_version\":\"%s\"},"
                             "\"device_version\":\"%s\","
                             "\"version\":\"%s\"}",
                             g_config.deviceName, GetSoftwareVersion(),
                             GetSoftwareVersion(), VERSION);
                    sendMessage(OP_OK, devInfo);
                }
                break;
                
            case OP_FREE_SPACE:
            case OP_TOTAL_SPACE:
                {
                    struct statfs stat;
                    if (statfs(FLASHDIR, &stat) == 0) {
                        long long space = (opcode == OP_FREE_SPACE) ?
                            (long long)stat.f_bavail * stat.f_bsize :
                            (long long)stat.f_blocks * stat.f_bsize;
                        
                        char spaceJson[128];
                        snprintf(spaceJson, sizeof(spaceJson),
                                "{\"total_space_on_device\":%lld,"
                                "\"free_space_on_device\":%lld}",
                                space, space);
                        sendMessage(OP_OK, spaceJson);
                    } else {
                        sendMessage(OP_ERROR, "{\"message\":\"Failed to get storage info\"}");
                    }
                }
                break;
                
            case OP_NOOP:
                sendMessage(OP_OK, "{}");
                break;
                
            default:
                logMessage("Unhandled opcode: %d", opcode);
                sendMessage(OP_ERROR, "{\"message\":\"Opcode not implemented\"}");
                break;
        }
    }
    
    // Cleanup
    if (g_state.socketFd >= 0) {
        close(g_state.socketFd);
        g_state.socketFd = -1;
    }
    
    g_state.connected = 0;
    g_state.connectionThread = 0;
    
    if (!g_state.exitRequested) {
        updateStatus("Disconnected");
    }
    
    return NULL;
}

// UI functions
static void drawMainScreen(void) {
    ClearScreen();
    
    SetFont(OpenFont(DEFAULTFONT, 28, 1), BLACK);
    DrawString(20, 40, APP_NAME);
    
    SetFont(OpenFont(DEFAULTFONT, 20, 1), BLACK);
    
    int y = 100;
    char line[256];
    
    // Status
    snprintf(line, sizeof(line), "Status: %s", g_state.statusMessage);
    DrawString(20, y, line);
    y += 40;
    
    // WiFi status
    int netStatus = QueryNetwork();
    NET_STATE netState = GetNetState();
    const char* wifiStatus = (netState == CONNECTED) ? "Connected" :
                            (netStatus & NET_WIFIREADY) ? "Ready" : "Off";
    snprintf(line, sizeof(line), "WiFi: %s", wifiStatus);
    DrawString(20, y, line);
    y += 40;
    
    // Calibre connection
    snprintf(line, sizeof(line), "Calibre: %s", g_state.connected ? "Connected" : "Not connected");
    DrawString(20, y, line);
    y += 40;
    
    // Server info
    if (strlen(g_config.serverIp) > 0) {
        snprintf(line, sizeof(line), "Server: %s:%d", g_config.serverIp, g_config.serverPort);
        DrawString(20, y, line);
        y += 40;
    }
    
    // Instructions
    y = ScreenHeight() - 150;
    SetFont(OpenFont(DEFAULTFONT, 18, 1), DGRAY);
    DrawString(20, y, "Press Menu to configure");
    y += 30;
    DrawString(20, y, "Press OK to connect/disconnect");
    y += 30;
    DrawString(20, y, "Press Back to exit");
}

static void startConnection(void) {
    logMessage("startConnection called");
    
    // Check if WiFi is connected
    NET_STATE netState = GetNetState();
    logMessage("GetNetState() = %d (CONNECTED=2)", netState);
    
    if (netState != CONNECTED) {
        // WiFi not connected, start connection
        logMessage("WiFi not connected, starting WiFi enable");
        
        if (!g_state.wifiEnableThread) {
            logMessage("Starting WiFi enable thread");
            pthread_t thread;
            if (pthread_create(&thread, NULL, wifiEnableThread, NULL) == 0) {
                g_state.wifiEnableThread = thread;
                pthread_detach(thread);
                logMessage("WiFi enable thread started");
            } else {
                logMessage("Failed to create WiFi enable thread");
                updateStatus("Thread error");
            }
        }
        
        // Set timeout for WiFi connection
        SetWeakTimer("WIFI_TIMEOUT", wifiTimeoutTimer, WIFI_TIMEOUT);
    } else {
        // WiFi already connected, start Calibre connection
        logMessage("WiFi already connected, starting Calibre connection");
        
        if (!g_state.connectionThread) {
            pthread_t thread;
            if (pthread_create(&thread, NULL, calibreConnectThread, NULL) == 0) {
                g_state.connectionThread = thread;
                pthread_detach(thread);
                logMessage("Started Calibre connection thread");
            } else {
                logMessage("Failed to create Calibre connection thread");
                updateStatus("Thread error");
            }
        }
    }
}

static void stopConnection(void) {
    logMessage("Stopping connection...");
    
    g_state.exitRequested = 1;
    
    if (g_state.socketFd >= 0) {
        shutdown(g_state.socketFd, SHUT_RDWR);
        close(g_state.socketFd);
        g_state.socketFd = -1;
    }
    
    g_state.connected = 0;
    
    // Wait for threads to finish
    if (g_state.connectionThread) {
        pthread_join(g_state.connectionThread, NULL);
        g_state.connectionThread = 0;
    }
    
    updateStatus("Stopped");
    logMessage("Connection stopped");
}

// Timer callbacks
static void wifiTimeoutTimer(void) {
    logMessage("WiFi connection timeout after %d seconds", WIFI_TIMEOUT / 1000);
    
    // Cancel connection attempt
    g_state.wifiEnableThread = 0;
    
    updateStatus("WiFi connection timeout");
}

static void wifiRetryTimer(void) {
    logMessage("WiFi retry timer fired");
    startConnection();
}

// Configuration editor
static iconfigedit configItems[] = {
    {CFG_TEXT, NULL, "Server IP", "IP address of Calibre server", "server_ip", "", NULL, NULL, NULL},
    {CFG_NUMBER, NULL, "Server Port", "Port number", "server_port", "9090", NULL, NULL, NULL},
    {CFG_TEXT, NULL, "Device Name", "Name shown in Calibre", "device_name", "PocketBook", NULL, NULL, NULL},
    {CFG_INDEX, NULL, "Auto Connect", "Connect on startup", "auto_connect", "1", 
     (char*[]){"No", "Yes", NULL}, NULL, NULL},
    {0}
};

static void configHandler(void) {
    logMessage("Config saved by user");
    
    // Reload config from file
    loadConfig();
    
    drawMainScreen();
    FullUpdate();
}

static void openConfigEditor(void) {
    logMessage("Opening config editor");
    
    OpenConfigEditor("Calibre Connect Settings", g_state.config, configItems, configHandler, NULL);
}

// Event handler
static int mainHandler(int type, int par1, int par2) {
    logMessage("Event: %d, p1: %d, p2: %d", type, par1, par2);
    
    switch (type) {
        case EVT_INIT:
            logMessage("EVT_INIT");
            initializeApp();
            drawMainScreen();
            
            if (g_config.autoConnect && strlen(g_config.serverIp) > 0) {
                startConnection();
            }
            break;
            
        case EVT_SHOW:
            drawMainScreen();
            FullUpdate();
            break;
            
        case EVT_KEYPRESS:
            switch (par1) {
                case IV_KEY_OK:
                    if (g_state.connected) {
                        stopConnection();
                    } else {
                        startConnection();
                    }
                    break;
                    
                case IV_KEY_MENU:
                    openConfigEditor();
                    break;
                    
                case IV_KEY_BACK:
                case IV_KEY_HOME:
                    logMessage("Exit key pressed");
                    CloseApp();
                    break;
            }
            break;
            
        case EVT_NET_CONNECTED:
            logMessage("WiFi connected event received");
            updateStatus("WiFi connected");
            
            // Clear retry counter on successful connection
            g_state.retryCount = 0;
            
            // Clear any pending timers
            ClearTimer(wifiRetryTimer);
            ClearTimer(wifiTimeoutTimer);
            
            // Start Calibre connection
            if (!g_state.connectionThread) {
                pthread_t thread;
                if (pthread_create(&thread, NULL, calibreConnectThread, NULL) == 0) {
                    g_state.connectionThread = thread;
                    pthread_detach(thread);
                    logMessage("Started Calibre connection thread");
                } else {
                    logMessage("Failed to create Calibre connection thread");
                    updateStatus("Connection error");
                }
            }
            break;
            
        case EVT_NET_DISCONNECTED:
            logMessage("WiFi disconnected event received");
            
            // Stop any active connection
            if (g_state.socketFd > 0) {
                close(g_state.socketFd);
                g_state.socketFd = -1;
            }
            
            updateStatus("WiFi disconnected");
            
            // Auto-retry if not manually stopped
            if (g_state.retryCount < MAX_RETRY_COUNT) {
                g_state.retryCount++;
                logMessage("Auto-retrying WiFi connection (attempt %d)", g_state.retryCount);
                SetWeakTimer("WIFI_RETRY", wifiRetryTimer, WIFI_RETRY_DELAY);
            } else {
                updateStatus("WiFi connection failed");
            }
            break;
            
        case EVT_CONFIGCHANGED:
            logMessage("Config changed");
            loadConfig();
            drawMainScreen();
            FullUpdate();
            break;
            
        case EVT_EXIT:
            logMessage("EVT_EXIT received");
            cleanupApp();
            break;
            
        default:
            break;
    }
    
    return 0;
}

static void initializeApp(void) {
    logMessage("Initializing application");
    
    // Create cache directory
    iv_mkdir(CACHE_DIR, 0755);
    
    // Load configuration
    loadConfig();
    
    updateStatus("Ready");
    logMessage("Application initialized");
}

static void cleanupApp(void) {
    logMessage("Cleaning up application");
    
    // Stop connection if active
    if (g_state.connected) {
        stopConnection();
    }
    
    // Close config
    if (g_state.config) {
        CloseConfig(g_state.config);
        g_state.config = NULL;
    }
    
    // Close log
    if (g_logFile) {
        time_t now = time(NULL);
        fprintf(g_logFile, "=== Calibre Connect Closed [%s] ===\n", ctime(&now));
        fclose(g_logFile);
        g_logFile = NULL;
    }
    
    logMessage("Cleanup complete");
}

int main(void) {
    InkViewMain(mainHandler);
    return 0;
}
