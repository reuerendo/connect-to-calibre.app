#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// APPLICATION STATE
// ============================================================================

struct AppSettings {
    char ip[64];
    char port[16];
    char password[128];
    char readColumn[64];
    char readDateColumn[64];
    char favoriteColumn[64];
    char inputFolder[256];
    bool connectionEnabled;
};

static AppSettings settings = {
    "192.168.1.100",
    "8080",
    "",
    "Last Read",
    "Last Read Date",
    "Favorite",
    "/mnt/ext1/Books",
    false
};

// Menu items
static imenu mainMenu;
static imenu *currentSubmenu = NULL;

// ============================================================================
// SETTINGS MANAGEMENT
// ============================================================================

void loadSettings() {
    FILE *f = iv_fopen("/mnt/ext1/system/config/calibre-companion.cfg", "r");
    if (f) {
        char line[512];
        while (iv_fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = line;
                char *value = eq + 1;
                
                char *nl = strchr(value, '\n');
                if (nl) *nl = '\0';
                
                if (strcmp(key, "ip") == 0) {
                    strncpy(settings.ip, value, sizeof(settings.ip) - 1);
                } else if (strcmp(key, "port") == 0) {
                    strncpy(settings.port, value, sizeof(settings.port) - 1);
                } else if (strcmp(key, "password") == 0) {
                    strncpy(settings.password, value, sizeof(settings.password) - 1);
                } else if (strcmp(key, "read_column") == 0) {
                    strncpy(settings.readColumn, value, sizeof(settings.readColumn) - 1);
                } else if (strcmp(key, "read_date_column") == 0) {
                    strncpy(settings.readDateColumn, value, sizeof(settings.readDateColumn) - 1);
                } else if (strcmp(key, "favorite_column") == 0) {
                    strncpy(settings.favoriteColumn, value, sizeof(settings.favoriteColumn) - 1);
                } else if (strcmp(key, "input_folder") == 0) {
                    strncpy(settings.inputFolder, value, sizeof(settings.inputFolder) - 1);
                } else if (strcmp(key, "connection_enabled") == 0) {
                    settings.connectionEnabled = (strcmp(value, "1") == 0);
                }
            }
        }
        iv_fclose(f);
    }
}

void saveSettings() {
    iv_buildpath("/mnt/ext1/system/config");
    FILE *f = iv_fopen("/mnt/ext1/system/config/calibre-companion.cfg", "w");
    if (f) {
        fprintf(f, "ip=%s\n", settings.ip);
        fprintf(f, "port=%s\n", settings.port);
        fprintf(f, "password=%s\n", settings.password);
        fprintf(f, "read_column=%s\n", settings.readColumn);
        fprintf(f, "read_date_column=%s\n", settings.readDateColumn);
        fprintf(f, "favorite_column=%s\n", settings.favoriteColumn);
        fprintf(f, "input_folder=%s\n", settings.inputFolder);
        fprintf(f, "connection_enabled=%d\n", settings.connectionEnabled ? 1 : 0);
        iv_fclose(f);
        iv_sync();
    }
}

// ============================================================================
// MENU CALLBACKS
// ============================================================================

void toggleConnectionCallback(int index) {
    settings.connectionEnabled = !settings.connectionEnabled;
    saveSettings();
    
    // Update menu item text
    if (mainMenu.submenu && mainMenu.submenu[0].text) {
        free(mainMenu.submenu[0].text);
        mainMenu.submenu[0].text = strdup(settings.connectionEnabled ? 
            "Connection: Enabled" : "Connection: Disabled");
    }
    
    // Show notification
    Message(ICON_INFORMATION, "Connection", 
            settings.connectionEnabled ? "Connection enabled" : "Connection disabled", 
            2000);
}

void ipCallback(char *text) {
    if (text) {
        strncpy(settings.ip, text, sizeof(settings.ip) - 1);
        settings.ip[sizeof(settings.ip) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "IP Address", "IP address updated", 2000);
    }
}

void portCallback(char *text) {
    if (text) {
        strncpy(settings.port, text, sizeof(settings.port) - 1);
        settings.port[sizeof(settings.port) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Port", "Port updated", 2000);
    }
}

void passwordCallback(char *text) {
    if (text) {
        strncpy(settings.password, text, sizeof(settings.password) - 1);
        settings.password[sizeof(settings.password) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Password", "Password updated", 2000);
    }
}

void readColumnCallback(char *text) {
    if (text) {
        strncpy(settings.readColumn, text, sizeof(settings.readColumn) - 1);
        settings.readColumn[sizeof(settings.readColumn) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Read Column", "Column name updated", 2000);
    }
}

void readDateColumnCallback(char *text) {
    if (text) {
        strncpy(settings.readDateColumn, text, sizeof(settings.readDateColumn) - 1);
        settings.readDateColumn[sizeof(settings.readDateColumn) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Read Date Column", "Column name updated", 2000);
    }
}

void favoriteColumnCallback(char *text) {
    if (text) {
        strncpy(settings.favoriteColumn, text, sizeof(settings.favoriteColumn) - 1);
        settings.favoriteColumn[sizeof(settings.favoriteColumn) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Favorite Column", "Column name updated", 2000);
    }
}

void folderCallback(char *path) {
    if (path && path[0] != '\0') {
        strncpy(settings.inputFolder, path, sizeof(settings.inputFolder) - 1);
        settings.inputFolder[sizeof(settings.inputFolder) - 1] = '\0';
        saveSettings();
        Message(ICON_INFORMATION, "Input Folder", "Folder path updated", 2000);
    }
}

void editIPHandler(int index) {
    char buffer[64];
    strncpy(buffer, settings.ip, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("IP Address", buffer, sizeof(buffer) - 1, KBD_NORMAL, ipCallback);
}

void editPortHandler(int index) {
    char buffer[16];
    strncpy(buffer, settings.port, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("Port", buffer, sizeof(buffer) - 1, KBD_NUMERIC, portCallback);
}

void editPasswordHandler(int index) {
    char buffer[128];
    strncpy(buffer, settings.password, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("Password", buffer, sizeof(buffer) - 1, KBD_PASSWORD, passwordCallback);
}

void editReadColumnHandler(int index) {
    char buffer[64];
    strncpy(buffer, settings.readColumn, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("Read Status Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, readColumnCallback);
}

void editReadDateColumnHandler(int index) {
    char buffer[64];
    strncpy(buffer, settings.readDateColumn, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("Read Date Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, readDateColumnCallback);
}

void editFavoriteColumnHandler(int index) {
    char buffer[64];
    strncpy(buffer, settings.favoriteColumn, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenKeyboard("Favorite Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, favoriteColumnCallback);
}

void editInputFolderHandler(int index) {
    char buffer[256];
    strncpy(buffer, settings.inputFolder, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    OpenDirectorySelector("Input Folder", buffer, sizeof(buffer), folderCallback);
}

// ============================================================================
// MENU CREATION
// ============================================================================

void createMainMenu() {
    // Create menu structure using standard PocketBook menu system
    mainMenu.type = 1; // Menu list type
    mainMenu.n = 8; // Number of items
    mainMenu.submenu = (imenu *)calloc(9, sizeof(imenu)); // +1 for terminator
    
    // Item 0: Connection toggle
    mainMenu.submenu[0].type = 0; // Regular menu item
    mainMenu.submenu[0].text = strdup(settings.connectionEnabled ? 
        "Connection: Enabled" : "Connection: Disabled");
    mainMenu.submenu[0].index = 0;
    mainMenu.submenu[0].handler = toggleConnectionCallback;
    
    // Item 1: IP Address
    mainMenu.submenu[1].type = 0;
    mainMenu.submenu[1].text = (char *)malloc(128);
    snprintf(mainMenu.submenu[1].text, 128, "IP Address: %s", settings.ip);
    mainMenu.submenu[1].index = 1;
    mainMenu.submenu[1].handler = editIPHandler;
    
    // Item 2: Port
    mainMenu.submenu[2].type = 0;
    mainMenu.submenu[2].text = (char *)malloc(64);
    snprintf(mainMenu.submenu[2].text, 64, "Port: %s", settings.port);
    mainMenu.submenu[2].index = 2;
    mainMenu.submenu[2].handler = editPortHandler;
    
    // Item 3: Password
    mainMenu.submenu[3].type = 0;
    mainMenu.submenu[3].text = (char *)malloc(128);
    if (strlen(settings.password) > 0) {
        snprintf(mainMenu.submenu[3].text, 128, "Password: ••••••••");
    } else {
        snprintf(mainMenu.submenu[3].text, 128, "Password: Not set");
    }
    mainMenu.submenu[3].index = 3;
    mainMenu.submenu[3].handler = editPasswordHandler;
    
    // Item 4: Read Column
    mainMenu.submenu[4].type = 0;
    mainMenu.submenu[4].text = (char *)malloc(128);
    snprintf(mainMenu.submenu[4].text, 128, "Read Status Column: %s", settings.readColumn);
    mainMenu.submenu[4].index = 4;
    mainMenu.submenu[4].handler = editReadColumnHandler;
    
    // Item 5: Read Date Column
    mainMenu.submenu[5].type = 0;
    mainMenu.submenu[5].text = (char *)malloc(128);
    snprintf(mainMenu.submenu[5].text, 128, "Read Date Column: %s", settings.readDateColumn);
    mainMenu.submenu[5].index = 5;
    mainMenu.submenu[5].handler = editReadDateColumnHandler;
    
    // Item 6: Favorite Column
    mainMenu.submenu[6].type = 0;
    mainMenu.submenu[6].text = (char *)malloc(128);
    snprintf(mainMenu.submenu[6].text, 128, "Favorite Column: %s", settings.favoriteColumn);
    mainMenu.submenu[6].index = 6;
    mainMenu.submenu[6].handler = editFavoriteColumnHandler;
    
    // Item 7: Input Folder
    mainMenu.submenu[7].type = 0;
    mainMenu.submenu[7].text = (char *)malloc(300);
    snprintf(mainMenu.submenu[7].text, 300, "Input Folder: %s", settings.inputFolder);
    mainMenu.submenu[7].index = 7;
    mainMenu.submenu[7].handler = editInputFolderHandler;
    
    // Terminator
    mainMenu.submenu[8].type = 0;
    mainMenu.submenu[8].text = NULL;
}

void freeMainMenu() {
    if (mainMenu.submenu) {
        for (int i = 0; i < mainMenu.n; i++) {
            if (mainMenu.submenu[i].text) {
                free(mainMenu.submenu[i].text);
            }
        }
        free(mainMenu.submenu);
        mainMenu.submenu = NULL;
    }
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            loadSettings();
            createMainMenu();
            
            // Open menu with title
            OpenMenu(&mainMenu, 0, 0, 0, "CALIBRE COMPANION", NULL);
            break;
            
        case EVT_SHOW:
            // Reopen menu if needed
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            freeMainMenu();
            break;
    }
    
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    return 0;
}
