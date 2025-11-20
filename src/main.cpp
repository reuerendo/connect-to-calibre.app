#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
static const char *STATUS_CHECKING = "Checking…";

// Default values
static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "9090";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "#read";
static const char *DEFAULT_READ_DATE_COLUMN = "#read_date";
static const char *DEFAULT_FAVORITE_COLUMN = "#favorite";

// Config editor structure
static iconfigedit configItems[] = {
	{
        CFG_INFO,
        NULL,
        (char*)"Connection",
        NULL,
        (char*)KEY_CONNECTION,// name
        (char*)STATUS_CHECKING,
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
            WriteInt(appConfig, KEY_CONNECTION, 0);
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
            break;
            
        case EVT_SHOW:
            FullUpdate();
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                saveAndCloseConfig();
                CloseApp();
                return 1;
            }
            if (par1 == IV_KEY_HOME) {
                saveAndCloseConfig();
                ClearScreen();  // Clear screen before closing
                FullUpdate();   // Force update to show cleared screen
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_PANEL:
            if (par1 == IV_KEY_HOME) {
                saveAndCloseConfig();
                ClearScreen();  // Clear screen before closing
                FullUpdate();   // Force update to show cleared screen
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            saveAndCloseConfig();
            break;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    return 0;
}
