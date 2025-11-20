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
static imenu menuItems[9];

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
// INPUT CALLBACKS
// ============================================================================

void ipCallback(char *text) {
    if (text) {
        strncpy(settings.ip, text, sizeof(settings.ip) - 1);
        settings.ip[sizeof(settings.ip) - 1] = '\0';
        saveSettings();
    }
}

void portCallback(char *text) {
    if (text) {
        strncpy(settings.port, text, sizeof(settings.port) - 1);
        settings.port[sizeof(settings.port) - 1] = '\0';
        saveSettings();
    }
}

void passwordCallback(char *text) {
    if (text) {
        strncpy(settings.password, text, sizeof(settings.password) - 1);
        settings.password[sizeof(settings.password) - 1] = '\0';
        saveSettings();
    }
}

void readColumnCallback(char *text) {
    if (text) {
        strncpy(settings.readColumn, text, sizeof(settings.readColumn) - 1);
        settings.readColumn[sizeof(settings.readColumn) - 1] = '\0';
        saveSettings();
    }
}

void readDateColumnCallback(char *text) {
    if (text) {
        strncpy(settings.readDateColumn, text, sizeof(settings.readDateColumn) - 1);
        settings.readDateColumn[sizeof(settings.readDateColumn) - 1] = '\0';
        saveSettings();
    }
}

void favoriteColumnCallback(char *text) {
    if (text) {
        strncpy(settings.favoriteColumn, text, sizeof(settings.favoriteColumn) - 1);
        settings.favoriteColumn[sizeof(settings.favoriteColumn) - 1] = '\0';
        saveSettings();
    }
}

void folderCallback(char *path) {
    if (path && path[0] != '\0') {
        strncpy(settings.inputFolder, path, sizeof(settings.inputFolder) - 1);
        settings.inputFolder[sizeof(settings.inputFolder) - 1] = '\0';
        saveSettings();
    }
}

// ============================================================================
// MENU HANDLER
// ============================================================================

void menuHandler(int index) {
    char buffer[256];
    
    switch (index) {
        case 0: // Connection toggle
            settings.connectionEnabled = !settings.connectionEnabled;
            saveSettings();
            Message(ICON_INFORMATION, "Connection", 
                    settings.connectionEnabled ? "Connection enabled" : "Connection disabled", 
                    2000);
            break;
            
        case 1: // IP Address
            strncpy(buffer, settings.ip, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("IP Address", buffer, sizeof(buffer) - 1, KBD_NORMAL, ipCallback);
            break;
            
        case 2: // Port
            strncpy(buffer, settings.port, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("Port", buffer, sizeof(buffer) - 1, KBD_NUMERIC, portCallback);
            break;
            
        case 3: // Password
            strncpy(buffer, settings.password, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("Password", buffer, sizeof(buffer) - 1, KBD_PASSWORD, passwordCallback);
            break;
            
        case 4: // Read Column
            strncpy(buffer, settings.readColumn, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("Read Status Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, readColumnCallback);
            break;
            
        case 5: // Read Date Column
            strncpy(buffer, settings.readDateColumn, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("Read Date Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, readDateColumnCallback);
            break;
            
        case 6: // Favorite Column
            strncpy(buffer, settings.favoriteColumn, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenKeyboard("Favorite Column", buffer, sizeof(buffer) - 1, KBD_NORMAL, favoriteColumnCallback);
            break;
            
        case 7: // Input Folder
            strncpy(buffer, settings.inputFolder, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenDirectorySelector("Input Folder", buffer, sizeof(buffer), folderCallback);
            break;
    }
}

// ============================================================================
// MENU CREATION
// ============================================================================

void updateMenuItems() {
    // Free old text if any
    for (int i = 0; i < 8; i++) {
        if (menuItems[i].text) {
            free(menuItems[i].text);
            menuItems[i].text = NULL;
        }
    }
    
    // Item 0: Connection toggle
    menuItems[0].type = 1; // Regular menu item
    menuItems[0].index = 0;
    menuItems[0].text = (char *)malloc(128);
    snprintf(menuItems[0].text, 128, "Connection: %s", 
             settings.connectionEnabled ? "Enabled" : "Disabled");
    menuItems[0].submenu = NULL;
    
    // Item 1: IP Address
    menuItems[1].type = 1;
    menuItems[1].index = 1;
    menuItems[1].text = (char *)malloc(128);
    snprintf(menuItems[1].text, 128, "IP Address: %s", settings.ip);
    menuItems[1].submenu = NULL;
    
    // Item 2: Port
    menuItems[2].type = 1;
    menuItems[2].index = 2;
    menuItems[2].text = (char *)malloc(64);
    snprintf(menuItems[2].text, 64, "Port: %s", settings.port);
    menuItems[2].submenu = NULL;
    
    // Item 3: Password
    menuItems[3].type = 1;
    menuItems[3].index = 3;
    menuItems[3].text = (char *)malloc(128);
    if (strlen(settings.password) > 0) {
        snprintf(menuItems[3].text, 128, "Password: ••••••••");
    } else {
        snprintf(menuItems[3].text, 128, "Password: Not set");
    }
    menuItems[3].submenu = NULL;
    
    // Item 4: Read Column
    menuItems[4].type = 1;
    menuItems[4].index = 4;
    menuItems[4].text = (char *)malloc(128);
    snprintf(menuItems[4].text, 128, "Read Status Column: %s", settings.readColumn);
    menuItems[4].submenu = NULL;
    
    // Item 5: Read Date Column
    menuItems[5].type = 1;
    menuItems[5].index = 5;
    menuItems[5].text = (char *)malloc(128);
    snprintf(menuItems[5].text, 128, "Read Date Column: %s", settings.readDateColumn);
    menuItems[5].submenu = NULL;
    
    // Item 6: Favorite Column
    menuItems[6].type = 1;
    menuItems[6].index = 6;
    menuItems[6].text = (char *)malloc(128);
    snprintf(menuItems[6].text, 128, "Favorite Column: %s", settings.favoriteColumn);
    menuItems[6].submenu = NULL;
    
    // Item 7: Input Folder
    menuItems[7].type = 1;
    menuItems[7].index = 7;
    menuItems[7].text = (char *)malloc(300);
    snprintf(menuItems[7].text, 300, "Input Folder: %s", settings.inputFolder);
    menuItems[7].submenu = NULL;
    
    // Terminator
    menuItems[8].type = 0;
    menuItems[8].index = 0;
    menuItems[8].text = NULL;
    menuItems[8].submenu = NULL;
}

void cleanupMenuItems() {
    for (int i = 0; i < 8; i++) {
        if (menuItems[i].text) {
            free(menuItems[i].text);
            menuItems[i].text = NULL;
        }
    }
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            loadSettings();
            updateMenuItems();
            // Open menu: array, selected index, x, y, handler
            OpenMenu(menuItems, 0, 0, 0, menuHandler);
            break;
            
        case EVT_SHOW:
            // Update menu items when returning from keyboard/directory selector
            updateMenuItems();
            OpenMenu(menuItems, 0, 0, 0, menuHandler);
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            cleanupMenuItems();
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
