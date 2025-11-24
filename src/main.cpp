#include "inkview.h"
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
#include <sys/stat.h>

// Custom events
#define EVT_USER_UPDATE 20001
#define EVT_CONNECTION_FAILED 20002
#define EVT_BOOK_RECEIVED 20004
#define EVT_SHOW_TOAST 20005

// Toast types
#define TOAST_CONNECTED 2
#define TOAST_DISCONNECTED 3

#define MAX_LOG_SIZE (256 * 1024)

// Debug logging
static FILE* logFile = NULL;

void initLog() {
    const char* logPath = "/mnt/ext1/system/calibre-connect.log";
    
    struct stat st;
    if (stat(logPath, &st) == 0) {
        if (st.st_size >= MAX_LOG_SIZE) {
            remove(logPath); 
        }
    }

    logFile = iv_fopen(logPath, "a");
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "\n= Calibre Connect Started [%s] =\n", ctime(&now));
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
        // Log closing time only
        time_t now = time(NULL);
        fprintf(logFile, "= Calibre Connect Closed [%s] =\n", ctime(&now));
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

// Global error message buffer
static char connectionErrorBuffer[256] = "";

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
static CacheManager* cacheManager = NULL;
static CalibreProtocol* protocol = NULL;
static pthread_t connectionThread;
static bool isConnecting = false;
static bool shouldStop = false;
static volatile bool exitRequested = false;

static pthread_mutex_t bookCountMutex = PTHREAD_MUTEX_INITIALIZER;
static int pendingBookCount = 0;
static bool bookCountTimerActive = false;
static int booksReceivedCount = 0;
static bool syncMessageShown = false;

// Forward declarations
int mainEventHandler(int type, int par1, int par2);
void performExit();
void startConnection();

// Config editor structure
static iconfigedit configItems[] = {
    {
        CFG_IPADDR,
        NULL,
        (char *)"     IP Address",
        NULL,
        (char *)KEY_IP,
        (char *)DEFAULT_IP,
        NULL,
        NULL
    },
    {
        CFG_NUMBER,
        NULL,
        (char *)"     Port",
        NULL,
        (char *)KEY_PORT,
        (char *)DEFAULT_PORT,
        NULL,
        NULL
    },
    {
        CFG_PASSWORD,
        NULL,
        (char *)"     Password",
        NULL,
        (char *)KEY_PASSWORD,
        (char *)DEFAULT_PASSWORD,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"     Read Status Column",
        NULL,
        (char *)KEY_READ_COLUMN,
        (char *)DEFAULT_READ_COLUMN,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"     Read Date Column",
        NULL,
        (char *)KEY_READ_DATE_COLUMN,
        (char *)DEFAULT_READ_DATE_COLUMN,
        NULL,
        NULL
    },
    {
        CFG_TEXT,
        NULL,
        (char *)"     Favorite Column",
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

void notifyConnectionFailed(const char* errorMsg) {
    logMsg("Connection failed: %s", errorMsg);
    snprintf(connectionErrorBuffer, sizeof(connectionErrorBuffer), "%s", errorMsg);
    SendEvent(mainEventHandler, EVT_CONNECTION_FAILED, 0, 0);
}

void showBookCountMessage() {
    pthread_mutex_lock(&bookCountMutex);
    
    if (pendingBookCount > 0) {
        SendEvent(mainEventHandler, EVT_BOOK_RECEIVED, pendingBookCount, 0);
        pendingBookCount = 0;
    }
    
    bookCountTimerActive = false;
    pthread_mutex_unlock(&bookCountMutex);
}

void* connectionThreadFunc(void* arg) {
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
    
    booksReceivedCount = 0;
    syncMessageShown = false;
    
    if (shouldStop) {
        isConnecting = false;
        return NULL;
    }
    
    if (!networkManager->connectToServer(ip, port)) {
        isConnecting = false;
        notifyConnectionFailed("Failed to connect to Calibre server.\nPlease check IP address and port.");
        return NULL;
    }
    
    if (shouldStop) {
        networkManager->disconnect();
        isConnecting = false;
        return NULL;
    }
    
    if (!protocol->performHandshake(password)) {
        logMsg("Handshake failed: %s", protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        
        std::string errorMsg = "Handshake failed: ";
        errorMsg += protocol->getErrorMessage();
        notifyConnectionFailed(errorMsg.c_str());
        return NULL;
    }
    
    logMsg("Handshake successful");
    SendEvent(mainEventHandler, EVT_SHOW_TOAST, TOAST_CONNECTED, 0);
    
    protocol->handleMessages([](const std::string& status) {
        if (status == "BOOK_SAVED") {
            int count = protocol->getBooksReceivedCount();
            SendEvent(mainEventHandler, EVT_BOOK_RECEIVED, count, 0);
        }
    });
    
    logMsg("Disconnecting");
    
    protocol->disconnect();
    networkManager->disconnect();
    
    isConnecting = false;
    
    SendEvent(mainEventHandler, EVT_SHOW_TOAST, TOAST_DISCONNECTED, 0);
    
    return NULL;
}

void startCalibreConnection() {
    if (isConnecting) {
        return;
    }
    
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
        logMsg("Failed to create thread");
        isConnecting = false;
        return;
    }
}

// Primary entry point for connection logic.
void startConnection() {
    if (isConnecting) {
        return;
    }
    
    // 1. Check if we are already connected
    iv_netinfo* netInfo = NetInfo();
    if (netInfo && netInfo->connected) {
        startCalibreConnection();
        return;
    }

    // 2. Not connected? Use standard SDK dialog to connect.
    
    // NetConnect(NULL) calls the native system dialog.
    int netResult = NetConnect(NULL);

    if (netResult == NET_OK) {
        startCalibreConnection();
    } else {
        logMsg("WiFi connection failed: %d", netResult);
        notifyConnectionFailed("Could not connect to WiFi network.");
    }
}

// Timer callback to delay connection start until UI is drawn
void connectionTimerFunc() {
    ClearTimer((iv_timerproc)connectionTimerFunc);
    startConnection();
}

void stopConnection() {
    shouldStop = true;
    
    if (protocol) protocol->disconnect();
    if (networkManager) networkManager->disconnect();

    if (isConnecting) {
        pthread_detach(connectionThread);
        isConnecting = false;
    }
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
            SaveConfig(appConfig);
        }
    }
}

void saveAndCloseConfig() {
    if (appConfig) {
        SaveConfig(appConfig);
        CloseConfig(appConfig);
        appConfig = NULL;
    }
}

void configSaveHandler() {
    if (appConfig) SaveConfig(appConfig);
}

void configItemChangedHandler(char *name) {
    if (appConfig) SaveConfig(appConfig);
}

void retryConnectionHandler(int button) {
    if (button == 1) {
        SoftUpdate(); 
        startConnection();
    }
}

void configCloseHandler() {
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

void updateConnectionStatus(const char* status) {
    logMsg("Status: %s", status);
}

void finalSyncMessageTimer() {
    ClearTimer((iv_timerproc)finalSyncMessageTimer);
    
    if (syncMessageShown) {
        return;
    }
    
    logMsg("Timer fired: Batch sync finished");
    syncMessageShown = true;
    
    char msgBuffer[128];
    snprintf(msgBuffer, sizeof(msgBuffer),
             "Batch sync finished.\nTotal received: %d book%s.",
             booksReceivedCount, booksReceivedCount == 1 ? "" : "s");
    
    Message(ICON_INFORMATION, "Sync Complete", msgBuffer, 4000);
    
    updateConnectionStatus("Connected (Idle)");
    SoftUpdate();
}

void performExit() {
    if (exitRequested) {
        return;
    }
    
    exitRequested = true;
    
    ClearTimer((iv_timerproc)connectionTimerFunc);
    ClearTimer((iv_timerproc)finalSyncMessageTimer);
    
    stopConnection();
    
    CloseConfigLevel();
    saveAndCloseConfig();
    
    if (protocol) { delete protocol; protocol = NULL; }
    if (cacheManager) { delete cacheManager; cacheManager = NULL; }
    if (networkManager) { delete networkManager; networkManager = NULL; }
    if (bookManager) { delete bookManager; bookManager = NULL; }
    
    closeLog();
    CloseApp();
}

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            initLog();
            SetPanelType(PANEL_ENABLED);
            initConfig();
            showMainScreen();
            SoftUpdate();
            SetWeakTimer("ConnectTimer", connectionTimerFunc, 300);
            break;
            
        case EVT_USER_UPDATE:
            PartialUpdate(0, 0, ScreenWidth(), ScreenHeight());
            break;
        
        case EVT_NET_CONNECTED:
            if (!isConnecting && !networkManager->isConnected()) {
                startCalibreConnection();
            }
            break;
            
        case EVT_NET_DISCONNECTED:
            if (isConnecting) {
                stopConnection();
            }
            break;
            
        case EVT_CONNECTION_FAILED:
            Dialog(ICON_ERROR, 
                   "Connection Failed", 
                   connectionErrorBuffer,
                   "Retry", "Cancel", 
                   retryConnectionHandler);
            break;

        case EVT_BOOK_RECEIVED: {
            int count = par1;
            
            booksReceivedCount = count;
            syncMessageShown = false;
            
            char statusBuffer[64];
            snprintf(statusBuffer, sizeof(statusBuffer), "Receiving... (%d book%s)",
                     count, count == 1 ? "" : "s");
            updateConnectionStatus(statusBuffer);
            SoftUpdate();
            
            ClearTimer((iv_timerproc)finalSyncMessageTimer);
            SetWeakTimer("SyncFinalize", (iv_timerproc)finalSyncMessageTimer, 500);
            
            break;
        }

        case EVT_SHOW_TOAST:
            if (par1 == TOAST_CONNECTED) {
                Message(ICON_INFORMATION, "Calibre", "Connected Successfully", 2000);
                updateConnectionStatus("Connected (Idle)");
            } else if (par1 == TOAST_DISCONNECTED) {
                Message(ICON_INFORMATION, "Calibre", "Disconnected", 2000);
                updateConnectionStatus("Disconnected");
            }
            break;
            
        case EVT_SHOW:
            SoftUpdate();
            break;
            
        // case EVT_KEYPRESS:
            // if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                // performExit();
                // return 1;
            // }
            // break;

        case EVT_EXIT:
            if (!exitRequested) {
                exitRequested = true;
                
                ClearTimer((iv_timerproc)connectionTimerFunc);
                ClearTimer((iv_timerproc)finalSyncMessageTimer);
                
                stopConnection();
                saveAndCloseConfig();
                
                if (protocol) { delete protocol; protocol = NULL; }
                if (cacheManager) { delete cacheManager; cacheManager = NULL; }
                if (networkManager) { delete networkManager; networkManager = NULL; }
                if (bookManager) { delete bookManager; bookManager = NULL; }
                closeLog();
            }
            return 1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    return 0;
}
