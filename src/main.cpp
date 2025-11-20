#include "inkview.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// GLOBALS
// ============================================================================

static iconfig *appConfig = NULL;
static const char *CONFIG_FILE = "/mnt/ext1/system/config/calibre-companion.cfg";

// Config keys
static const char *KEY_IP = "ip";
static const char *KEY_PORT = "port";
static const char *KEY_PASSWORD = "password";
static const char *KEY_READ_COLUMN = "read_column";
static const char *KEY_READ_DATE_COLUMN = "read_date_column";
static const char *KEY_FAVORITE_COLUMN = "favorite_column";
static const char *KEY_INPUT_FOLDER = "input_folder";
static const char *KEY_CONNECTION_ENABLED = "connection_enabled";

// Default values
static const char *DEFAULT_IP = "192.168.1.100";
static const char *DEFAULT_PORT = "8080";
static const char *DEFAULT_PASSWORD = "";
static const char *DEFAULT_READ_COLUMN = "Last Read";
static const char *DEFAULT_READ_DATE_COLUMN = "Last Read Date";
static const char *DEFAULT_FAVORITE_COLUMN = "Favorite";
static const char *DEFAULT_INPUT_FOLDER = "/mnt/ext1/Books";

// Connection status choices
static char *connectionChoices[] = {
    (char *)"@Disabled",
    (char *)"@Enabled",
    NULL
};

// ============================================================================
// CONFIG EDITOR STRUCTURE
// ============================================================================

static iconfigedit configItems[] = {
    // Connection section header
    {
        CFG_INFO,
        NULL,
        (char *)"@Connection",
        NULL,
        NULL,
        NULL,
        NULL
    },
    
    // Connection enabled/disabled
    {
        CFG_CHOICE,
        NULL,
        (char *)"@Status",
        NULL,
        (char *)KEY_CONNECTION_ENABLED,
        (char *)"0",
        connectionChoices,
        NULL
    },
    
    // IP Address
    {
        CFG_ENTEXT,
        NULL,
        (char *)"@IP_Address",
        (char *)"@Enter_IP_address",
        (char *)KEY_IP,
        (char *)DEFAULT_IP,
        NULL,
        NULL
    },
    
    // Port
    {
        CFG_NUMBER,
        NULL,
        (char *)"@Port",
        (char *)"@Enter_port_number",
        (char *)KEY_PORT,
        (char *)DEFAULT_PORT,
        NULL,
        NULL
    },
    
    // Password
    {
        CFG_PASSWORD,
        NULL,
        (char *)"@Password",
        (char *)"@Enter_password",
        (char *)KEY_PASSWORD,
        (char *)DEFAULT_PASSWORD,
        NULL,
        NULL
    },
    
    // Calibre Settings section header
    {
        CFG_INFO,
        NULL,
        (char *)"@Calibre_Settings",
        NULL,
        NULL,
        NULL,
        NULL
    },
    
    // Read Status Column
    {
        CFG_TEXT,
        NULL,
        (char *)"@Read_Status_Column",
        (char *)"@Column_name_for_read_status",
        (char *)KEY_READ_COLUMN,
        (char *)DEFAULT_READ_COLUMN,
        NULL,
        NULL
    },
    
    // Read Date Column
    {
        CFG_TEXT,
        NULL,
        (char *)"@Read_Date_Column",
        (char *)"@Column_name_for_read_date",
        (char *)KEY_READ_DATE_COLUMN,
        (char *)DEFAULT_READ_DATE_COLUMN,
        NULL,
        NULL
    },
    
    // Favorite Column
    {
        CFG_TEXT,
        NULL,
        (char *)"@Favorite_Column",
        (char *)"@Column_name_for_favorites",
        (char *)KEY_FAVORITE_COLUMN,
        (char *)DEFAULT_FAVORITE_COLUMN,
        NULL,
        NULL
    },
    
    // Input Folder
    {
        CFG_DIRECTORY,
        NULL,
        (char *)"@Input_Folder",
        (char *)"@Select_books_folder",
        (char *)KEY_INPUT_FOLDER,
        (char *)DEFAULT_INPUT_FOLDER,
        NULL,
        NULL
    },
    
    // End marker
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

// ============================================================================
// CONFIG FUNCTIONS
// ============================================================================

void initConfig() {
    // Ensure config directory exists
    iv_buildpath("/mnt/ext1/system/config");
    
    // Open or create config
    appConfig = OpenConfig(CONFIG_FILE, configItems);
    
    if (!appConfig) {
        // Create new config with defaults
        appConfig = OpenConfig(CONFIG_FILE, NULL);
        if (appConfig) {
            WriteString(appConfig, KEY_IP, DEFAULT_IP);
            WriteString(appConfig, KEY_PORT, DEFAULT_PORT);
            WriteString(appConfig, KEY_PASSWORD, DEFAULT_PASSWORD);
            WriteString(appConfig, KEY_READ_COLUMN, DEFAULT_READ_COLUMN);
            WriteString(appConfig, KEY_READ_DATE_COLUMN, DEFAULT_READ_DATE_COLUMN);
            WriteString(appConfig, KEY_FAVORITE_COLUMN, DEFAULT_FAVORITE_COLUMN);
            WriteString(appConfig, KEY_INPUT_FOLDER, DEFAULT_INPUT_FOLDER);
            WriteInt(appConfig, KEY_CONNECTION_ENABLED, 0);
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

// ============================================================================
// CONFIG EDITOR HANDLERS
// ============================================================================

void configSaveHandler() {
    // Called when config is saved
    saveAndCloseConfig();
    Message(ICON_INFORMATION, (char *)"@Settings", (char *)"@Settings_saved_successfully", 2000);
}

void configItemChangedHandler(char *name) {
    // Called when config item changes
    if (appConfig) {
        SaveConfig(appConfig);
    }
}

// ============================================================================
// MAIN SCREEN
// ============================================================================

void showMainScreen() {
    ClearScreen();
    
    // Initialize and show config editor
    OpenConfigEditor(
        (char *)"@Calibre_Companion_Settings",  // Title
        appConfig,                               // Config handle
        configItems,                             // Config items array
        configSaveHandler,                       // Save handler
        configItemChangedHandler                 // Item change handler
    );
}

// ============================================================================
// EVENT HANDLER
// ============================================================================

int mainEventHandler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            // Initialize panel
            SetPanelType(PANEL_ENABLED);
            
            // Load translations (add them to system)
            AddTranslation("Connection", "Connection");
            AddTranslation("Status", "Status");
            AddTranslation("Disabled", "Disabled");
            AddTranslation("Enabled", "Enabled");
            AddTranslation("IP_Address", "IP Address");
            AddTranslation("Enter_IP_address", "Enter IP address");
            AddTranslation("Port", "Port");
            AddTranslation("Enter_port_number", "Enter port number");
            AddTranslation("Password", "Password");
            AddTranslation("Enter_password", "Enter password");
            AddTranslation("Calibre_Settings", "Calibre Settings");
            AddTranslation("Read_Status_Column", "Read Status Column");
            AddTranslation("Column_name_for_read_status", "Column name for read status");
            AddTranslation("Read_Date_Column", "Read Date Column");
            AddTranslation("Column_name_for_read_date", "Column name for read date");
            AddTranslation("Favorite_Column", "Favorite Column");
            AddTranslation("Column_name_for_favorites", "Column name for favorites");
            AddTranslation("Input_Folder", "Input Folder");
            AddTranslation("Select_books_folder", "Select books folder");
            AddTranslation("Calibre_Companion_Settings", "Calibre Companion");
            AddTranslation("Settings", "Settings");
            AddTranslation("Settings_saved_successfully", "Settings saved successfully");
            
            // Initialize config
            initConfig();
            
            // Show main screen
            showMainScreen();
            break;
            
        case EVT_SHOW:
            // Redraw when returning to app
            FullUpdate();
            break;
            
        case EVT_KEYPRESS:
            if (par1 == IV_KEY_BACK || par1 == IV_KEY_PREV) {
                // Save and close
                saveAndCloseConfig();
                CloseApp();
                return 1;
            }
            break;
            
        case EVT_EXIT:
            // Clean up
            saveAndCloseConfig();
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
