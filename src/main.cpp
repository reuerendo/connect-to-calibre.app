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

// System dialog path
static const char* DIALOG_PATH = "/ebrmain/bin/dialog";

// Dialog icons (using different names to avoid conflicts with inkview.h)
enum PBDialogIcon {
    PB_ICON_NONE = 0,
    PB_ICON_INFO = 1,
    PB_ICON_QUESTION = 2,
    PB_ICON_ATTENTION = 3,
    PB_ICON_ERROR = 4,
    PB_ICON_WLAN = 5
};

// Forward declarations
void showMainMenu();
void showSettingsMenu();

// Show system dialog
int showDialog(PBDialogIcon icon, const char* text, const char* buttons[], int buttonCount) {
    // Build command arguments
    char iconStr[8];
    snprintf(iconStr, sizeof(iconStr), "%d", icon);
    
    // Allocate arguments array
    char** args = (char**)malloc(sizeof(char*) * (buttonCount + 4));
    args[0] = (char*)DIALOG_PATH;
    args[1] = iconStr;
    args[2] = (char*)"";  // title (empty)
    args[3] = (char*)text;
    
    for (int i = 0; i < buttonCount; i++) {
        args[4 + i] = (char*)buttons[i];
    }
    args[4 + buttonCount] = NULL;
    
    // Execute dialog
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
    // Input IP
    OpenKeyboard("IP адрес", settingsIP, 63, KBD_NORMAL, NULL);
    
    // Input Port
    OpenKeyboard("Порт", settingsPort, 15, KBD_NORMAL, NULL);
    
    // Input Password
    OpenKeyboard("Пароль", settingsPassword, 63, KBD_PASSWORD, NULL);
    
    // Input Read column
    OpenKeyboard("Read column", settingsReadColumn, 63, KBD_NORMAL, NULL);
    
    // Input Read date column
    OpenKeyboard("Read date column", settingsReadDateColumn, 63, KBD_NORMAL, NULL);
    
    // Input Favorite column
    OpenKeyboard("Favorite column", settingsFavoriteColumn, 63, KBD_NORMAL, NULL);
    
    // Input folder
    OpenKeyboard("Input folder", settingsInputFolder, 255, KBD_NORMAL, NULL);
    
    // Show confirmation dialog
    const char* buttons[] = { "Сохранить" };
    
    char confirmText[1024];
    snprintf(confirmText, sizeof(confirmText),
        "Настройки:\n\n"
        "IP адрес: %s\n"
        "Порт: %s\n"
        "Пароль: %s\n"
        "Read column: %s\n"
        "Read date column: %s\n"
        "Favorite column: %s\n"
        "Input folder: %s",
        settingsIP,
        settingsPort,
        settingsPassword[0] != '\0' ? "***" : "(не установлен)",
        settingsReadColumn,
        settingsReadDateColumn,
        settingsFavoriteColumn,
        settingsInputFolder
    );
    
    showDialog(PB_ICON_INFO, confirmText, buttons, 1);
    
    // Return to main menu
    showMainMenu();
}

// Application entry point
int main(int argc, char *argv[]) {
    OpenScreen();
    
    // Show main menu directly
    showMainMenu();
    
    return 0;
}
