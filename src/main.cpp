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

// UI state
static ifont *mainFont = NULL;
static ifont *headerFont = NULL;
static ifont *labelFont = NULL;
static int screenWidth = 0;
static int screenHeight = 0;
static int currentEditField = -1;
static const int HEADER_HEIGHT = 80;
static const int PADDING = 20;
static const int FIELD_HEIGHT = 60;
static const int LABEL_WIDTH = 300;

enum FieldIndex {
    FIELD_CONNECTION_TOGGLE = 0,
    FIELD_IP,
    FIELD_PORT,
    FIELD_PASSWORD,
    FIELD_READ_COLUMN,
    FIELD_READ_DATE_COLUMN,
    FIELD_FAVORITE_COLUMN,
    FIELD_INPUT_FOLDER,
    FIELD_COUNT
};

struct UIField {
    const char* label;
    char* value;
    int maxLen;
    bool isFolder;
    bool isToggle;
    int y;
};

static UIField uiFields[FIELD_COUNT];

// ============================================================================
// SETTINGS MANAGEMENT
// ============================================================================

void initUIFields() {
    uiFields[FIELD_CONNECTION_TOGGLE] = {"Connection:", NULL, 0, false, true, 0};
    uiFields[FIELD_IP] = {"IP Address:", settings.ip, sizeof(settings.ip), false, false, 0};
    uiFields[FIELD_PORT] = {"Port:", settings.port, sizeof(settings.port), false, false, 0};
    uiFields[FIELD_PASSWORD] = {"Password:", settings.password, sizeof(settings.password), false, false, 0};
    uiFields[FIELD_READ_COLUMN] = {"Read status column:", settings.readColumn, sizeof(settings.readColumn), false, false, 0};
    uiFields[FIELD_READ_DATE_COLUMN] = {"Read date column:", settings.readDateColumn, sizeof(settings.readDateColumn), false, false, 0};
    uiFields[FIELD_FAVORITE_COLUMN] = {"Favorite column:", settings.favoriteColumn, sizeof(settings.favoriteColumn), false, false, 0};
    uiFields[FIELD_INPUT_FOLDER] = {"Input folder:", settings.inputFolder, sizeof(settings.inputFolder), true, false, 0};
    
    // Calculate Y positions
    int y = HEADER_HEIGHT + PADDING * 2;
    for (int i = 0; i < FIELD_COUNT; i++) {
        uiFields[i].y = y;
        y += FIELD_HEIGHT + PADDING;
    }
}

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
// UI DRAWING
// ============================================================================

void drawHeader() {
    // Draw header background
    FillArea(0, 0, screenWidth, HEADER_HEIGHT, WHITE);
    DrawLine(0, HEADER_HEIGHT - 1, screenWidth, HEADER_HEIGHT - 1, BLACK);
    
    // Draw title
    SetFont(headerFont, BLACK);
    int titleWidth = StringWidth("Pocketbook Companion");
    DrawString((screenWidth - titleWidth) / 2, PADDING, "Pocketbook Companion");
}

void drawToggleSwitch(int x, int y, bool enabled) {
    const int toggleWidth = 80;
    const int toggleHeight = 40;
    const int toggleRadius = 20;
    
    // Draw background
    int bgColor = enabled ? DGRAY : LGRAY;
    FillArea(x, y, toggleWidth, toggleHeight, bgColor);
    DrawRect(x, y, x + toggleWidth, y + toggleHeight, BLACK);
    
    // Draw circle
    int circleX = enabled ? x + toggleWidth - toggleRadius - 5 : x + toggleRadius + 5;
    int circleY = y + toggleHeight / 2;
    FillArea(circleX - toggleRadius, circleY - toggleRadius, 
             toggleRadius * 2, toggleRadius * 2, WHITE);
    DrawRect(circleX - toggleRadius, circleY - toggleRadius,
             circleX + toggleRadius, circleY + toggleRadius, BLACK);
    
    // Draw status text
    SetFont(labelFont, BLACK);
    const char* statusText = enabled ? "ON" : "OFF";
    DrawString(x + toggleWidth + 20, y + 10, statusText);
}

void drawField(int index) {
    UIField *field = &uiFields[index];
    int y = field->y;
    
    // Draw label
    SetFont(labelFont, BLACK);
    DrawString(PADDING, y, field->label);
    
    if (field->isToggle) {
        // Draw toggle switch
        drawToggleSwitch(PADDING + LABEL_WIDTH, y, settings.connectionEnabled);
    } else {
        // Draw value box
        int boxX = PADDING + LABEL_WIDTH;
        int boxY = y - 5;
        int boxWidth = screenWidth - boxX - PADDING;
        int boxHeight = FIELD_HEIGHT - 10;
        
        // Draw box
        FillArea(boxX, boxY, boxWidth, boxHeight, WHITE);
        DrawRect(boxX, boxY, boxX + boxWidth, boxY + boxHeight, BLACK);
        
        // Draw value text
        SetFont(mainFont, BLACK);
        if (field->value && field->value[0] != '\0') {
            char displayValue[256];
            if (index == FIELD_PASSWORD && strlen(field->value) > 0) {
                // Mask password
                int len = strlen(field->value);
                for (int j = 0; j < len && j < 127; j++) {
                    displayValue[j] = '*';
                }
                displayValue[len] = '\0';
            } else {
                strncpy(displayValue, field->value, sizeof(displayValue) - 1);
                displayValue[sizeof(displayValue) - 1] = '\0';
            }
            
            // Truncate if too long
            int maxWidth = boxWidth - 20;
            while (StringWidth(displayValue) > maxWidth && strlen(displayValue) > 0) {
                displayValue[strlen(displayValue) - 1] = '\0';
            }
            
            DrawString(boxX + 10, boxY + 15, displayValue);
        }
    }
}

void drawUI() {
    ClearScreen();
    
    drawHeader();
    
    // Draw all fields
    for (int i = 0; i < FIELD_COUNT; i++) {
        drawField(i);
    }
    
    FullUpdate();
}

// ============================================================================
// INPUT CALLBACKS
// ============================================================================

void keyboardCallback(char *text) {
    if (text && currentEditField >= 0 && currentEditField < FIELD_COUNT) {
        UIField *field = &uiFields[currentEditField];
        if (field->value) {
            strncpy(field->value, text, field->maxLen - 1);
            field->value[field->maxLen - 1] = '\0';
            saveSettings();
        }
    }
    currentEditField = -1;
    drawUI();
}

void folderCallback(char *path) {
    if (path && path[0] != '\0' && currentEditField == FIELD_INPUT_FOLDER) {
        strncpy(settings.inputFolder, path, sizeof(settings.inputFolder) - 1);
        settings.inputFolder[sizeof(settings.inputFolder) - 1] = '\0';
        saveSettings();
    }
    currentEditField = -1;
    drawUI();
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

int isInsideField(int x, int y, int fieldIndex) {
    UIField *field = &uiFields[fieldIndex];
    int fieldY = field->y;
    
    if (field->isToggle) {
        // Check toggle switch area
        int toggleX = PADDING + LABEL_WIDTH;
        int toggleY = fieldY;
        int toggleWidth = 80;
        int toggleHeight = 40;
        
        return (x >= toggleX && x <= toggleX + toggleWidth &&
                y >= toggleY && y <= toggleY + toggleHeight);
    } else {
        // Check value box area
        int boxX = PADDING + LABEL_WIDTH;
        int boxY = fieldY - 5;
        int boxWidth = screenWidth - boxX - PADDING;
        int boxHeight = FIELD_HEIGHT - 10;
        
        return (x >= boxX && x <= boxX + boxWidth &&
                y >= boxY && y <= boxY + boxHeight);
    }
}

void handleFieldClick(int fieldIndex) {
    UIField *field = &uiFields[fieldIndex];
    
    if (field->isToggle) {
        // Toggle connection
        settings.connectionEnabled = !settings.connectionEnabled;
        saveSettings();
        drawUI();
        
        // Show status message
        if (settings.connectionEnabled) {
            Message(ICON_INFORMATION, "Connection", "Connection enabled", 1500);
        } else {
            Message(ICON_INFORMATION, "Connection", "Connection disabled", 1500);
        }
    } else if (field->isFolder) {
        // Open folder selector
        currentEditField = fieldIndex;
        char buffer[256];
        strncpy(buffer, field->value, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        OpenDirectorySelector("Select folder", buffer, sizeof(buffer), folderCallback);
    } else {
        // Open keyboard
        currentEditField = fieldIndex;
        char buffer[256];
        strncpy(buffer, field->value, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        
        int keyboardType = (fieldIndex == FIELD_PORT) ? KBD_NUMERIC : KBD_NORMAL;
        OpenKeyboard(field->label, buffer, field->maxLen - 1, keyboardType, keyboardCallback);
    }
}

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            screenWidth = ScreenWidth();
            screenHeight = ScreenHeight();
            
            headerFont = OpenFont("LiberationSans-Bold", 32, 1);
            mainFont = OpenFont("LiberationSans", 24, 1);
            labelFont = OpenFont("LiberationSans", 22, 1);
            
            loadSettings();
            initUIFields();
            drawUI();
            break;
            
        case EVT_SHOW:
            drawUI();
            break;
            
        case EVT_POINTERUP:
            // Check if click is inside any field
            for (int i = 0; i < FIELD_COUNT; i++) {
                if (isInsideField(par1, par2, i)) {
                    handleFieldClick(i);
                    return 1;
                }
            }
            break;
            
        case EVT_KEYPRESS:
            if (par1 == KEY_BACK || par1 == KEY_HOME) {
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            if (headerFont) CloseFont(headerFont);
            if (mainFont) CloseFont(mainFont);
            if (labelFont) CloseFont(labelFont);
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
