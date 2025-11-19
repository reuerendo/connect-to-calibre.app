#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Application state
static bool isConnected = false;
static char statusText[512] = "Ожидание подключения к Calibre...\n\nПриложение готово к работе.";

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
    const char* buttons[] = { "Выход", "Настройки", "Обновить" };
    
    char menuText[1024];
    snprintf(menuText, sizeof(menuText),
        "Pocketbook Companion\n\n"
        "Статус: %s\n\n"
        "%s",
        isConnected ? "Подключено" : "Не подключено",
        statusText
    );
    
    int result = showDialog(PB_ICON_INFO, menuText, buttons, 3);
    
    switch (result) {
        case 1:  // Выход
            CloseApp();
            break;
            
        case 2:  // Настройки
            showSettingsMenu();
            break;
            
        case 3:  // Обновить
            // Здесь логика обновления статуса
            strncpy(statusText, "Проверка подключения...", sizeof(statusText) - 1);
            showMainMenu();
            break;
    }
}

// Show settings menu
void showSettingsMenu() {
    const char* buttons[] = { "Назад", "IP адрес", "Порт" };
    
    int result = showDialog(
        PB_ICON_NONE,
        "Настройки подключения\n\n"
        "IP адрес: 192.168.1.100\n"
        "Порт: 8080\n\n"
        "Выберите параметр для изменения:",
        buttons,
        3
    );
    
    if (result == 1) {
        showMainMenu();
    } else if (result == 2) {
        // Редактирование IP
        showMainMenu();
    } else if (result == 3) {
        // Редактирование порта
        showMainMenu();
    }
}

// Show initial confirmation
void showInitialDialog() {
    const char* buttons[] = { "Отмена", "Продолжить" };
    
    int result = showDialog(
        PB_ICON_QUESTION,
        "Pocketbook Companion\n\n"
        "Это приложение позволяет синхронизировать\n"
        "вашу библиотеку с Calibre.\n\n"
        "Продолжить?",
        buttons,
        2
    );
    
    if (result == 1) {
        // Отмена
        CloseApp();
    } else {
        // Продолжить - показать главное меню
        showMainMenu();
    }
}

// Application entry point
int main(int argc, char *argv[]) {
    OpenScreen();
    
    // Show initial dialog
    showInitialDialog();
    
    return 0;
}
