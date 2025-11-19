#include "inkview.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// SETTINGS MODULE
// ============================================================================

struct AppSettings {
    char ip[64];
    char port[16];
    char password[128];
    char readColumn[64];
    char readDateColumn[64];
    char favoriteColumn[64];
    char inputFolder[256];
};

static AppSettings settings = {
    "192.168.1.100",
    "8080",
    "",
    "Last Read",
    "Last Read Date",
    "Favorite",
    "/mnt/ext1/Books"
};

enum FieldIndex {
    FIELD_IP = 0,
    FIELD_PORT,
    FIELD_PASSWORD,
    FIELD_READ_COLUMN,
    FIELD_READ_DATE_COLUMN,
    FIELD_FAVORITE_COLUMN,
    FIELD_INPUT_FOLDER,
    FIELD_COUNT
};

struct FieldInfo {
    const char* label;
    char* value;
    int maxLen;
    bool isFolder;
};

static FieldInfo fieldInfo[FIELD_COUNT];
static int currentField = -1;
static imenu *currentMenu = NULL;

void initFieldInfo() {
    fieldInfo[FIELD_IP] = {"IP адрес:", settings.ip, sizeof(settings.ip), false};
    fieldInfo[FIELD_PORT] = {"Порт:", settings.port, sizeof(settings.port), false};
    fieldInfo[FIELD_PASSWORD] = {"Пароль:", settings.password, sizeof(settings.password), false};
    fieldInfo[FIELD_READ_COLUMN] = {"Read column:", settings.readColumn, sizeof(settings.readColumn), false};
    fieldInfo[FIELD_READ_DATE_COLUMN] = {"Read date column:", settings.readDateColumn, sizeof(settings.readDateColumn), false};
    fieldInfo[FIELD_FAVORITE_COLUMN] = {"Favorite column:", settings.favoriteColumn, sizeof(settings.favoriteColumn), false};
    fieldInfo[FIELD_INPUT_FOLDER] = {"Input folder:", settings.inputFolder, sizeof(settings.inputFolder), true};
}

void loadSettings() {
    FILE *f = fopen("/mnt/ext1/system/config/calibre-companion.cfg", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
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
                }
            }
        }
        fclose(f);
    }
}

void saveSettings() {
    FILE *f = fopen("/mnt/ext1/system/config/calibre-companion.cfg", "w");
    if (f) {
        fprintf(f, "ip=%s\n", settings.ip);
        fprintf(f, "port=%s\n", settings.port);
        fprintf(f, "password=%s\n", settings.password);
        fprintf(f, "read_column=%s\n", settings.readColumn);
        fprintf(f, "read_date_column=%s\n", settings.readDateColumn);
        fprintf(f, "favorite_column=%s\n", settings.favoriteColumn);
        fprintf(f, "input_folder=%s\n", settings.inputFolder);
        fclose(f);
        
        Message(ICON_INFORMATION, "Успешно", "Настройки сохранены", 1500);
    } else {
        Message(ICON_ERROR, "Ошибка", "Не удалось сохранить настройки", 2000);
    }
}

// Forward declarations
void showSettingsPanel();
void showMainMenu();

// Callback для ввода текста
void keyboardCallback(char *text) {
    if (text && currentField >= 0 && currentField < FIELD_COUNT) {
        strncpy(fieldInfo[currentField].value, text, fieldInfo[currentField].maxLen - 1);
        fieldInfo[currentField].value[fieldInfo[currentField].maxLen - 1] = '\0';
        showSettingsPanel();
    }
}

// Callback для выбора папки
void folderCallback(char *path) {
    if (path && path[0] != '\0') {
        strncpy(settings.inputFolder, path, sizeof(settings.inputFolder) - 1);
        settings.inputFolder[sizeof(settings.inputFolder) - 1] = '\0';
        showSettingsPanel();
    }
}

// ============================================================================
// SETTINGS PANEL
// ============================================================================

void settingsMenuHandler(int index) {
    // Menu closes automatically when handler is called
    
    if (currentMenu) {
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (currentMenu[i].text) {
                free((void*)currentMenu[i].text);
            }
        }
        free(currentMenu);
        currentMenu = NULL;
    }
    
    if (index == -1) {
        // Cancel
        showMainMenu();
        return;
    }
    
    if (index == 1000) {
        // Save
        saveSettings();
        showMainMenu();
        return;
    }
    
    // Edit field
    if (index >= 0 && index < FIELD_COUNT) {
        currentField = index;
        
        if (fieldInfo[index].isFolder) {
            char buffer[256];
            strncpy(buffer, fieldInfo[index].value, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            OpenDirectorySelector("Выберите папку", buffer, sizeof(buffer), folderCallback);
        } else {
            char buffer[256];
            strncpy(buffer, fieldInfo[index].value, sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            
            int keyboardType = (index == FIELD_PORT) ? KBD_NUMERIC : KBD_NORMAL;
            OpenKeyboard(fieldInfo[index].label, buffer, fieldInfo[index].maxLen - 1, keyboardType, keyboardCallback);
        }
    }
}

void showSettingsPanel() {
    currentMenu = (imenu*)calloc(FIELD_COUNT + 2, sizeof(imenu));
    int menuIndex = 0;
    
    // Add settings fields
    for (int i = 0; i < FIELD_COUNT; i++) {
        char buffer[512];
        
        if (i == FIELD_PASSWORD && strlen(fieldInfo[i].value) > 0) {
            char masked[128];
            int len = strlen(fieldInfo[i].value);
            for (int j = 0; j < len && j < 127; j++) {
                masked[j] = '*';
            }
            masked[len] = '\0';
            snprintf(buffer, sizeof(buffer), "%s %s", fieldInfo[i].label, masked);
        } else {
            snprintf(buffer, sizeof(buffer), "%s %s", fieldInfo[i].label, fieldInfo[i].value);
        }
        
        currentMenu[menuIndex].type = ITEM_ACTIVE;
        currentMenu[menuIndex].text = strdup(buffer);
        currentMenu[menuIndex].index = i;
        menuIndex++;
    }
    
    // Separator
    currentMenu[menuIndex].type = ITEM_SEPARATOR;
    currentMenu[menuIndex].text = NULL;
    menuIndex++;
    
    // Save button
    currentMenu[menuIndex].type = ITEM_ACTIVE;
    currentMenu[menuIndex].text = (char*)"Сохранить";
    currentMenu[menuIndex].index = 1000;
    menuIndex++;
    
    OpenMenu(currentMenu, 0, 0, 0, settingsMenuHandler);
}

// ============================================================================
// MAIN MENU
// ============================================================================

static bool isConnected = false;

void mainMenuHandler(int index) {
    // Menu closes automatically when handler is called
    
    if (currentMenu) {
        free(currentMenu);
        currentMenu = NULL;
    }
    
    switch (index) {
        case 1:
            // Sync
            Message(ICON_INFORMATION, "Синхронизация", "Функция в разработке", 1500);
            showMainMenu();
            break;
            
        case 2:
            // Settings
            showSettingsPanel();
            break;
            
        case 3:
            // Exit
            CloseApp();
            break;
            
        default:
            // Cancel
            CloseApp();
            break;
    }
}

void showMainMenu() {
    currentMenu = (imenu*)calloc(4, sizeof(imenu));
    int menuIndex = 0;
    
    // Header
    char statusText[512];
    snprintf(statusText, sizeof(statusText), 
        "Pocketbook Companion\n\nСтатус: %s", 
        isConnected ? "Подключено" : "Не подключено");
    
    currentMenu[menuIndex].type = ITEM_HEADER;
    currentMenu[menuIndex].text = statusText;
    menuIndex++;
    
    // Sync
    currentMenu[menuIndex].type = ITEM_ACTIVE;
    currentMenu[menuIndex].text = (char*)"Синхронизация";
    currentMenu[menuIndex].index = 1;
    menuIndex++;
    
    // Settings
    currentMenu[menuIndex].type = ITEM_ACTIVE;
    currentMenu[menuIndex].text = (char*)"Настройки";
    currentMenu[menuIndex].index = 2;
    menuIndex++;
    
    // Exit
    currentMenu[menuIndex].type = ITEM_ACTIVE;
    currentMenu[menuIndex].text = (char*)"Выход";
    currentMenu[menuIndex].index = 3;
    menuIndex++;
    
    OpenMenu(currentMenu, 0, 0, 0, mainMenuHandler);
}

// ============================================================================
// MAIN APPLICATION
// ============================================================================

static int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            loadSettings();
            initFieldInfo();
            showMainMenu();
            break;
            
        case EVT_SHOW:
            showMainMenu();
            break;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(mainEventHandler);
    return 0;
}
