#include "inkview.h"
#include "network.h"
#include "calibre_protocol.h"
#include "book_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>

// Custom events for safe UI updates from thread
#define EVT_USER_UPDATE 20001
#define EVT_CONNECTION_FAILED 20002
#define EVT_SYNC_COMPLETE 20003

// Debug logging
static FILE* logFile = NULL;

void initLog() {
    const char* logPath = "/mnt/ext1/system/calibre-connect.log";
    logFile = iv_fopen(logPath, "a");
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "\n=== Calibre Connect Started [%s] ===\n", ctime(&now));
        fflush(logFile);
    }
}

void logMsg(const char* format, ...) {
    if (!logFile) {
        initLog(); 
        if (!logFile) return;
    }
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    fprintf(logFile, "[%02d:%02d:%02d] ", 
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fprintf(logFile, "\n");
    fflush(logFile);
}

void closeLog() {
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "=== Calibre Connect Closed [%s] ===\n", ctime(&now));
        fflush(logFile);
        iv_fclose(logFile);
        logFile = NULL;
    }
}

// Global config
static iconfig *appConfig = NULL;
static const char *CONFIG_FILE = "/mnt/ext1/system/config/calibre-connect.cfg";

// Config keys
static const char *KEY_IP = "ip";
static const char *KEY_PORT = "port";
static const char *KEY_PASSWORD = "password";
static const char *KEY_READ_COLUMN = "read_column";
static const char *KEY_READ_DATE_COLUMN = "read_date_column";
static const char *KEY_FAVORITE_COLUMN = "favorite_column";
static const char *KEY_CONNECTION = "connection_enabled";

// Global connection status buffer and error message
static char connectionStatusBuffer[128] = "Disconnected"; 
static char connectionErrorBuffer[256] = "";
static char syncCompleteBuffer[256] = "";
static int booksReceivedCount = 0;

// Default values
static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "9090";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "#read";
static const char *DEFAULT_READ_DATE_COLUMN = "#read_date";
static const char *DEFAULT_FAVORITE_COLUMN = "#favorite";

// Connection state
static NetworkManager* networkManager = NULL;
static BookManager* bookManager = NULL;
static CalibreProtocol* protocol = NULL;
static pthread_t connectionThread;
static bool isConnecting = false;
static bool shouldStop = false;
static volatile bool exitRequested = false;

// Forward declarations
int mainEventHandler(int type, int par1, int par2);
void performExit();
void startConnection();

// Config editor structure
static iconfigedit configItems[] = {
    {
        CFG_INFO,
        NULL,
        (char*)"Connection",
        NULL,
        (char*)KEY_CONNECTION,
        connectionStatusBuffer, 
        NULL,
        NULL,
        NULL
    },
    {
        CFG_IPADDR,
        NULL,
        (char *)"IP Address",
        NULL,
        (char *)KEY_IP,
        (char *)DEFAULT_IP,
        NULL,
        NULL
    },
    {
        CFG_NUMBER,
        NULL,
        (char *)"Port",
        NULL,
        (char *)KEY_PORT,
        (char *)DEFAULT_PORT,
        NULL,
        NULL
    },
    {
        CFG_PASSWORD,
        NULL,
        (char *)"Password",
        NULL,
        (char *)KEY_PASSWORD,
        (char *)DEFAULT_PASSWORD,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"Read Status Column",
        NULL,
        (char *)KEY_READ_COLUMN,
        (char *)DEFAULT_READ_COLUMN,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"Read Date Column",
        NULL,
        (char *)KEY_READ_DATE_COLUMN,
        (char *)DEFAULT_READ_DATE_COLUMN,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"Favorite Column",
        NULL,
        (char *)KEY_FAVORITE_COLUMN,
        (char *)DEFAULT_FAVORITE_COLUMN,
        NULL,
        NULL
    },
    {
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    }
};

void updateConnectionStatus(const char* status) {
    logMsg("Status update: %s", status);
    snprintf(connectionStatusBuffer, sizeof(connectionStatusBuffer), "%s", status);
    
    // Update config value
    if (appConfig) {
        WriteString(appConfig, KEY_CONNECTION, status);
    }
    
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
}

void notifyConnectionFailed(const char* errorMsg) {
    logMsg("Connection failed: %s", errorMsg);
    snprintf(connectionErrorBuffer, sizeof(connectionErrorBuffer), "%s", errorMsg);
    SendEvent(mainEventHandler, EVT_CONNECTION_FAILED, 0, 0);
}

void notifySyncComplete(int booksReceived) {
    logMsg("Sync complete: %d books received", booksReceived);
    booksReceivedCount = booksReceived;
    snprintf(syncCompleteBuffer, sizeof(syncCompleteBuffer), 
             "Synchronization complete!\n%d book%s received from Calibre.", 
             booksReceived, booksReceived == 1 ? "" : "s");
    SendEvent(mainEventHandler, EVT_SYNC_COMPLETE, 0, 0);
}

// Check if WiFi is ready and connected
bool isWiFiReady() {
    int netStatus = QueryNetwork();
    logMsg("WiFi status check: 0x%X (READY: %d, CONNECTED: %d)", 
           netStatus, 
           (netStatus & NET_WIFIREADY) ? 1 : 0,
           (netStatus & NET_CONNECTED) ? 1 : 0);
    return (netStatus & NET_WIFIREADY) && (netStatus & NET_CONNECTED);
}

// Enable WiFi if not already enabled
bool enableWiFi() {
    logMsg("Checking WiFi status");
    int netStatus = QueryNetwork();
    
    if (netStatus & NET_WIFIREADY) {
        if (netStatus & NET_CONNECTED) {
            logMsg("WiFi already connected");
            return true;
        }
        logMsg("WiFi enabled but not connected yet");
        return false;
    }
    
    logMsg("WiFi is not enabled, attempting to enable");
    int result = WiFiPower(1);
    if (result != NET_OK) {
        logMsg("Failed to enable WiFi: %d", result);
        return false;
    }
    
    logMsg("WiFi power on initiated");
    return false; // Not connected yet, but enabling
}

void* connectionThreadFunc(void* arg) {
    logMsg("Connection thread started");
    isConnecting = true;
    
    updateConnectionStatus("Connecting...");
    
    const char* ip = ReadString(appConfig, KEY_IP, DEFAULT_IP);
    int port = ReadInt(appConfig, KEY_PORT, atoi(DEFAULT_PORT));
    
    const char* encryptedPassword = ReadString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
    std::string password;
    
    if (encryptedPassword && strlen(encryptedPassword) > 0) {
        if (encryptedPassword[0] == '$') {
            const char* decrypted = ReadSecret(appConfig, KEY_PASSWORD, "");
            if (decrypted) password = decrypted;
        } else {
            password = encryptedPassword;
        }
    } else {
        password = "";
    }
    
    logMsg("Connecting to %s:%d", ip, port);
    
    if (shouldStop) {
        logMsg("Connection cancelled before connect");
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        return NULL;
    }
    
    if (!networkManager->connectToServer(ip, port)) {
        logMsg("Connection failed");
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        notifyConnectionFailed("Failed to connect to Calibre server.\nPlease check IP address and port.");
        return NULL;
    }
    
    if (shouldStop) {
        logMsg("Connection cancelled after connect");
        networkManager->disconnect();
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        return NULL;
    }
    
    logMsg("Connected, starting handshake");
    updateConnectionStatus("Handshake...");
    
    if (!protocol->performHandshake(password)) {
        logMsg("Handshake failed: %s", protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        
        std::string errorMsg = "Handshake failed: ";
        errorMsg += protocol->getErrorMessage();
        notifyConnectionFailed(errorMsg.c_str());
        return NULL;
    }
    
    logMsg("Handshake successful");
    updateConnectionStatus("Connected");
    
    protocol->handleMessages([](const std::string& status) {
        // Callback from protocol
    });
    
    logMsg("Disconnecting");
    
    // Get books received count before disconnecting
    int booksReceived = protocol->getBooksReceivedCount();
    
    protocol->disconnect();
    networkManager->disconnect();
    
    updateConnectionStatus("Disconnected");
    isConnecting = false;
    
    // Notify about sync completion if any books were received
    if (booksReceived > 0) {
        notifySyncComplete(booksReceived);
    }
    
    return NULL;
}

void startConnection() {
    if (isConnecting) {
        logMsg("Already connecting, ignoring request");
        return;
    }
    
    logMsg("startConnection called");
    
    // Check WiFi status first
    if (!isWiFiReady()) {
        logMsg("WiFi not ready, attempting to enable");
        ShowHourglass();
        updateConnectionStatus("Enabling WiFi...");
        
        // Try to enable WiFi
        enableWiFi();
        
        // Give it time to connect (try for up to 5 seconds)
        for (int i = 0; i < 10; i++) {
            usleep(500000); // 0.5 seconds
            if (isWiFiReady()) {
                logMsg("WiFi became ready after %d attempts", i + 1);
                break;
            }
        }
        
        HideHourglass();
        
        if (!isWiFiReady()) {
            updateConnectionStatus("Disconnected (WiFi not ready)");
            logMsg("WiFi still not ready after enable attempt");
            return;
        }
        
        logMsg("WiFi is now ready");
    }
    
    isConnecting = true;
    shouldStop = false;
    
    if (!networkManager) networkManager = new NetworkManager();
    if (!bookManager) {
        bookManager = new BookManager();
        bookManager->initialize("");
    }
    
    if (!protocol) {
        const char* readCol = ReadString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
        const char* readDateCol = ReadString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
        const char* favCol = ReadString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
        
        protocol = new CalibreProtocol(networkManager, bookManager, 
                                      readCol ? readCol : "", 
                                      readDateCol ? readDateCol : "", 
                                      favCol ? favCol : "");
    }
    
    if (pthread_create(&connectionThread, NULL, connectionThreadFunc, NULL) != 0) {
        logMsg("Failed to create connection thread");
        updateConnectionStatus("Disconnected");
        isConnecting = false;
        return;
    }
}

void stopConnection() {
    logMsg("Stopping connection...");
    shouldStop = true;
    
    if (protocol) protocol->disconnect();
    if (networkManager) networkManager->disconnect();

    if (isConnecting) {
        logMsg("Connection thread is running, detaching for fast exit");
        pthread_detach(connectionThread);
        isConnecting = false;
    }
    
    snprintf(connectionStatusBuffer, sizeof(connectionStatusBuffer), "Disconnected");
}

void initConfig() {
    iv_buildpath("/mnt/ext1/system/config");
    appConfig = OpenConfig(CONFIG_FILE, configItems);
    
    if (!appConfig) {
        appConfig = OpenConfig(CONFIG_FILE, NULL);
        if (appConfig) {
            WriteString(appConfig, KEY_IP, DEFAULT_IP);
            WriteString(appConfig, KEY_PORT, DEFAULT_PORT);
            WriteString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
            WriteString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
            WriteString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
            WriteString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
            WriteString(appConfig, KEY_CONNECTION, "Disconnected");
            SaveConfig(appConfig);
        }
    }

    if (appConfig) {
        snprintf(connectionStatusBuffer, sizeof(connectionStatusBuffer), "Disconnected");
        WriteString(appConfig, KEY_CONNECTION, "Disconnected");
    }
}

void saveAndCloseConfig() {
    if (appConfig) {
        WriteString(appConfig, KEY_CONNECTION, "Disconnected");
        SaveConfig(appConfig);
        CloseConfig(appConfig);
        appConfig = NULL;
    }
}

void configSaveHandler() {
    logMsg("Config save handler called");
    if (appConfig) SaveConfig(appConfig);
}

void configItemChangedHandler(char *name) {
    logMsg("Config item changed: %s", name ? name : "NULL");
    if (appConfig) SaveConfig(appConfig);
}

void retryConnectionHandler(int button) {
    logMsg("Retry dialog closed with button: %d", button);
    
    if (button == 1) { // "Retry" button
        logMsg("User chose to retry connection");
        startConnection();
    } else { // "Cancel" button
        logMsg("User cancelled retry");
    }
}

void configCloseHandler() {
    logMsg("Config editor closed by user");
    performExit();
}

void showMainScreen() {
    ClearScreen();
    OpenConfigEditor(
        (char *)"Connect to Calibre",
        appConfig,
        configItems,
        configCloseHandler,
        configItemChangedHandler
    );
    FullUpdate();
}

void performExit() {
    if (exitRequested) {
        logMsg("Exit already in progress, ignoring");
        return;
    }
    
    exitRequested = true;
    logMsg("Performing exit");
    
    stopConnection();
    
    logMsg("Closing config editor...");
    CloseConfigLevel();
    
    saveAndCloseConfig();
    
    if (protocol) {
        delete protocol;
        protocol = NULL;
    }
    if (networkManager) {
        delete networkManager;
        networkManager = NULL;
    }
    if (bookManager) {
        delete bookManager;
        bookManager = NULL;
    }
    
    logMsg("Closing application normally");
    closeLog();
    
    CloseApp();
}

int mainEventHandler(int type, int par1, int par2) {
    if (type != EVT_POINTERMOVE && type != 49) {
        logMsg("Event: %d, p1: %d, p2: %d", type, par1, par2);
    }

    switch (type) {
        case EVT_INIT:
            initLog();
            logMsg("EVT_INIT");
            SetPanelType(PANEL_ENABLED);
            initConfig();
            showMainScreen();
            startConnection();
            break;
            
        case EVT_USER_UPDATE: {
            // Reopen config editor to refresh the display
            logMsg("Refreshing config editor");
            CloseConfigLevel();
            showMainScreen();
            break;
        }
            
        case EVT_CONNECTION_FAILED:
            logMsg("Showing connection failed dialog");
            Dialog(ICON_ERROR, 
                   "Connection Failed", 
                   connectionErrorBuffer,
                   "Retry", "Cancel", 
                   retryConnectionHandler);
            break;
            
        case EVT_SYNC_COMPLETE:
            logMsg("Showing sync complete message");
            Message(ICON_INFORMATION, 
                    "Sync Complete", 
                    syncCompleteBuffer, 
                    5000);
            break;
            
        case EVT_SHOW:
            SoftUpdate();
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                logMsg("Hardware KEY_BACK pressed - Exiting");
                performExit();
                return 1;
            }
            break;

        case EVT_EXIT:
            logMsg("EVT_EXIT received");
            if (!exitRequested) {
                exitRequested = true;
                stopConnection();
                saveAndCloseConfig();
                
                if (protocol) {
                    delete protocol;
                    protocol = NULL;
                }
                if (networkManager) {
                    delete networkManager;
                    networkManager = NULL;
                }
                if (bookManager) {
                    delete bookManager;
                    bookManager = NULL;
                }
                
                closeLog();
            }
            return 1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    logMsg("After InkViewMain - this should not happen");
    return 0;
}
