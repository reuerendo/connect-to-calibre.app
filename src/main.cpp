#include "inkview.h"
#include "network.h"
#include "calibre_protocol.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

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
    if (appConfig) {
        WriteString(appConfig, KEY_CONNECTION, status);
        SaveConfig(appConfig);
        // Trigger UI redraw
        FullUpdate();
    }
}

void* connectionThreadFunc(void* arg) {
    isConnecting = true;
    
    updateConnectionStatus("Connecting...");
    
    const char* ip = ReadString(appConfig, KEY_IP, DEFAULT_IP);
    int port = ReadInt(appConfig, KEY_PORT, atoi(DEFAULT_PORT));
    const char* password = ReadString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
    
    // Connect to server
    if (!networkManager->connectToServer(ip, port)) {
        updateConnectionStatus("Connection failed");
        isConnecting = false;
        return NULL;
    }
    
    updateConnectionStatus("Handshaking...");
    
    // Perform handshake
    if (!protocol->performHandshake(password)) {
        updateConnectionStatus(protocol->getErrorMessage().c_str());
        networkManager->disconnect();
        isConnecting = false;
        return NULL;
    }
    
    updateConnectionStatus("Connected");
    
    // Handle messages
    protocol->handleMessages([](const std::string& status) {
        updateConnectionStatus(status.c_str());
    });
    
    // Cleanup after disconnect
    protocol->disconnect();
    networkManager->disconnect();
    updateConnectionStatus("Disconnected");
    isConnecting = false;
    
    return NULL;
}

void startConnection() {
    if (isConnecting) {
        return;
    }
    
    if (!networkManager) {
        networkManager = new NetworkManager();
    }
    
    if (!protocol) {
        protocol = new CalibreProtocol(networkManager);
    }
    
    shouldStop = false;
    pthread_create(&connectionThread, NULL, connectionThreadFunc, NULL);
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
            SetPanelType(PANEL_ENABLED);
            initConfig();
            showMainScreen();
            startConnection();
            break;
            
        case EVT_SHOW:
            FullUpdate();
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                stopConnection();
                saveAndCloseConfig();
                CloseApp();
                return 1;
            }
            if (par1 == IV_KEY_HOME) {
                stopConnection();
                saveAndCloseConfig();
                ClearScreen();
                FullUpdate();
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_PANEL:
            if (par1 == IV_KEY_HOME) {
                stopConnection();
                saveAndCloseConfig();
                ClearScreen();
                FullUpdate();
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            stopConnection();
            saveAndCloseConfig();
            break;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    
    // Cleanup
    if (protocol) {
        delete protocol;
    }
    if (networkManager) {
        delete networkManager;
    }
    
    return 0;
}
