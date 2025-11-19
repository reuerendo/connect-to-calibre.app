#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Forward declarations
void showMainMenu();
void showSettingsScreen();

// ============================================================================
// SETTINGS MODULE
// ============================================================================

// Settings structure
struct AppSettings {
    char ip[64];
    char port[16];
    char password[128];
    char readColumn[64];
    char readDateColumn[64];
    char favoriteColumn[64];
    char inputFolder[256];
};

// Global settings
static AppSettings settings = {
    "192.168.1.100",
    "8080",
    "",
    "Last Read",
    "Last Read Date",
    "Favorite",
    "/mnt/ext1/Books"
};

// Field indices
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

// Field structure
struct FieldRect {
    int y;
    int height;
    const char* label;
    char* value;
    int maxLen;
    bool isFolder;
};

// Current state
static int currentField = -1;
static ifont *mainFont = NULL;
static ifont *labelFont = NULL;
static FieldRect fields[FIELD_COUNT];

// Initialize fields
void initFields() {
    int startY = 80;
    int fieldHeight = 60;
    int spacing = 10;
    
    fields[FIELD_IP] = {startY, fieldHeight, "IP адрес:", settings.ip, sizeof(settings.ip), false};
    fields[FIELD_PORT] = {startY + (fieldHeight + spacing) * 1, fieldHeight, "Порт:", settings.port, sizeof(settings.port), false};
    fields[FIELD_PASSWORD] = {startY + (fieldHeight + spacing) * 2, fieldHeight, "Пароль:", settings.password, sizeof(settings.password), false};
    fields[FIELD_READ_COLUMN] = {startY + (fieldHeight + spacing) * 3, fieldHeight, "Read column:", settings.readColumn, sizeof(settings.readColumn), false};
    fields[FIELD_READ_DATE_COLUMN] = {startY + (fieldHeight + spacing) * 4, fieldHeight, "Read date column:", settings.readDateColumn, sizeof(settings.readDateColumn), false};
    fields[FIELD_FAVORITE_COLUMN] = {startY + (fieldHeight + spacing) * 5, fieldHeight, "Favorite column:", settings.favoriteColumn, sizeof(settings.favoriteColumn), false};
    fields[FIELD_INPUT_FOLDER] = {startY + (fieldHeight + spacing) * 6, fieldHeight, "Input folder:", settings.inputFolder, sizeof(settings.inputFolder), true};
}

// Load settings from file
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
                
                // Remove newline
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

// Save settings to file
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
        showMainMenu();
    } else {
        Message(ICON_ERROR, "Ошибка", "Не удалось сохранить настройки", 2000);
    }
}

// Cancel settings
void cancelSettings() {
    showMainMenu();
}

// Draw settings screen
void drawSettings() {
    ClearScreen();
    
    int screenW = ScreenWidth();
    int screenH = ScreenHeight();
    
    // Initialize fonts if needed
    if (!mainFont) {
        mainFont = OpenFont("LiberationSans", 24, 1);
        labelFont = OpenFont("LiberationSans", 20, 1);
    }
    
    // Draw title
    SetFont(mainFont, BLACK);
    DrawString(20, 30, "Настройки");
    
    // Draw fields
    SetFont(labelFont, BLACK);
    for (int i = 0; i < FIELD_COUNT; i++) {
        FieldRect &field = fields[i];
        
        // Draw label
        DrawString(20, field.y + 5, field.label);
        
        // Draw value box
        int boxY = field.y + 25;
        int boxHeight = 35;
        
        // Highlight current field
        if (i == currentField) {
            FillArea(15, boxY - 2, screenW - 30, boxHeight + 4, LGRAY);
        }
        
        // Draw border
        DrawRect(15, boxY, screenW - 30, boxHeight, BLACK);
        
        // Draw value text
        const char* displayValue = field.value;
        
        // For password field, show asterisks
        if (i == FIELD_PASSWORD && strlen(field.value) > 0) {
            static char masked[128];
            int len = strlen(field.value);
            for (int j = 0; j < len && j < 127; j++) {
                masked[j] = '*';
            }
            masked[len] = '\0';
            displayValue = masked;
        }
        
        DrawString(20, boxY + 7, displayValue);
        
        // Draw folder icon indicator for folder field
        if (field.isFolder) {
            DrawString(screenW - 80, boxY + 7, "[...]");
        }
    }
    
    // Draw buttons at bottom
    int buttonY = screenH - 70;
    int buttonWidth = (screenW - 60) / 2;
    
    // Cancel button
    DrawRect(20, buttonY, buttonWidth, 50, BLACK);
    SetFont(mainFont, BLACK);
    int cancelW = StringWidth("Отмена");
    DrawString(20 + (buttonWidth - cancelW) / 2, buttonY + 15, "Отмена");
    
    // Save button
    DrawRect(40 + buttonWidth, buttonY, buttonWidth, 50, BLACK);
    FillArea(42 + buttonWidth, buttonY + 2, buttonWidth - 4, 46, DGRAY);
    SetFont(mainFont, WHITE);
    int saveW = StringWidth("Сохранить");
    DrawString(40 + buttonWidth + (buttonWidth - saveW) / 2, buttonY + 15, "Сохранить");
    
    FullUpdate();
}

// Open keyboard for field input
void openKeyboardForField(int fieldIndex) {
    if (fieldIndex < 0 || fieldIndex >= FIELD_COUNT) return;
    
    FieldRect &field = fields[fieldIndex];
    
    // Create a simple input dialog
    char buffer[256];
    strncpy(buffer, field.value, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    // Use OpenKeyboard if available, otherwise use simple message
    // Note: OpenKeyboard API varies between SDK versions
    // This is a placeholder - actual implementation depends on SDK version
    Message(ICON_QUESTION, "Ввод", field.label, 2000);
    
    // In production, you would use OpenKeyboard here
    // The callback would update field.value and call drawSettings()
}

// Callback for directory selector
void dirSelectorHandler(char *path) {
    if (path != NULL && path[0] != '\0') {
        strncpy(settings.inputFolder, path, sizeof(settings.inputFolder) - 1);
        settings.inputFolder[sizeof(settings.inputFolder) - 1] = '\0';
        drawSettings();
    }
}

// Select folder using directory chooser
void selectFolder() {
    // Use OpenDirectorySelector with callback
    char buffer[256];
    strncpy(buffer, settings.inputFolder, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    OpenDirectorySelector("Выберите папку", buffer, sizeof(buffer), dirSelectorHandler);
}

// Handle pointer events in settings
void handleSettingsPointer(int x, int y) {
    int screenW = ScreenWidth();
    int screenH = ScreenHeight();
    
    // Check if clicked on a field
    for (int i = 0; i < FIELD_COUNT; i++) {
        int boxY = fields[i].y + 25;
        if (x >= 15 && x <= screenW - 15 && y >= boxY && y <= boxY + 35) {
            currentField = i;
            
            if (fields[i].isFolder) {
                selectFolder();
            } else {
                openKeyboardForField(i);
            }
            return;
        }
    }
    
    // Check if clicked on buttons
    int buttonY = screenH - 70;
    int buttonWidth = (screenW - 60) / 2;
    
    if (y >= buttonY && y <= buttonY + 50) {
        if (x >= 20 && x <= 20 + buttonWidth) {
            cancelSettings();
        } else if (x >= 40 + buttonWidth && x <= 40 + buttonWidth * 2) {
            saveSettings();
        }
    }
}

// Handle key events in settings
void handleSettingsKey(int key) {
    // Use IV_KEY_BACK and IV_KEY_HOME for PocketBook SDK
    if (key == IV_KEY_BACK || key == IV_KEY_HOME) {
        cancelSettings();
    }
}

// Event handler for settings screen
static int settingsEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_SHOW:
            drawSettings();
            break;
            
        case EVT_KEYDOWN:
            handleSettingsKey(par1);
            break;
            
        case EVT_POINTERUP:
            handleSettingsPointer(par1, par2);
            break;
            
        case EVT_EXIT:
            if (mainFont) {
                CloseFont(mainFont);
                mainFont = NULL;
            }
            if (labelFont) {
                CloseFont(labelFont);
                labelFont = NULL;
            }
            break;
    }
    
    return 0;
}

// ============================================================================
// MAIN MENU MODULE
// ============================================================================

// Application state
static bool isConnected = false;
static char statusText[512] = "Ожидание подключения к Calibre...\n\nПриложение готово к работе.";

// System dialog path
static const char* DIALOG_PATH = "/ebrmain/bin/dialog";

// Dialog icons
enum PBDialogIcon {
    PB_ICON_NONE = 0,
    PB_ICON_INFO = 1,
    PB_ICON_QUESTION = 2,
    PB_ICON_ATTENTION = 3,
    PB_ICON_ERROR = 4,
    PB_ICON_WLAN = 5
};

// Show system dialog
int showDialog(PBDialogIcon icon, const char* text, const char* buttons[], int buttonCount) {
    char iconStr[8];
    snprintf(iconStr, sizeof(iconStr), "%d", icon);
    
    char** args = (char**)malloc(sizeof(char*) * (buttonCount + 4));
    args[0] = (char*)DIALOG_PATH;
    args[1] = iconStr;
    args[2] = (char*)"";
    args[3] = (char*)text;
    
    for (int i = 0; i < buttonCount; i++) {
        args[4 + i] = (char*)buttons[i];
    }
    args[4 + buttonCount] = NULL;
    
    int pid = fork();
    if (pid == 0) {
        execv(DIALOG_PATH, args);
        exit(-1);
    }
    
    int status = 0;
    if (pid > 0) {
        waitpid(pid, &status, 0);
    }
    
    free(args);
    return WEXITSTATUS(status);
}

// Show main menu
void showMainMenu() {
    const char* buttons[] = { "Настройки", "Выход" };
    
    char menuText[1024];
    snprintf(menuText, sizeof(menuText),
        "Pocketbook Companion\n\n"
        "Статус: %s\n\n"
        "%s",
        isConnected ? "Подключено" : "Не подключено",
        statusText
    );
    
    int result = showDialog(PB_ICON_INFO, menuText, buttons, 2);
    
    switch (result) {
        case 1:
            showSettingsScreen();
            break;
            
        case 2:
            CloseApp();
            break;
            
        default:
            showMainMenu();
            break;
    }
}

// Show settings screen
void showSettingsScreen() {
    ClearScreen();
    SetEventHandler(settingsEventHandler);
    loadSettings();
    initFields();
    drawSettings();
}

// ============================================================================
// MAIN APPLICATION
// ============================================================================

// Main event handler
static int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            showMainMenu();
            break;
            
        case EVT_SHOW:
            showMainMenu();
            break;
            
        case EVT_EXIT:
            break;
    }
    
    return 0;
}

// Application entry point
int main(int argc, char *argv[]) {
    OpenScreen();
    SetEventHandler(mainEventHandler);
    InkViewMain(mainEventHandler);
    
    return 0;
}
