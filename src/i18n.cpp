#include "i18n.h"
#include "inkview.h"
#include <string.h>

// Translation table: [language][string_id]
static const char* translations[][STR_COUNT] = {
    // English
    {
        "Connect to Calibre",              // STR_APP_TITLE
        "     IP Address",                 // STR_IP_ADDRESS
        "     Port",                       // STR_PORT
        "     Password",                   // STR_PASSWORD
        "     Read Status Column",         // STR_READ_COLUMN
        "     Read Date Column",           // STR_READ_DATE_COLUMN
        "     Favorite Column",            // STR_FAVORITE_COLUMN
        "Connection Failed",               // STR_CONNECTION_FAILED
        "Connected",                       // STR_CONNECTED
        "Disconnected",                    // STR_DISCONNECTED
        "Sync Complete",                   // STR_SYNC_COMPLETE
        "Batch sync finished",             // STR_BATCH_SYNC_FINISHED
        "book",                            // STR_BOOK_SINGULAR
        "books",                           // STR_BOOKS_PLURAL
        "Receiving...",                    // STR_RECEIVING
        "Connected (Idle)",                // STR_CONNECTED_IDLE
        "Cancel",                          // STR_CANCEL
        "Retry",                           // STR_RETRY
        "Failed to connect to Calibre server", // STR_FAILED_CONNECT_SERVER
        "Please check IP address and port",    // STR_CHECK_IP_PORT
        "Handshake failed",                // STR_HANDSHAKE_FAILED
        "Could not connect to WiFi network",   // STR_WIFI_CONNECT_FAILED
        "Total received"                   // STR_TOTAL_RECEIVED
    },
    // Russian
    {
        "Подключение к Calibre",           // STR_APP_TITLE
        "     IP-адрес",                   // STR_IP_ADDRESS
        "     Порт",                       // STR_PORT
        "     Пароль",                     // STR_PASSWORD
        "     Столбец статуса чтения",     // STR_READ_COLUMN
        "     Столбец даты прочтения",        // STR_READ_DATE_COLUMN
        "     Столбец избранного",         // STR_FAVORITE_COLUMN
        "Ошибка подключения",              // STR_CONNECTION_FAILED
        "Подключено",                      // STR_CONNECTED
        "Отключено",                       // STR_DISCONNECTED
        "Синхронизация завершена",         // STR_SYNC_COMPLETE
        "Передача файлов синхронизация завершена", // STR_BATCH_SYNC_FINISHED
        "книга",                           // STR_BOOK_SINGULAR
        "книг",                            // STR_BOOKS_PLURAL
        "Получение...",                    // STR_RECEIVING
        "Подключено (ожидание)",           // STR_CONNECTED_IDLE
        "Отмена",                          // STR_CANCEL
        "Повтор",                          // STR_RETRY
        "Не удалось подключиться к Calibre", // STR_FAILED_CONNECT_SERVER
        "Проверьте IP-адрес и порт",       // STR_CHECK_IP_PORT
        "Ошибка рукопожатия",              // STR_HANDSHAKE_FAILED
        "Не удалось подключиться к WiFi сети", // STR_WIFI_CONNECT_FAILED
        "Всего получено"                   // STR_TOTAL_RECEIVED
    },
    // Ukrainian
    {
        "Підключення до Calibre",          // STR_APP_TITLE
        "     IP-адреса",                  // STR_IP_ADDRESS
        "     Порт",                       // STR_PORT
        "     Пароль",                     // STR_PASSWORD
        "     Стовпчик статусу читання",    // STR_READ_COLUMN
        "     Стовпчик дати читання",       // STR_READ_DATE_COLUMN
        "     Стовпчик улюбленого",         // STR_FAVORITE_COLUMN
        "Помилка підключення",             // STR_CONNECTION_FAILED
        "Підключено",                      // STR_CONNECTED
        "Відключено",                      // STR_DISCONNECTED
        "Синхронізація завершена",         // STR_SYNC_COMPLETE
        "Передача файлів завершена", // STR_BATCH_SYNC_FINISHED
        "книга",                           // STR_BOOK_SINGULAR
        "книг",                            // STR_BOOKS_PLURAL
        "Отримання...",                    // STR_RECEIVING
        "Підключено (очікування)",         // STR_CONNECTED_IDLE
        "Скасувати",                       // STR_CANCEL
        "Повтор",                          // STR_RETRY
        "Не вдалося підключитися до Calibre", // STR_FAILED_CONNECT_SERVER
        "Перевірте IP-адресу та порт",     // STR_CHECK_IP_PORT
        "Помилка рукостискання",           // STR_HANDSHAKE_FAILED
        "Не вдалося підключитися до WiFi мережі", // STR_WIFI_CONNECT_FAILED
        "Всього отримано"                  // STR_TOTAL_RECEIVED
    },
    // Spanish
    {
        "Conectar a Calibre",              // STR_APP_TITLE
        "     Dirección IP",               // STR_IP_ADDRESS
        "     Puerto",                     // STR_PORT
        "     Contraseña",                 // STR_PASSWORD
        "     Columna de estado de lectura", // STR_READ_COLUMN
        "     Columna de fecha de lectura",  // STR_READ_DATE_COLUMN
        "     Columna de favoritos",       // STR_FAVORITE_COLUMN
        "Error de conexión",               // STR_CONNECTION_FAILED
        "Conectado",                       // STR_CONNECTED
        "Desconectado",                    // STR_DISCONNECTED
        "Sincronización completa",         // STR_SYNC_COMPLETE
        "Sincronización por lotes finalizada", // STR_BATCH_SYNC_FINISHED
        "libro",                           // STR_BOOK_SINGULAR
        "libros",                          // STR_BOOKS_PLURAL
        "Recibiendo...",                   // STR_RECEIVING
        "Conectado (inactivo)",            // STR_CONNECTED_IDLE
        "Cancelar",                        // STR_CANCEL
        "Reintentar",                      // STR_RETRY
        "No se pudo conectar al servidor Calibre", // STR_FAILED_CONNECT_SERVER
        "Verifique la dirección IP y el puerto",   // STR_CHECK_IP_PORT
        "Error de handshake",              // STR_HANDSHAKE_FAILED
        "No se pudo conectar a la red WiFi",       // STR_WIFI_CONNECT_FAILED
        "Total recibido"                   // STR_TOTAL_RECEIVED
    }
};

static LanguageCode currentLanguage = LANG_ENGLISH;

// Map Pocketbook language codes to our enum
static LanguageCode mapPocketbookLanguage(int pbLang) {
    switch (pbLang) {
        case 2:  // Russian
            return LANG_RUSSIAN;
        case 27: // Ukrainian
            return LANG_UKRAINIAN;
        case 7:  // Spanish
            return LANG_SPANISH;
        default:
            return LANG_ENGLISH;
    }
}

void i18n_init() {
    // Get system language from Pocketbook
    int systemLang = GetLang();
    currentLanguage = mapPocketbookLanguage(systemLang);
}

const char* i18n_get(StringId id) {
    if (id >= STR_COUNT) {
        return "";
    }
    return translations[currentLanguage][id];
}

void i18n_set_language(LanguageCode lang) {
    if (lang < LANG_ENGLISH || lang > LANG_SPANISH) {
        return;
    }
    currentLanguage = lang;
}

LanguageCode i18n_get_language() {
    return currentLanguage;
}
