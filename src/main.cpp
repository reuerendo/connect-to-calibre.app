void performExit();
void delayedConnectionStart();#include "inkview.h"
#include "network.h"
#include "calibre_protocol.h"
#include "book_manager.h"
#include "cache_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>

#define EVT_USER_UPDATE 20001
#define EVT_CONNECTION_FAILED 20002
#define EVT_SYNC_COMPLETE 20003

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

static iconfig *appConfig = NULL;
static const char *CONFIG_FILE = "/mnt/ext1/system/config/calibre-connect.cfg";

static const char *KEY_IP = "ip";
static const char *KEY_PORT = "port";
static const char *KEY_PASSWORD = "password";
static const char *KEY_READ_COLUMN = "read_column";
static const char *KEY_READ_DATE_COLUMN = "read_date_column";
static const char *KEY_FAVORITE_COLUMN = "favorite_column";
static const char *KEY_CONNECTION = "connection_enabled";

static char connectionStatusBuffer[128] = "Disconnected"; 
static char connectionErrorBuffer[256] = "";
static char syncCompleteBuffer[256] = "";
static int booksReceivedCount = 0;

static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "9090";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "#read";
static const char *DEFAULT_READ_DATE_COLUMN = "#read_date";
static const char *DEFAULT_FAVORITE_COLUMN = "#favorite";

static NetworkManager* networkManager = NULL;
static BookManager* bookManager = NULL;
static CacheManager* cacheManager = NULL;
static CalibreProtocol* protocol = NULL;
static pthread_t connectionThread;
static bool isConnecting = false;
static bool shouldStop = false;
static volatile bool exitRequested = false;

int mainEventHandler(int type, int par1, int par2);

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
    
    if (appConfig) {
        WriteString(appConfig, KEY_CONNECTION, status);
    }
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

void* connectionThreadFunc(void* arg) {
    logMsg("Connection thread started");
    isConnecting = true;
    
    updateConnectionStatus("Connecting...");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    
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
        SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
        return NULL;
    }
    
    if (!networkManager->connectToServer(ip, port)) {
        logMsg("Connection failed");
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
        notifyConnectionFailed("Failed to connect to Calibre server.\nPlease check IP address and port.");
        return NULL;
    }
    
    if (shouldStop) {
        logMsg("Connection cancelled after connect");
        networkManager->disconnect();
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
        return NULL;
    }
    
    logMsg("Connected, starting handshake");
    updateConnectionStatus("Handshake...");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    
    if (!protocol->performHandshake(password)) {
        logMsg("Handshake failed: %s", protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        updateConnectionStatus("Disconnected");
        SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
        
        std::string errorMsg = "Handshake failed: ";
        errorMsg += protocol->getErrorMessage();
        notifyConnectionFailed(errorMsg.c_str());
        return NULL;
    }
    
    logMsg("Handshake successful");
    updateConnectionStatus("Connected");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    
    protocol->handleMessages([](const std::string& status) {
    });
    
    logMsg("Disconnecting");
    
    int booksReceived = protocol->getBooksReceivedCount();
    
    protocol->disconnect();
    networkManager->disconnect();
    
    updateConnectionStatus("Disconnected");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    isConnecting = false;
    
    if (booksReceived > 0) {
        notifySyncComplete(booksReceived);
    }
    
    return NULL;
}

void startCalibreConnection() {
    if (isConnecting) {
        logMsg("Connection already in progress");
        return;
    }
    
    logMsg("Starting Calibre connection thread");
    
    isConnecting = true;
    shouldStop = false;
    
    if (!networkManager) networkManager = new NetworkManager();
    if (!bookManager) {
        bookManager = new BookManager();
        bookManager->initialize("");
    }
    if (!cacheManager) {
        cacheManager = new CacheManager();
    }
    
    if (!protocol) {
        const char* readCol = ReadString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
        const char* readDateCol = ReadString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
        const char* favCol = ReadString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
        
        protocol = new CalibreProtocol(networkManager, bookManager, cacheManager,
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

void delayedConnectionStart() {
    logMsg("Delayed connection start");
    
    // Check current WiFi state
    NET_STATE currentState = GetNetState();
    logMsg("Current NET_STATE: %d (DISCONNECTED=%d, CONNECTING=%d, CONNECTED=%d)", 
           currentState, DISCONNECTED, CONNECTING, CONNECTED);
    
    if (currentState == CONNECTED) {
        logMsg("WiFi already connected");
        updateConnectionStatus("WiFi connected");
        SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
        startCalibreConnection();
        return;
    }
    
    // Enable WiFi if disabled
    logMsg("Enabling WiFi module");
    updateConnectionStatus("Enabling WiFi...");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    
    if (WiFiPower(1) != 0) {
        logMsg("Failed to enable WiFi");
        updateConnectionStatus("WiFi failed");
        notifyConnectionFailed("Cannot enable WiFi.\nPlease check device settings.");
        return;
    }
    
    // Wait for WiFi module initialization
    logMsg("Waiting for WiFi module initialization");
    int attempts = 0;
    while (attempts < 10) {
        usleep(500000); // 0.5 seconds
        NET_STATE state = GetNetState();
        logMsg("Init wait attempt %d, state: %d", attempts, state);
        if (state != DISCONNECTED) break;
        attempts++;
    }
    
    if (attempts >= 10) {
        logMsg("WiFi module initialization timeout");
        updateConnectionStatus("WiFi timeout");
        notifyConnectionFailed("WiFi module initialization timeout.\nPlease try again.");
        return;
    }
    
    // Try to connect to WiFi
    logMsg("Attempting WiFi connection");
    updateConnectionStatus("Connecting to WiFi...");
    SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
    
    int connectResult = NetConnect(NULL);
    logMsg("NetConnect result: %d (NET_OK=%d)", connectResult, NET_OK);
    
    if (connectResult == NET_OK || connectResult == NET_CONNECT) {
        // Wait for connection to establish
        logMsg("Waiting for WiFi connection");
        attempts = 0;
        while (attempts < 20) { // maximum 10 seconds
            usleep(500000); // 0.5 seconds
            NET_STATE state = GetNetState();
            
            if (attempts % 4 == 0) { // Update status every 2 seconds
                char statusBuf[64];
                snprintf(statusBuf, sizeof(statusBuf), "Connecting to WiFi... %ds", attempts / 2);
                updateConnectionStatus(statusBuf);
                SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
            }
            
            logMsg("Connection wait attempt %d, state: %d", attempts, state);
            
            if (state == CONNECTED) {
                logMsg("WiFi connected successfully!");
                updateConnectionStatus("WiFi connected");
                SendEvent(mainEventHandler, EVT_USER_UPDATE, 0, 0);
                startCalibreConnection();
                return;
            }
            
            attempts++;
        }
        
        logMsg("WiFi connection timeout");
        updateConnectionStatus("Connection timeout");
        notifyConnectionFailed("Failed to connect to WiFi.\nConnection timeout.");
    } else {
        logMsg("NetConnect failed with code: %d", connectResult);
        updateConnectionStatus("WiFi failed");
        
        const char* errorMsg = NULL;
        switch (connectResult) {
            case NET_FAIL:       errorMsg = "Network connection failed"; break;
            case NET_ENOTCONF:   errorMsg = "No WiFi network configured.\nPlease configure WiFi in device settings."; break;
            case NET_EWRONGKEY:  errorMsg = "Wrong WiFi password"; break;
            case NET_EAUTH:      errorMsg = "WiFi authentication failed"; break;
            case NET_ETIMEOUT:   errorMsg = "Connection timeout"; break;
            case NET_EDISABLED:  errorMsg = "WiFi disabled"; break;
            case NET_ABORTED:    errorMsg = "Connection cancelled by user"; break;
            default:             errorMsg = "WiFi connection failed"; break;
        }
        
        notifyConnectionFailed(errorMsg);
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
    
    if (button == 1) {
        logMsg("User chose to retry connection");
        delayedConnectionStart();
    } else {
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
}

void performExit() {
    if (exitRequested) {
        logMsg("Exit already in progress, ignoring");
        return;
    }
    
    exitRequested = true;
    logMsg("Performing exit");
    
    ClearTimerByName("connect_timer");
    
    stopConnection();
    
    logMsg("Closing config editor...");
    CloseConfigLevel();
    
    saveAndCloseConfig();
    
    if (protocol) {
        delete protocol;
        protocol = NULL;
    }
    if (cacheManager) {
        delete cacheManager;
        cacheManager = NULL;
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
            break;
            
        case EVT_USER_UPDATE:
            logMsg("EVT_USER_UPDATE - refreshing config editor");
            UpdateCurrentConfigPage();
            break;
            
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
            Message(ICON_INFORMATION, "Success", syncCompleteBuffer, 3000);
            break;
            
        case EVT_SHOW:
            logMsg("EVT_SHOW - starting delayed connection");
            SoftUpdate();
            SetWeakTimer("connect_timer", delayedConnectionStart, 500);
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
                if (cacheManager) {
                    delete cacheManager;
                    cacheManager = NULL;
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
