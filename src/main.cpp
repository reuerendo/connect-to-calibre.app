#include "inkview.h"
#include "network.h"
#include "calibre_protocol.h"
#include "book_manager.h"  // <-- Добавлено: необходимо подключить заголовок
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

// Define DEBUG_LOG macro
#define DEBUG_LOG(fmt, ...) logMsg(fmt, ##__VA_ARGS__)

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
        initLog(); // Try to reopen if closed
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

// Default values
static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "9090";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "#read";
static const char *DEFAULT_READ_DATE_COLUMN = "#read_date";
static const char *DEFAULT_FAVORITE_COLUMN = "#favorite";

// Connection state
static NetworkManager* networkManager = NULL;
static BookManager* bookManager = NULL; // <-- Добавлено: глобальная переменная
static CalibreProtocol* protocol = NULL;
static pthread_t connectionThread;
static bool isConnecting = false;
static bool shouldStop = false;

// Config editor structure
static iconfigedit configItems[] = {
    {
        CFG_INFO,
        NULL,
        (char*)"Connection",
        NULL,
        (char*)KEY_CONNECTION,
        (char*)"Disconnected",
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
    if (appConfig) {
        WriteString(appConfig, KEY_CONNECTION, status);
        // Don't call FullUpdate here - config editor handles updates
    }
}

bool ensureWiFiEnabled() {
    logMsg("Checking WiFi status");
    
    int netStatus = QueryNetwork();
    logMsg("Network status: 0x%X", netStatus);
    
    // Check if WiFi is ready
    if (netStatus & NET_WIFIREADY) {
        logMsg("WiFi is ready");
        
        // Check if connected
        if (netStatus & NET_CONNECTED) {
            logMsg("WiFi is connected");
            return true;
        }
        
        logMsg("WiFi is ready but not connected");
        return false;
    }
    
    // WiFi is not enabled, try to enable it
    logMsg("WiFi is not enabled, attempting to enable");
    
    int result = WiFiPower(NET_WIFI);
    logMsg("WiFiPower result: %d", result);
    
    if (result != NET_OK) {
        logMsg("Failed to enable WiFi: %d", result);
        return false;
    }
    
    // Wait for WiFi to become ready (up to 5 seconds)
    for (int i = 0; i < 50; i++) {
        usleep(100000); // 100ms
        netStatus = QueryNetwork();
        
        if (netStatus & NET_WIFIREADY) {
            logMsg("WiFi enabled successfully");
            return false; // WiFi is ready but not connected yet
        }
    }
    
    logMsg("WiFi failed to become ready within timeout");
    return false;
}

void* connectionThreadFunc(void* arg) {
    logMsg("Connection thread started");
    isConnecting = true;
    
    updateConnectionStatus("Connecting...");
    
    const char* ip = ReadString(appConfig, KEY_IP, DEFAULT_IP);
    int port = ReadInt(appConfig, KEY_PORT, atoi(DEFAULT_PORT));
    
    // Read password using ReadSecret to get the decrypted value
    const char* encryptedPassword = ReadString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
    std::string password;
    
    if (encryptedPassword && strlen(encryptedPassword) > 0) {
        // If password starts with '$', it's encrypted - use ReadSecret to decrypt
        if (encryptedPassword[0] == '$') {
            const char* decrypted = ReadSecret(appConfig, KEY_PASSWORD, "");
            if (decrypted && strlen(decrypted) > 0) {
                password = decrypted;
                logMsg("Using decrypted password (length: %d)", (int)password.length());
            } else {
                password = "";
                logMsg("Password decryption returned empty string");
            }
        } else {
            // Password is not encrypted yet, use as-is
            password = encryptedPassword;
            logMsg("Using plaintext password (length: %d)", (int)password.length());
        }
    } else {
        password = "";
        logMsg("No password configured");
    }
    
    logMsg("Connecting to %s:%d", ip, port);
    
    // Connect to server
    if (!networkManager->connectToServer(ip, port)) {
        logMsg("Connection failed");
        updateConnectionStatus("Connection failed");
        isConnecting = false;
        return NULL;
    }
    
    logMsg("Connected, starting handshake");
    updateConnectionStatus("Handshaking...");
    
    // Perform handshake
    if (!protocol->performHandshake(password)) {
        logMsg("Handshake failed: %s", protocol->getErrorMessage().c_str());
        updateConnectionStatus(protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        return NULL;
    }
    
    logMsg("Handshake successful");
    updateConnectionStatus("Connected");
    
    // Handle messages
    protocol->handleMessages([](const std::string& status) {
        logMsg("Protocol status: %s", status.c_str());
        updateConnectionStatus(status.c_str());
    });
    
    // Cleanup after disconnect
    logMsg("Disconnecting");
    protocol->disconnect();
    networkManager->disconnect();
    updateConnectionStatus("Disconnected");
    isConnecting = false;
    
    return NULL;
}

void startConnection() {
    logMsg("startConnection called, isConnecting=%d", isConnecting);
    
    if (isConnecting) {
        logMsg("Already connecting");
        return;
    }
    
    // Check and enable WiFi if needed
    if (!ensureWiFiEnabled()) {
        int netStatus = QueryNetwork();
        
        if (!(netStatus & NET_WIFIREADY)) {
            updateConnectionStatus("WiFi not available");
            Message(ICON_WARNING, "Network Error", 
                    "Failed to enable WiFi. Please check WiFi settings.", 3000);
            return;
        }
        
        if (!(netStatus & NET_CONNECTED)) {
            updateConnectionStatus("WiFi not connected");
            Message(ICON_WARNING, "Network Error", 
                    "WiFi is not connected. Please connect to a WiFi network first.", 3000);
            return;
        }
    }
    
    isConnecting = true;
    
    if (!networkManager) {
        logMsg("Creating NetworkManager");
        networkManager = new NetworkManager();
    }

    if (!bookManager) {
        logMsg("Creating BookManager");
        bookManager = new BookManager();
        // Initialize the database path
        std::string dbPath = "/mnt/ext1/system/config/calibre_books.db";
        if (!bookManager->initialize(dbPath)) {
             logMsg("Failed to initialize database");
        }
    }
    
    if (!protocol) {
        logMsg("Creating CalibreProtocol");
        
        // Читаем настройки столбцов
        const char* readCol = ReadString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
        const char* readDateCol = ReadString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
        const char* favCol = ReadString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
        
        // Создаем протокол с передачей этих настроек
        protocol = new CalibreProtocol(networkManager, bookManager, 
                                      readCol ? readCol : "", 
                                      readDateCol ? readDateCol : "", 
                                      favCol ? favCol : "");
    }
    
    // Start connection in thread using pthread
    logMsg("Creating connection thread");
    if (pthread_create(&connectionThread, NULL, connectionThreadFunc, NULL) != 0) {
        logMsg("Failed to create connection thread");
        updateConnectionStatus("Failed to start");
        isConnecting = false;
        return;
    }
    logMsg("Connection thread started");
}

void stopConnection() {
    shouldStop = true;
    
    if (isConnecting) {
        if (protocol) {
            protocol->disconnect();
        }
        if (networkManager) {
            networkManager->disconnect();
        }
        pthread_join(connectionThread, NULL);
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
            WriteString(appConfig, KEY_CONNECTION, "Disconnected");
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
    saveAndCloseConfig();
}

void configItemChangedHandler(char *name) {
    if (appConfig) {
        SaveConfig(appConfig);
    }
}

void showMainScreen() {
    ClearScreen();
    
    OpenConfigEditor(
        (char *)"Connect to Calibre",
        appConfig,
        configItems,
        configSaveHandler,
        configItemChangedHandler
    );
}

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            initLog();
            logMsg("EVT_INIT received");
            SetPanelType(PANEL_ENABLED);
            initConfig();
            showMainScreen();
            startConnection();
            break;
            
        case EVT_SHOW:
            // Don't call FullUpdate - config editor handles updates
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                logMsg("Back key pressed");
                stopConnection();
                saveAndCloseConfig();
                closeLog();
                CloseApp();
                return 1;
            }
            if (par1 == IV_KEY_HOME) {
                logMsg("Home key pressed");
                stopConnection();
                saveAndCloseConfig();
                ClearScreen();
                FullUpdate();
                closeLog();
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_PANEL:
            if (par1 == IV_KEY_HOME) {
                logMsg("Panel home pressed");
                stopConnection();
                saveAndCloseConfig();
                ClearScreen();
                FullUpdate();
                closeLog();
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            logMsg("EVT_EXIT received");
            stopConnection();
            saveAndCloseConfig();
            closeLog();
            break;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    
    // Cleanup
    if (protocol) {
        delete protocol;
        protocol = NULL;
    }
    if (networkManager) {
        delete networkManager;
        networkManager = NULL;
    }
    // <-- ИСПРАВЛЕНИЕ: Очистка памяти bookManager
    if (bookManager) {
        delete bookManager;
        bookManager = NULL;
    }
    
    return 0;
}