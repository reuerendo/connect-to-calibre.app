#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Application state
static bool isConnected = false;
static char statusText[512] = "Ожидание подключения к Calibre...\n\nПриложение готово к работе.";

// Settings
static char settingsIP[64] = "192.168.1.100";
static char settingsPort[16] = "8080";
static char settingsPassword[64] = "";
static char settingsReadColumn[64] = "read";
static char settingsReadDateColumn[64] = "read_date";
static char settingsFavoriteColumn[64] = "favorite";
static char settingsInputFolder[256] = "/mnt/ext1/Books";

// Text buffers for menu items
static char ipText[128];
static char portText[128];
static char passwordText[128];
static char readColumnText[128];
static char readDateColumnText[128];
static char favoriteColumnText[128];
static char inputFolderText[300];

// Current selected menu index
static int currentMenuIndex = 0;

// Forward declarations
void showMainMenu();
void showSettingsMenu();
int mainHandler(int type, int par1, int par2);

// Settings menu items
static imenu settingsMenuItems[] = {
    { ITEM_HEADER, 0, (char*)"Настройки", NULL },
    { ITEM_ACTIVE, 0, ipText, NULL },
    { ITEM_ACTIVE, 1, portText, NULL },
    { ITEM_ACTIVE, 2, passwordText, NULL },
    { ITEM_ACTIVE, 3, readColumnText, NULL },
    { ITEM_ACTIVE, 4, readDateColumnText, NULL },
    { ITEM_ACTIVE, 5, favoriteColumnText, NULL },
    { ITEM_ACTIVE, 6, inputFolderText, NULL },
    { 0, 0, NULL, NULL }
};

// Keyboard handler for IP
static void ipKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsIP, text, sizeof(settingsIP) - 1);
        settingsIP[sizeof(settingsIP) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Port
static void portKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsPort, text, sizeof(settingsPort) - 1);
        settingsPort[sizeof(settingsPort) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Password
static void passwordKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsPassword, text, sizeof(settingsPassword) - 1);
        settingsPassword[sizeof(settingsPassword) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Read column
static void readColumnKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsReadColumn, text, sizeof(settingsReadColumn) - 1);
        settingsReadColumn[sizeof(settingsReadColumn) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Read date column
static void readDateColumnKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsReadDateColumn, text, sizeof(settingsReadDateColumn) - 1);
        settingsReadDateColumn[sizeof(settingsReadDateColumn) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Favorite column
static void favoriteColumnKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsFavoriteColumn, text, sizeof(settingsFavoriteColumn) - 1);
        settingsFavoriteColumn[sizeof(settingsFavoriteColumn) - 1] = '\0';
    }
    showSettingsMenu();
}

// Keyboard handler for Input folder
static void inputFolderKeyboardHandler(char* text) {
    if (text != NULL) {
        strncpy(settingsInputFolder, text, sizeof(settingsInputFolder) - 1);
        settingsInputFolder[sizeof(settingsInputFolder) - 1] = '\0';
    }
    showSettingsMenu();
}

// Settings menu item handler
static void settingsMenuHandler(int index) {
    currentMenuIndex = index;
    
    if (index == -1) {
        // Back button pressed
        return;
    }
    
    switch (index) {
        case 0: // IP адрес
            OpenKeyboard("IP адрес", settingsIP, 63, KBD_NORMAL, ipKeyboardHandler);
            break;
            
        case 1: // Порт
            OpenKeyboard("Порт", settingsPort, 15, KBD_NORMAL, portKeyboardHandler);
            break;
            
        case 2: // Пароль
            OpenKeyboard("Пароль", settingsPassword, 63, KBD_PASSWORD, passwordKeyboardHandler);
            break;
            
        case 3: // Read column
            OpenKeyboard("Read column", settingsReadColumn, 63, KBD_NORMAL, readColumnKeyboardHandler);
            break;
            
        case 4: // Read date column
            OpenKeyboard("Read date column", settingsReadDateColumn, 63, KBD_NORMAL, readDateColumnKeyboardHandler);
            break;
            
        case 5: // Favorite column
            OpenKeyboard("Favorite column", settingsFavoriteColumn, 63, KBD_NORMAL, favoriteColumnKeyboardHandler);
            break;
            
        case 6: // Input folder
            OpenKeyboard("Input folder", settingsInputFolder, 255, KBD_NORMAL, inputFolderKeyboardHandler);
            break;
    }
}

// Main menu items
static imenu mainMenuItems[] = {
    { ITEM_HEADER, 0, (char*)"Pocketbook Companion", NULL },
    { ITEM_ACTIVE, 1, (char*)"Настройки", NULL },
    { ITEM_SEPARATOR, 0, NULL, NULL },
    { ITEM_ACTIVE, 2, (char*)"Выход", NULL },
    { 0, 0, NULL, NULL }
};

// Main menu handler
static void mainMenuHandler(int index) {
    switch (index) {
        case 1:  // Настройки
            showSettingsMenu();
            break;
            
        case 2:  // Выход
            CloseApp();
            break;
    }
}

// Show settings menu
void showSettingsMenu() {
    // Update menu items with current values
    snprintf(ipText, sizeof(ipText), "IP адрес: %s", settingsIP);
    snprintf(portText, sizeof(portText), "Порт: %s", settingsPort);
    snprintf(passwordText, sizeof(passwordText), "Пароль: %s", 
             settingsPassword[0] != '\0' ? "***" : "(не установлен)");
    snprintf(readColumnText, sizeof(readColumnText), "Read column: %s", settingsReadColumn);
    snprintf(readDateColumnText, sizeof(readDateColumnText), "Read date column: %s", settingsReadDateColumn);
    snprintf(favoriteColumnText, sizeof(favoriteColumnText), "Favorite column: %s", settingsFavoriteColumn);
    snprintf(inputFolderText, sizeof(inputFolderText), "Input folder: %s", settingsInputFolder);
    
    // Show menu at center of screen
    OpenMenu(settingsMenuItems, currentMenuIndex, ScreenWidth() / 2, ScreenHeight() / 2, settingsMenuHandler);
}

// Show main menu
void showMainMenu() {
    OpenMenu(mainMenuItems, 0, ScreenWidth() / 2, ScreenHeight() / 2, mainMenuHandler);
}

// Main event handler
int mainHandler(int type, int par1, int par2) {
    if (type == EVT_INIT) {
        // Initialize application
        OpenScreen();
    }
    
    if (type == EVT_SHOW) {
        // Clear screen and show main menu
        ClearScreen();
        FullUpdate();
        showMainMenu();
    }
    
    if (type == EVT_KEYPRESS) {
        if (par1 == KEY_BACK) {
            CloseApp();
        }
    }
    
    return 0;
}

// Application entry point
int main(int argc, char *argv[]) {
    InkViewMain(mainHandler);
    return 0;
}
