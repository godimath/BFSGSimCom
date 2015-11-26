/*
 * TeamSpeak 3 demo plugin
 *
 * Copyright (c) 2008-2014 TeamSpeak Systems GmbH
 */

#ifdef _WIN32
#pragma warning (disable : 4100)  /* Disable Unreferenced parameter warning */
#include <Windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "public_errors.h"
#include "public_errors_rare.h"
#include "public_definitions.h"
#include "public_rare_definitions.h"
#include "ts3_functions.h"

#include "BFSGSimCom.h"
#include "FSUIPCWrapper.h"
#include "TS3Channels.h"

#include "config.h"

struct TS3Functions ts3Functions;

#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#define snprintf sprintf_s
#else
#define _strcpy(dest, destSize, src) { strncpy(dest, src, destSize-1); (dest)[destSize-1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 20

#define PATH_BUFSIZE 512
#define COMMAND_BUFSIZE 128
#define INFODATA_BUFSIZE 1024
#define SERVERINFO_BUFSIZE 256
#define CHANNELINFO_BUFSIZE 512
#define RETURNCODE_BUFSIZE 128

static char* pluginID = NULL;
static char* callbackReturnCode = NULL;

static FSUIPCWrapper* fsuipc = NULL;
TS3Channels ts3Channels;
FSUIPCWrapper::SimComData simComData;
Config* cfg;

bool blConnected;
anyID myTS3ID;
uint64 lastTargetChannel;
uint64 currentChannel;

PluginItemType infoDataType = PluginItemType(0);
uint64 infoDataId = 0;

#ifdef _WIN32
/* Helper function to convert wchar_T to Utf-8 encoded strings on Windows */
static int wcharToUtf8(const wchar_t* str, char** result) {
    int outlen = WideCharToMultiByte(CP_UTF8, 0, str, -1, 0, 0, 0, 0);
    *result = (char*)malloc(outlen);
    if (WideCharToMultiByte(CP_UTF8, 0, str, -1, *result, outlen, 0, 0) == 0) {
        *result = NULL;
        return -1;
    }
    return 0;
}
#endif

void callback(FSUIPCWrapper::SimComData data)
{
    uint64 serverConnectionHandlerID = ts3Functions.getCurrentServerConnectionHandlerID();

    if (pluginID != NULL && callbackReturnCode == NULL)
    {
        callbackReturnCode = (char*)malloc(65 * sizeof(char));
        ts3Functions.createReturnCode(pluginID, callbackReturnCode, 64);
    }
    
    simComData = data;

    // Assuming we're connected, and we're supposed to be moving when the channel changes
    if (blConnected)
    {
        if (cfg->getMode() != Config::CONFIG_DISABLED)
        {
            uint16_t frequency;
            uint64 targetChannel;

            // Work out which radio is active, and therefore the current tuned frequency.
            switch (data.selectedCom)
            {
            case FSUIPCWrapper::Com1:
                frequency = data.iCom1Freq;
                break;
            case FSUIPCWrapper::Com2:
                frequency = data.iCom2Freq;
                break;
            default:
                frequency = 0;
            }

            // Get any new target channel
            targetChannel = ts3Channels.getChannelID(
                frequency,
                currentChannel,
                cfg->getRootChannel()
                );

            // If there's no defined channel found
            if (targetChannel == TS3Channels::CHANNEL_ID_NOT_FOUND)
            {
                // Then if we're going somewhere special in the event that we tune an unknown channel, find out
                // what that is.
                if (cfg->getUntuned())
                {
                    targetChannel = cfg->getUntunedChannel();
                }
                else
                {
                    // Otherwise the target this time around is the same as the target last time around.
                    targetChannel = lastTargetChannel;
                }
            }

            // if where we are is different from where we should be, then initiate the change.
            if (currentChannel != targetChannel)
            {
                ts3Functions.requestClientMove(serverConnectionHandlerID, myTS3ID, targetChannel, "", callbackReturnCode);
                //ts3Functions.requestClientMove(serverConnectionHandlerID, myTS3ID, targetChannel, "", NULL);

                // remember where we were sent last time - just in case!
                lastTargetChannel = targetChannel;
            }
        }
        else
        {
            // if we're connected, but disabled, keep an eye on where we are, and make it the last target.
            lastTargetChannel = currentChannel;
        }
    }

    //if (infoDataType > 0 && infoDataId > 0 ) ts3Functions.requestInfoUpdate(serverConnectionHandlerID, infoDataType, infoDataId);
    
    // This is a frig to avoid trying to update client data from an unrelated thread. What we're waiting for is
    // for the onServerUpdatedEvent to fire so we can safely request the info update.
    if (infoDataType > 0 && infoDataId > 0) ts3Functions.requestServerVariables(serverConnectionHandlerID);
}


/*********************************** Required functions ************************************/
/*
 * If any of these required functions is not implemented, TS3 will refuse to load the plugin
 */

/* Unique name identifying this plugin */
const char* ts3plugin_name() {
#ifdef _WIN32
    /* TeamSpeak expects UTF-8 encoded characters. Following demonstrates a possibility how to convert UTF-16 wchar_t into UTF-8. */
    static char* result = NULL;  /* Static variable so it's allocated only once */
    if (!result) {
        const wchar_t* name = L"BFSGSimCom";
        if (wcharToUtf8(name, &result) == -1) {  /* Convert name into UTF-8 encoded result */
            result = "BFSGSimCom";  /* Conversion failed, fallback here */
        }
    }
    return result;
#else
    return "BFSGSimCom";
#endif
}

/* Plugin version */
const char* ts3plugin_version() {
    return "0.1";
}

/* Plugin API version. Must be the same as the clients API major version, else the plugin fails to load. */
int ts3plugin_apiVersion() {
    return PLUGIN_API_VERSION;
}

/* Plugin author */
const char* ts3plugin_author() {
    /* If you want to use wchar_t, see ts3plugin_name() on how to use */
    return "Andrew J. Parish";
}

/* Plugin description */
const char* ts3plugin_description() {
    /* If you want to use wchar_t, see ts3plugin_name() on how to use */
    return "This plugin enables automatic channel moving for simulator pilots as they tune their simulated COM radio.";
}

/* Set TeamSpeak 3 callback functions */
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

void loadChannels(uint64 serverConnectionHandlerID)
{
    uint64* channelList;

    if (ts3Functions.getChannelList(serverConnectionHandlerID, &channelList) == ERROR_ok)
    {
        for (int i = 0; channelList[i] != NULL; i++)
        {
            string strName;
            char* cName;
            uint64 parent;

            // For each channel in the list, get the name of the channel and its parent, and add it to the channel list.
            ts3Functions.getChannelVariableAsString(serverConnectionHandlerID, channelList[i], CHANNEL_NAME, &cName);
            ts3Functions.getParentChannelOfChannel(serverConnectionHandlerID, channelList[i], &parent);
            ts3Channels.addOrUpdateChannel(cName, channelList[i], parent);

            // Not forgetting to free up the memory we've used for the channel name.
            ts3Functions.freeMemory(cName);
        }

        // And not forgetting to free up the memory we've used for the channel list.
        ts3Functions.freeMemory(channelList);
    }

}

/*
 * Custom code called right after loading the plugin. Returns 0 on success, 1 on failure.
 * If the function returns 1 on failure, the plugin will be unloaded again.
 */
int ts3plugin_init() {

    uint64 serverConnectionHandlerID;
    int connectionStatus;

    serverConnectionHandlerID = ts3Functions.getCurrentServerConnectionHandlerID();
    
    // Initialise the server connection status
    if (ts3Functions.getConnectionStatus(serverConnectionHandlerID, &connectionStatus) == ERROR_ok)
    {
        blConnected = (connectionStatus != 0);

        // If we're already connected when the plugin starts, get my ID and the current channel
        if (blConnected)
        {
            if (ts3Functions.getClientID(serverConnectionHandlerID, &myTS3ID) == ERROR_ok)
            {
                ts3Functions.getChannelOfClient(serverConnectionHandlerID, myTS3ID, &currentChannel);
            }
        }
    }
    else
    {
        blConnected = false;
    }

    if (fsuipc == NULL)
    {
        fsuipc = new FSUIPCWrapper(&callback);
    }

    cfg = new Config(ts3Channels);

    if (fsuipc)
    {
        fsuipc->start();
    }

//#if defined(_DEBUG)
//    ts3Channels.addOrUpdateChannel("Lobby", 1, 0);
//    ts3Channels.addOrUpdateChannel("Fly-in 1", 2, 0);
//    ts3Channels.addOrUpdateChannel("F1 Dep - 118.300", 3, 2);
//    ts3Channels.addOrUpdateChannel("F1 Unicom - 122.800", 4, 2);
//    ts3Channels.addOrUpdateChannel("F1 Dep - 119.125", 5, 2);
//    ts3Channels.addOrUpdateChannel("Fly-in 2", 6, 0);
//    ts3Channels.addOrUpdateChannel("F2 Departure", 7, 6);
//    ts3Channels.addOrUpdateChannel("F2D Ground - 118.300", 8, 7);
//    ts3Channels.addOrUpdateChannel("F2D Tower - 125.100", 9, 7);
//    ts3Channels.addOrUpdateChannel("F2D Departure - 119.125", 10, 7);
//    ts3Channels.addOrUpdateChannel("F2 EnRoute", 11, 6);
//    ts3Channels.addOrUpdateChannel("F2 Unicom - 122.800", 12, 11);
//    ts3Channels.addOrUpdateChannel("F2 Arrival", 13, 6);
//    ts3Channels.addOrUpdateChannel("F2D Departure - 118.300", 14, 13);
//    ts3Channels.addOrUpdateChannel("F2D Tower - 125.100", 15, 13);
//    ts3Channels.addOrUpdateChannel("F2D Ground - 119.125", 16, 13);
//    ts3Channels.addOrUpdateChannel("F2 Other - 118.300", 17, 6);
//    ts3Channels.addOrUpdateChannel("F2 Other - 122.800", 18, 17);
//    ts3Channels.addOrUpdateChannel("F2 Other - 118.300", 19, 18);
//#endif

    loadChannels(serverConnectionHandlerID);

    return 0;  /* 0 = success, 1 = failure, -2 = failure but client will not show a "failed to load" warning */
    /* -2 is a very special case and should only be used if a plugin displays a dialog (e.g. overlay) asking the user to disable
     * the plugin again, avoiding the show another dialog by the client telling the user the plugin failed to load.
     * For normal case, if a plugin really failed to load because of an error, the correct return value is 1. */
}

/* Custom code called right before the plugin is unloaded */
void ts3plugin_shutdown() {

    // Close down the connection to the simulator
    if (fsuipc)
    {
        fsuipc->stop();
        delete fsuipc;
    }

    // Close down the settings dialog
    if (cfg)
    {
        cfg->close();
        delete cfg;
    }

    // Free pluginID if we registered it
    if (pluginID) {
        free(pluginID);
        pluginID = NULL;
    }
}

/****************************** Optional functions ********************************/
/*
 * Following functions are optional, if not needed you don't need to implement them.
 */

/* Tell client if plugin offers a configuration window. If this function is not implemented, it's an assumed "does not offer" (PLUGIN_OFFERS_NO_CONFIGURE). */
int ts3plugin_offersConfigure() {

    return PLUGIN_OFFERS_CONFIGURE_QT_THREAD;  /* In this case ts3plugin_configure does not need to be implemented */
}

/* Plugin might offer a configuration window. If ts3plugin_offersConfigure returns 0, this function does not need to be implemented. */
void ts3plugin_configure(void* handle, void* qParentWidget) {

    /* Execute the config dialog */
    cfg->exec();
}

/*
 * If the plugin wants to use error return codes, plugin commands, hotkeys or menu items, it needs to register a command ID. This function will be
 * automatically called after the plugin was initialized. This function is optional. If you don't use these features, this function can be omitted.
 * Note the passed pluginID parameter is no longer valid after calling this function, so you must copy it and store it in the plugin.
 */
void ts3plugin_registerPluginID(const char* id) {
    const size_t sz = strlen(id) + 1;
    pluginID = (char*)malloc(sz * sizeof(char));
    if (pluginID != NULL)
        _strcpy(pluginID, sz, id);  /* The id buffer will invalidate after exiting this function */
}

/* Plugin command keyword. Return NULL or "" if not used. */
const char* ts3plugin_commandKeyword() {
    return NULL;
}


/* Client changed current server connection handler */
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    //printf("PLUGIN: currentServerConnectionChanged %llu (%llu)\n", (long long unsigned int)serverConnectionHandlerID, (long long unsigned int)ts3Functions.getCurrentServerConnectionHandlerID());
}

/*
 * Implement the following three functions when the plugin should display a line in the server/channel/client info.
 * If any of ts3plugin_infoTitle, ts3plugin_infoData or ts3plugin_freeMemory is missing, the info text will not be displayed.
 */

/* Static title shown in the left column in the info frame */
const char* ts3plugin_infoTitle() {
    return "BFSGSimCom Information";
}

/*
 * Dynamic content shown in the right column in the info frame. Memory for the data string needs to be allocated in this
 * function. The client will call ts3plugin_freeMemory once done with the string to release the allocated memory again.
 * Check the parameter "type" if you want to implement this feature only for specific item types. Set the parameter
 * "data" to NULL to have the client ignore the info data.
 */
void ts3plugin_infoData(uint64 serverConnectionHandlerID, uint64 id, enum PluginItemType type, char** data) {

    char* strConnected = "";
    char* strMode = "";
    char* strUntuned = "";
    char* strCom = "";
    char* strFormat = "";

    double dCom1 = 0.0f;
    double dCom1s = 0.0f;
    double dCom2 = 0.0f;
    double dCom2s = 0.0f;

    // Save this in case we need to do it adhoc...
    infoDataType = type;
    infoDataId = id;

    // Must be allocated in the plugin
    *data = (char*)malloc(INFODATA_BUFSIZE * sizeof(char));  /* Must be allocated in the plugin! */

    if (*data != NULL)
    {
        bool connected = fsuipc->isConnected();

        if (connected)
        {
            strConnected = "Connected to Sim.";

            Config::ConfigMode mode = cfg->getMode();
            switch (mode)
            {
            case Config::CONFIG_DISABLED:
                strMode = "Disabled";
                break;
            case Config::CONFIG_MANUAL:
                strMode = "Enabled but not locked";
                break;
            case Config::CONFIG_AUTO:
                strMode = "Enabled and locked";
                break;
            default:
                strMode = "";
            }

            if (mode != Config::CONFIG_DISABLED)
            {
                strUntuned = (cfg->getUntuned()) ? "" : "not ";
                strFormat = "%s\n\nMode: %s, %smoving with unrecognised freq.\n\n%s radio selected.\n\nCom 1 Freq:%6.2f\nCom 2 Freq:%6.2f\nCom 1 Stby:%6.2f\nCom 2 Stby:%6.2f";

                switch (simComData.selectedCom)
                {
                case FSUIPCWrapper::Com1:
                    strCom = "COM1";
                    break;
                case FSUIPCWrapper::Com2:
                    strCom = "COM2";
                    break;
                default:
                    strCom = "Unrecognised";
                    break;
                }

                dCom1 = 0.01 * simComData.iCom1Freq;
                dCom1s = 0.01 * simComData.iCom1Sby;
                dCom2 = 0.01 * simComData.iCom2Freq;
                dCom2s = 0.01 * simComData.iCom2Sby;
            }
            else
            {
                strFormat = "%s\n\nMode: %s";
            }
        }
        else
        {
            strConnected = "Not connected to Sim.";
            strFormat = "%s";
        }

        snprintf(
            *data,
            INFODATA_BUFSIZE,
            strFormat,
            strConnected, strMode, strUntuned, strCom, dCom1, dCom2, dCom1s, dCom2s
            );

    }
}

/* Required to release the memory for parameter "data" allocated in ts3plugin_infoData and ts3plugin_initMenus */
void ts3plugin_freeMemory(void* data) {
    free(data);
}

/*
 * Plugin requests to be always automatically loaded by the TeamSpeak 3 client unless
 * the user manually disabled it in the plugin dialog.
 * This function is optional. If missing, no autoload is assumed.
 */
int ts3plugin_requestAutoload() {
    return 0;  /* 1 = request autoloaded, 0 = do not request autoload */
}

/* Helper function to create a menu item */
static struct PluginMenuItem* createMenuItem(enum PluginMenuType type, int id, const char* text, const char* icon) {
    struct PluginMenuItem* menuItem = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));

    if (menuItem != NULL)
    {
        menuItem->type = type;
        menuItem->id = id;
        _strcpy(menuItem->text, PLUGIN_MENU_BUFSZ, text);
        _strcpy(menuItem->icon, PLUGIN_MENU_BUFSZ, icon);
    }

    return menuItem;
}

/* Some makros to make the code to create menu items a bit more readable */
#define BEGIN_CREATE_MENUS(x) const size_t sz = x + 1; size_t n = 0; *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * sz);
#define CREATE_MENU_ITEM(a, b, c, d) if (menuItems != NULL && *menuItems != NULL) (*menuItems)[n++] = createMenuItem(a, b, c, d);
#define END_CREATE_MENUS if (menuItems != NULL && *menuItems != NULL) (*menuItems)[n++] = NULL; assert(n == sz);

/*
 * Menu IDs for this plugin. Pass these IDs when creating a menuitem to the TS3 client. When the menu item is triggered,
 * ts3plugin_onMenuItemEvent will be called passing the menu ID of the triggered menu item.
 * These IDs are freely choosable by the plugin author. It's not really needed to use an enum, it just looks prettier.
 */
enum {
    MENU_ID_SIMCOM_CONFIGURE = 1,
    MENU_ID_SIMCOM_MODE_DISABLE,
    MENU_ID_SIMCOM_MODE_MANUAL,
    MENU_ID_SIMCOM_MODE_AUTO
};

/*
 * Initialize plugin menus.
 * This function is called after ts3plugin_init and ts3plugin_registerPluginID. A pluginID is required for plugin menus to work.
 * Both ts3plugin_registerPluginID and ts3plugin_freeMemory must be implemented to use menus.
 * If plugin menus are not used by a plugin, do not implement this function or return NULL.
 */
void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    /*
     * Create the menus
     * There are three types of menu items:
     * - PLUGIN_MENU_TYPE_CLIENT:  Client context menu
     * - PLUGIN_MENU_TYPE_CHANNEL: Channel context menu
     * - PLUGIN_MENU_TYPE_GLOBAL:  "Plugins" menu in menu bar of main window
     *
     * Menu IDs are used to identify the menu item when ts3plugin_onMenuItemEvent is called
     *
     * The menu text is required, max length is 128 characters
     *
     * The icon is optional, max length is 128 characters. When not using icons, just pass an empty string.
     * Icons are loaded from a subdirectory in the TeamSpeak client plugins folder. The subdirectory must be named like the
     * plugin filename, without dll/so/dylib suffix
     * e.g. for "test_plugin.dll", icon "1.png" is loaded from <TeamSpeak 3 Client install dir>\plugins\test_plugin\1.png
     */

    BEGIN_CREATE_MENUS(4);  /* IMPORTANT: Number of menu items must be correct! */
    CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_SIMCOM_CONFIGURE, "Configure", "");
    CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_SIMCOM_MODE_DISABLE, "Mode - Disable", "");
    CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_SIMCOM_MODE_MANUAL, "Mode - Manual", "");
    CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_SIMCOM_MODE_AUTO, "Mode - Auto", "");
    END_CREATE_MENUS;  /* Includes an assert checking if the number of menu items matched */

    /*
     * Specify an optional icon for the plugin. This icon is used for the plugins submenu within context and main menus
     * If unused, set menuIcon to NULL
     */
    //	*menuIcon = (char*)malloc(PLUGIN_MENU_BUFSZ * sizeof(char));
    //	_strcpy(*menuIcon, PLUGIN_MENU_BUFSZ, "t.png");

    /*
     * Menus can be enabled or disabled with: ts3Functions.setPluginMenuEnabled(pluginID, menuID, 0|1);
     * Test it with plugin command: /test enablemenu <menuID> <0|1>
     * Menus are enabled by default. Please note that shown menus will not automatically enable or disable when calling this function to
     * ensure Qt menus are not modified by any thread other the UI thread. The enabled or disable state will change the next time a
     * menu is displayed.
     */
    /* For example, this would disable MENU_ID_GLOBAL_2: */

    /* All memory allocated in this function will be automatically released by the TeamSpeak client later by calling ts3plugin_freeMemory */
}

/* Helper function to create a hotkey */
static struct PluginHotkey* createHotkey(const char* keyword, const char* description) {
    struct PluginHotkey* hotkey = (struct PluginHotkey*)malloc(sizeof(struct PluginHotkey));
    if (hotkey != NULL)
    {
        _strcpy(hotkey->keyword, PLUGIN_HOTKEY_BUFSZ, keyword);
        _strcpy(hotkey->description, PLUGIN_HOTKEY_BUFSZ, description);
    }
    return hotkey;
}

/* Some makros to make the code to create hotkeys a bit more readable */
#define BEGIN_CREATE_HOTKEYS(x) const size_t sz = x + 1; size_t n = 0; *hotkeys = (struct PluginHotkey**)malloc(sizeof(struct PluginHotkey*) * sz);
#define CREATE_HOTKEY(a, b) if (hotkeys != NULL && *hotkeys != NULL) (*hotkeys)[n++] = createHotkey(a, b);
#define END_CREATE_HOTKEYS if (hotkeys != NULL && *hotkeys != NULL) (*hotkeys)[n++] = NULL; assert(n == sz);

/*
 * Initialize plugin hotkeys. If your plugin does not use this feature, this function can be omitted.
 * Hotkeys require ts3plugin_registerPluginID and ts3plugin_freeMemory to be implemented.
 * This function is automatically called by the client after ts3plugin_init.
 */
void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys) {
    /* Register hotkeys giving a keyword and a description.
     * The keyword will be later passed to ts3plugin_onHotkeyEvent to identify which hotkey was triggered.
     * The description is shown in the clients hotkey dialog. */
    BEGIN_CREATE_HOTKEYS(3);  /* Create 3 hotkeys. Size must be correct for allocating memory. */
    CREATE_HOTKEY("BFSGSimCom_Off", "BFSGSimCom - Disabled");
    CREATE_HOTKEY("BFSGSimCom_Man", "BFSGSimCom - Manual");
    CREATE_HOTKEY("BFSGSimCom_Aut", "BFSGSimCom - Automatic");
    END_CREATE_HOTKEYS;

    /* The client will call ts3plugin_freeMemory to release all allocated memory */
}

/************************** TeamSpeak callbacks ***************************/
/*
 * Following functions are optional, feel free to remove unused callbacks.
 * See the clientlib documentation for details on each function.
 */

/* Clientlib */

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {

    switch (newStatus)
    {
        // When we disconnect, forget everything.
    case STATUS_DISCONNECTED:
        ts3Channels.deleteAllChannels();
        blConnected = false;
        myTS3ID = 0;
        break;
        // When we connect...
    case STATUS_CONNECTION_ESTABLISHED:
        // Get a list of all of the channels on the server, and iterate through it.
        loadChannels(serverConnectionHandlerID);
        blConnected = true;
        ts3Functions.getClientID(serverConnectionHandlerID, &myTS3ID);
        break;
    default:
        break;
    }


}

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage) {

    if (clientID = myTS3ID)
    {
        if (cfg->getMode() == Config::ConfigMode::CONFIG_AUTO && newChannelID != lastTargetChannel)
        {
            ts3Functions.requestClientMove(serverConnectionHandlerID, clientID, lastTargetChannel, "", NULL);
        }
        else
        {
            currentChannel = newChannelID;
        }
    }
}

int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage) {
    //printf("PLUGIN: onServerErrorEvent %llu %s %d %s\n", (long long unsigned int)serverConnectionHandlerID, errorMessage, error, (returnCode ? returnCode : ""));
    if (error != ERROR_ok )
    {
        if (returnCode) {
            /* A plugin could now check the returnCode with previously (when calling a function) remembered returnCodes and react accordingly */
            /* In case of using a a plugin return code, the plugin can return:
            * 0: Client will continue handling this error (print to chat tab)
            * 1: Client will ignore this error, the plugin announces it has handled it */
            return 0;
        }
    }
    else
    {
        // There's no error - so just say we've handled it.
        return 1;
    }

    /* If no plugin return code was used, the return value of this function is ignored */
}

/*
 * Called when a plugin menu item (see ts3plugin_initMenus) is triggered. Optional function, when not using plugin menus, do not implement this.
 *
 * Parameters:
 * - serverConnectionHandlerID: ID of the current server tab
 * - type: Type of the menu (PLUGIN_MENU_TYPE_CHANNEL, PLUGIN_MENU_TYPE_CLIENT or PLUGIN_MENU_TYPE_GLOBAL)
 * - menuItemID: Id used when creating the menu item
 * - selectedItemID: Channel or Client ID in the case of PLUGIN_MENU_TYPE_CHANNEL and PLUGIN_MENU_TYPE_CLIENT. 0 for PLUGIN_MENU_TYPE_GLOBAL.
 */
void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID) {
    //    printf("PLUGIN: onMenuItemEvent: serverConnectionHandlerID=%llu, type=%d, menuItemID=%d, selectedItemID=%llu\n", (long long unsigned int)serverConnectionHandlerID, type, menuItemID, (long long unsigned int)selectedItemID);
    switch (type) {
    case PLUGIN_MENU_TYPE_GLOBAL:
        /* Global menu item was triggered. selectedItemID is unused and set to zero. */
        switch (menuItemID) {
        case MENU_ID_SIMCOM_CONFIGURE:
            /* Menu global 1 was triggered */
            cfg->exec();
            break;
        case MENU_ID_SIMCOM_MODE_DISABLE:
            /* Menu global 1 was triggered */
            cfg->setMode(Config::ConfigMode::CONFIG_DISABLED);
            break;
        case MENU_ID_SIMCOM_MODE_MANUAL:
            /* Menu global 1 was triggered */
            cfg->setMode(Config::ConfigMode::CONFIG_MANUAL);
            break;
        case MENU_ID_SIMCOM_MODE_AUTO:
            /* Menu global 1 was triggered */
            cfg->setMode(Config::ConfigMode::CONFIG_AUTO);
            break;
        default:
            break;
        }

        ts3Functions.requestInfoUpdate(serverConnectionHandlerID, infoDataType, infoDataId);
        break;
    default:
        break;
    }
}

/* This function is called if a plugin hotkey was pressed. Omit if hotkeys are unused. */
void ts3plugin_onHotkeyEvent(const char* keyword) {
    //printf("PLUGIN: Hotkey event: %s\n", keyword);
    /* Identify the hotkey by keyword ("keyword_1", "keyword_2" or "keyword_3" in this example) and handle here... */
    string strKeyword(keyword);

    if (strKeyword == "BFSGSimCom_Off")
    {
        cfg->setMode(Config::ConfigMode::CONFIG_DISABLED);
    }
    else if (strKeyword == "BFSGSimCom_Man")
    {
        cfg->setMode(Config::ConfigMode::CONFIG_MANUAL);
    }
    else if (strKeyword == "BFSGSimCom_Aut")
    {
        cfg->setMode(Config::ConfigMode::CONFIG_AUTO);
    }
}

void ts3plugin_onServerUpdatedEvent(uint64 serverConnectionHandlerID)
{
    ts3Functions.requestInfoUpdate(serverConnectionHandlerID, infoDataType, infoDataId);
}