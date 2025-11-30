#include "inkview.h"
#include "network.h"
#include "calibre_protocol.h"
#include "book_manager.h"
#include "cache_manager.h"
#include "i18n.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

// Modern C++ includes
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <string>

// Custom events
#define EVT_USER_UPDATE 20001
#define EVT_CONNECTION_FAILED 20002
#define EVT_BOOK_RECEIVED 20004
#define EVT_SHOW_TOAST 20005
#define EVT_BATCH_COMPLETE 20006

// Toast types
#define TOAST_CONNECTED 2
#define TOAST_DISCONNECTED 3

#define MAX_LOG_SIZE (256 * 1024)

// --- Logging System (Thread Safe) ---
static std::atomic<bool> isLoggingEnabled(false);
static FILE* logFile = NULL;
static std::mutex logMutex; // Mutex for log file access

void initLog() {
    if (!isLoggingEnabled) return;

    const char* logPath = "/mnt/ext1/system/calibre-connect.log";
    
    std::lock_guard<std::mutex> lock(logMutex);

    if (logFile) return;

    struct stat st;
    if (stat(logPath, &st) == 0) {
        if (st.st_size >= MAX_LOG_SIZE) {
            remove(logPath); 
        }
    }

    logFile = fopen(logPath, "a");
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "\n= Calibre Connect Started [%s] =\n", ctime(&now));
        fflush(logFile);
    }
}

void logMsg(const char* format, ...) {
    if (!isLoggingEnabled) return;

    std::lock_guard<std::mutex> lock(logMutex);
    
    if (!logFile) return;
    
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
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "= Calibre Connect Closed [%s] =\n", ctime(&now));
        fflush(logFile);
        fclose(logFile);
        logFile = NULL;
    }
}

// --- Global Config ---
static iconfig *appConfig = NULL;
static const char *CONFIG_FILE = "/mnt/ext1/system/config/calibre-connect.cfg";

// Config keys
static const char *KEY_IP = "ip";
static const char *KEY_PORT = "port";
static const char *KEY_PASSWORD = "password";
static const char *KEY_READ_COLUMN = "read_column";
static const char *KEY_READ_DATE_COLUMN = "read_date_column";
static const char *KEY_FAVORITE_COLUMN = "favorite_column";

// Log
static const char *KEY_ENABLE_LOG = "enable_logging";
static const char *DEFAULT_ENABLE_LOG = "0";

// Default values
static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "9090";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "#read";
static const char *DEFAULT_READ_DATE_COLUMN = "#read_date";
static const char *DEFAULT_FAVORITE_COLUMN = "#favorite";
static const char* CHECKBOX_VARIANTS[] = {
    NULL,
    NULL,
    NULL
};

// Global error message buffer
static char connectionErrorBuffer[256] = "";

// --- State Management (RAII & Atomic) ---
static std::unique_ptr<NetworkManager> networkManager;
static std::unique_ptr<BookManager> bookManager;
static std::unique_ptr<CacheManager> cacheManager;
static std::unique_ptr<CalibreProtocol> protocol;

static std::thread connectionThread;
static std::atomic<bool> isConnecting(false);
static std::atomic<bool> shouldStop(false);
static std::atomic<bool> exitRequested(false);

static std::mutex bookCountMutex;
static int booksReceivedCount = 0;

// --- Helper Struct for Thread Safety ---
// Holds a copy of config data so the worker thread doesn't access 'appConfig'
struct ConnectionConfig {
    std::string ip;
    int port;
    std::string password;
};

// Forward declarations
int mainEventHandler(int type, int par1, int par2);
void performExit();
void startConnection();

// Config editor structure
static iconfigedit* configItems = NULL;

void initConfigItems() {
	
	CHECKBOX_VARIANTS[0] = i18n_get(STR_OFF);
    CHECKBOX_VARIANTS[1] = i18n_get(STR_ON);
    CHECKBOX_VARIANTS[2] = NULL; // Terminator
	
    // Using calloc to ensure C-compatibility for InkView API
    configItems = (iconfigedit*)calloc(8, sizeof(iconfigedit));
    
    configItems[0].type = CFG_IPADDR;
    configItems[0].text = strdup(i18n_get(STR_IP_ADDRESS));
    configItems[0].name = (char*)KEY_IP;
    configItems[0].deflt = (char*)DEFAULT_IP;
    
    configItems[1].type = CFG_NUMBER;
    configItems[1].text = strdup(i18n_get(STR_PORT));
    configItems[1].name = (char*)KEY_PORT;
    configItems[1].deflt = (char*)DEFAULT_PORT;
    
    configItems[2].type = CFG_PASSWORD;
    configItems[2].text = strdup(i18n_get(STR_PASSWORD));
    configItems[2].name = (char*)KEY_PASSWORD;
    configItems[2].deflt = (char*)DEFAULT_PASSWORD;
    
    configItems[3].type = CFG_TEXT;
    configItems[3].text = strdup(i18n_get(STR_READ_COLUMN));
    configItems[3].name = (char*)KEY_READ_COLUMN;
    configItems[3].deflt = (char*)DEFAULT_READ_COLUMN;
    
    configItems[4].type = CFG_TEXT;
    configItems[4].text = strdup(i18n_get(STR_READ_DATE_COLUMN));
    configItems[4].name = (char*)KEY_READ_DATE_COLUMN;
    configItems[4].deflt = (char*)DEFAULT_READ_DATE_COLUMN;
    
    configItems[5].type = CFG_TEXT;
    configItems[5].text = strdup(i18n_get(STR_FAVORITE_COLUMN));
    configItems[5].name = (char*)KEY_FAVORITE_COLUMN;
    configItems[5].deflt = (char*)DEFAULT_FAVORITE_COLUMN;
	
	configItems[6].type = CFG_INDEX;
    configItems[6].text = strdup(i18n_get(STR_ENABLE_LOG));
    configItems[6].name = (char*)KEY_ENABLE_LOG;
    configItems[6].deflt = (char*)DEFAULT_ENABLE_LOG;
    configItems[6].variants = (char**)CHECKBOX_VARIANTS;
    
    // Terminator
    configItems[7].type = 0;
}

void freeConfigItems() {
    if (configItems) {
        for (int i = 0; i < 7; i++) {
            if (configItems[i].text) free(configItems[i].text);
        }
        free(configItems);
        configItems = NULL;
    }
}

void notifyConnectionFailed(const char* errorMsg) {
    logMsg("Connection failed: %s", errorMsg);
    snprintf(connectionErrorBuffer, sizeof(connectionErrorBuffer), "%s", errorMsg);
    SendEvent(mainEventHandler, EVT_CONNECTION_FAILED, 0, 0);
}

// Thread function - accepts config by value to avoid race conditions
void connectionThreadFunc(ConnectionConfig config) {
    logMsg("Connecting to %s:%d", config.ip.c_str(), config.port);
    
    booksReceivedCount = 0;
    
    if (shouldStop) {
        isConnecting = false;
        return;
    }
    
    // Use the smart pointers initialized in startCalibreConnection
    if (!networkManager->connectToServer(config.ip, config.port)) {
        isConnecting = false;
        
        char errorMsg[512];
        snprintf(errorMsg, sizeof(errorMsg), "%s.\n%s",
                i18n_get(STR_FAILED_CONNECT_SERVER),
                i18n_get(STR_CHECK_IP_PORT));
        notifyConnectionFailed(errorMsg);
        return;
    }
    
    if (shouldStop) {
        networkManager->disconnect();
        isConnecting = false;
        return;
    }
    
    if (!protocol->performHandshake(config.password)) {
        logMsg("Handshake failed: %s", protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        
        char errorMsg[512];
        snprintf(errorMsg, sizeof(errorMsg), "%s: %s",
                i18n_get(STR_HANDSHAKE_FAILED),
                protocol->getErrorMessage().c_str());
        notifyConnectionFailed(errorMsg);
        return;
    }
    
    logMsg("Handshake successful");
    SendEvent(mainEventHandler, EVT_SHOW_TOAST, TOAST_CONNECTED, 0);
    
    // Clean password from memory for security
    std::fill(config.password.begin(), config.password.end(), 0);
    
	protocol->handleMessages([](const std::string& status) {
			if (status == "BOOK_RECEIVED") {
				int count = protocol->getBooksReceivedCount();
				SendEvent(mainEventHandler, EVT_BOOK_RECEIVED, count, 0);
			} else if (status == "BATCH_COMPLETE") {
				int count = protocol->getLastBatchCount();
				SendEvent(mainEventHandler, EVT_BATCH_COMPLETE, count, 0);
			}
		});
    
    logMsg("Disconnecting");
    
    if (protocol) protocol->disconnect();
    if (networkManager) networkManager->disconnect();
    
    isConnecting = false;
    SendEvent(mainEventHandler, EVT_SHOW_TOAST, TOAST_DISCONNECTED, 0);
}

void startCalibreConnection() {
    if (isConnecting) {
        return;
    }
    
    isConnecting = true;
    shouldStop = false;
    
    // --- 1. Prepare Managers (Main Thread) ---
    
    if (!networkManager) {
        networkManager.reset(new NetworkManager());
    }
    
    if (!bookManager) {
        bookManager.reset(new BookManager());
        bookManager->initialize("");
        
        // Log SD card status at connection start
        if (bookManager->hasSDCard()) {
            logMsg("Connection started - SD Card available: %s", bookManager->getSDCardPath().c_str());
        } else {
            logMsg("Connection started - No SD Card detected");
        }
    }
    
    if (!cacheManager) {
        cacheManager.reset(new CacheManager());
    }
    
    // --- 2. Read Configuration (Main Thread) ---
    ConnectionConfig config;
    config.ip = ReadString(appConfig, KEY_IP, DEFAULT_IP);
    config.port = ReadInt(appConfig, KEY_PORT, atoi(DEFAULT_PORT));
    
    const char* encryptedPassword = ReadString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
    if (encryptedPassword && strlen(encryptedPassword) > 0) {
        if (encryptedPassword[0] == '$') {
            const char* decrypted = ReadSecret(appConfig, KEY_PASSWORD, "");
            if (decrypted) config.password = decrypted;
        } else {
            config.password = encryptedPassword;
        }
    }
    
    // Re-create protocol with current settings
    const char* readCol = ReadString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
    const char* readDateCol = ReadString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
    const char* favCol = ReadString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
    
    protocol.reset(new CalibreProtocol(
        networkManager.get(), 
        bookManager.get(), 
        cacheManager.get(),
        readCol ? readCol : "", 
        readDateCol ? readDateCol : "", 
        favCol ? favCol : ""
    ));
    
    // --- 3. Start Thread ---
    if (connectionThread.joinable()) {
        connectionThread.join();
    }
    
    try {
        connectionThread = std::thread(connectionThreadFunc, config);
    } catch (const std::system_error& e) {
        logMsg("Failed to create thread: %s", e.what());
        isConnecting = false;
    }
}

void startConnection() {
    if (isConnecting) return;
    
    iv_netinfo* netInfo = NetInfo();
    if (netInfo && netInfo->connected) {
        startCalibreConnection();
        return;
    }

    int netResult = NetConnect(NULL);

    if (netResult == NET_OK) {
        startCalibreConnection();
    } else {
        logMsg("WiFi connection failed: %d", netResult);
        notifyConnectionFailed(i18n_get(STR_WIFI_CONNECT_FAILED));
    }
}

void connectionTimerFunc() {
    ClearTimer((iv_timerproc)connectionTimerFunc);
    startConnection();
}

void stopConnection() {
    shouldStop = true;
    
    // Trigger disconnect to break blocking socket calls
    if (protocol) protocol->disconnect();
    if (networkManager) networkManager->disconnect();

    // Wait for thread to finish if it's running
    if (connectionThread.joinable()) {
        connectionThread.join();
    }
    
    isConnecting = false;
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
			WriteString(appConfig, KEY_ENABLE_LOG, DEFAULT_ENABLE_LOG);
            SaveConfig(appConfig);
        }
    }
	
	if (appConfig) {
        int logState = ReadInt(appConfig, KEY_ENABLE_LOG, atoi(DEFAULT_ENABLE_LOG));
        isLoggingEnabled = (logState != 0);
        
        if (isLoggingEnabled) {
            initLog();
        } else {
            closeLog();
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
    if (appConfig) {
        SaveConfig(appConfig);

        // Проверяем, изменилась ли настройка логов
        if (strcmp(name, KEY_ENABLE_LOG) == 0) {
            int logState = ReadInt(appConfig, KEY_ENABLE_LOG, 0);
            bool newState = (logState != 0);
            
            if (newState != isLoggingEnabled) {
                isLoggingEnabled = newState;
                if (isLoggingEnabled) {
                    initLog();
                    logMsg("Logging enabled by user");
                } else {
                    logMsg("Logging disabled by user");
                    closeLog();
                }
            }
        }
    }
}

void retryConnectionHandler(int button) {
    if (button == 2) { // "Retry" button
        SoftUpdate(); 
        startConnection();
    }
}

void configCloseHandler() {
    performExit();
}

void showMainScreen() {
    ClearScreen();
    char* title = strdup(i18n_get(STR_APP_TITLE));
    OpenConfigEditor(title, appConfig, configItems, configCloseHandler, configItemChangedHandler);
    free(title);
}

void updateConnectionStatus(const char* status) {
    logMsg("Status: %s", status);
    // Optional: Draw status on screen if UI requires it
}

void performExit() {
    if (exitRequested) return;
    exitRequested = true;
    
    // 2. Stop Network & Thread
    stopConnection();
    
    // 3. Save Config
    CloseConfigLevel();
    saveAndCloseConfig();
    
    // 4. Release Resources (RAII handles deletion)
    protocol.reset();
    cacheManager.reset();
    networkManager.reset();
    bookManager.reset();
    
    freeConfigItems();
    closeLog(); // Thread-safe close
    CloseApp();
}

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            i18n_init();
            initConfigItems();
            SetPanelType(PANEL_ENABLED);
            initConfig();
            
            // Check and log SD card status
            if (bookManager) {
                if (bookManager->hasSDCard()) {
                    logMsg("SD Card detected: %s", bookManager->getSDCardPath().c_str());
                    updateConnectionStatus("SD Card available");
                } else {
                    logMsg("No SD Card detected");
                    updateConnectionStatus("No SD Card");
                }
            }
            
            showMainScreen();
            SoftUpdate();
            SetWeakTimer("ConnectTimer", (iv_timerproc)connectionTimerFunc, 300);
            break;
            
        case EVT_USER_UPDATE:
            PartialUpdate(0, 0, ScreenWidth(), ScreenHeight());
            break;
        
        case EVT_NET_CONNECTED:
            if (!isConnecting && (!networkManager || !networkManager->isConnected())) {
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
                   i18n_get(STR_CONNECTION_FAILED), 
                   connectionErrorBuffer,
                   i18n_get(STR_CANCEL), 
                   i18n_get(STR_RETRY),
                   retryConnectionHandler);
            break;

		case EVT_BOOK_RECEIVED: {
            int count = par1;
            booksReceivedCount = count;
            
            char statusBuffer[128];
            snprintf(statusBuffer, sizeof(statusBuffer), "%s (%d)",
                     i18n_get(STR_RECEIVING), count);
            updateConnectionStatus(statusBuffer);
            SoftUpdate();
            break;
        }

        case EVT_BATCH_COMPLETE: {
            int count = par1;
            
            if (count > 0) {
                char msgBuffer[256];
                snprintf(msgBuffer, sizeof(msgBuffer),
                         "%s: %d",
                         i18n_get(STR_BOOKS_RECEIVED),
                         count);
                
                Message(ICON_INFORMATION, i18n_get(STR_SYNC_COMPLETE), msgBuffer, 4000);
            }
            
            updateConnectionStatus(i18n_get(STR_CONNECTED_IDLE));
            SoftUpdate();
            break;
        }

        case EVT_SHOW_TOAST:
            if (par1 == TOAST_CONNECTED) {
                Message(ICON_INFORMATION, "Calibre", i18n_get(STR_CONNECTED), 2000);
                updateConnectionStatus(i18n_get(STR_CONNECTED_IDLE));
            } else if (par1 == TOAST_DISCONNECTED) {
                Message(ICON_INFORMATION, "Calibre", i18n_get(STR_DISCONNECTED), 2000);
                updateConnectionStatus(i18n_get(STR_DISCONNECTED));
            }
            break;
            
        case EVT_SHOW:
            SoftUpdate();
            break;

        case EVT_EXIT:
            performExit();
            return 1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    return 0;
}




