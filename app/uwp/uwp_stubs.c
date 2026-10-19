/* Stand-ins for desktop-only Windows functions that Dolphin code references but the Xbox
 * doesn't let UWP apps import (scripts/uwp_import_check.py lists them). None is used by the
 * game on Xbox; each fails the way the real function does when the feature is unavailable.
 * Defining both the function and its __imp_ pointer keeps the linker from pulling the
 * desktop import library entry. */
#include <stdint.h>

intptr_t BluetoothAuthenticateDevice(void) { return (intptr_t)(50 /* ERROR_NOT_SUPPORTED */); }
void* __imp_BluetoothAuthenticateDevice = (void*)&BluetoothAuthenticateDevice;
intptr_t BluetoothEnumerateInstalledServices(void) { return (intptr_t)(50 /* ERROR_NOT_SUPPORTED */); }
void* __imp_BluetoothEnumerateInstalledServices = (void*)&BluetoothEnumerateInstalledServices;
intptr_t BluetoothFindDeviceClose(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindDeviceClose = (void*)&BluetoothFindDeviceClose;
intptr_t BluetoothFindFirstDevice(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindFirstDevice = (void*)&BluetoothFindFirstDevice;
intptr_t BluetoothFindFirstRadio(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindFirstRadio = (void*)&BluetoothFindFirstRadio;
intptr_t BluetoothFindNextDevice(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindNextDevice = (void*)&BluetoothFindNextDevice;
intptr_t BluetoothFindNextRadio(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindNextRadio = (void*)&BluetoothFindNextRadio;
intptr_t BluetoothFindRadioClose(void) { return (intptr_t)(0); }
void* __imp_BluetoothFindRadioClose = (void*)&BluetoothFindRadioClose;
intptr_t BluetoothGetRadioInfo(void) { return (intptr_t)(50 /* ERROR_NOT_SUPPORTED */); }
void* __imp_BluetoothGetRadioInfo = (void*)&BluetoothGetRadioInfo;
intptr_t BluetoothRemoveDevice(void) { return (intptr_t)(50 /* ERROR_NOT_SUPPORTED */); }
void* __imp_BluetoothRemoveDevice = (void*)&BluetoothRemoveDevice;
intptr_t BluetoothSetServiceState(void) { return (intptr_t)(50 /* ERROR_NOT_SUPPORTED */); }
void* __imp_BluetoothSetServiceState = (void*)&BluetoothSetServiceState;
intptr_t CM_Get_Child(void) { return (intptr_t)(0x13 /* CR_FAILURE */); }
void* __imp_CM_Get_Child = (void*)&CM_Get_Child;
intptr_t CM_Get_Sibling(void) { return (intptr_t)(0x13 /* CR_FAILURE */); }
void* __imp_CM_Get_Sibling = (void*)&CM_Get_Sibling;
intptr_t CloseClipboard(void) { return (intptr_t)(0); }
void* __imp_CloseClipboard = (void*)&CloseClipboard;
intptr_t CryptStringToBinaryW(void) { return (intptr_t)(0); }
void* __imp_CryptStringToBinaryW = (void*)&CryptStringToBinaryW;
intptr_t EmptyClipboard(void) { return (intptr_t)(0); }
void* __imp_EmptyClipboard = (void*)&EmptyClipboard;
intptr_t GetAsyncKeyState(void) { return (intptr_t)(0); }
void* __imp_GetAsyncKeyState = (void*)&GetAsyncKeyState;
intptr_t GetBestInterface(void) { return (intptr_t)(50); }
void* __imp_GetBestInterface = (void*)&GetBestInterface;
intptr_t GetClientRect(void) { return (intptr_t)(0); }
void* __imp_GetClientRect = (void*)&GetClientRect;
intptr_t GetClipboardData(void) { return (intptr_t)(0); }
void* __imp_GetClipboardData = (void*)&GetClipboardData;
intptr_t GetIpForwardTable(void) { return (intptr_t)(50); }
void* __imp_GetIpForwardTable = (void*)&GetIpForwardTable;
intptr_t GetUserDefaultGeoName(void) { return (intptr_t)(0); }
void* __imp_GetUserDefaultGeoName = (void*)&GetUserDefaultGeoName;
intptr_t HidD_GetAttributes(void) { return (intptr_t)(0); }
void* __imp_HidD_GetAttributes = (void*)&HidD_GetAttributes;
intptr_t InitSecurityInterfaceW(void) { return (intptr_t)(0); }
void* __imp_InitSecurityInterfaceW = (void*)&InitSecurityInterfaceW;
intptr_t MessageBoxW(void) { return (intptr_t)(1 /* IDOK */); }
void* __imp_MessageBoxW = (void*)&MessageBoxW;
intptr_t OpenClipboard(void) { return (intptr_t)(0); }
void* __imp_OpenClipboard = (void*)&OpenClipboard;
intptr_t QOSAddSocketToFlow(void) { return (intptr_t)(0); }
void* __imp_QOSAddSocketToFlow = (void*)&QOSAddSocketToFlow;
intptr_t QOSCloseHandle(void) { return (intptr_t)(0); }
void* __imp_QOSCloseHandle = (void*)&QOSCloseHandle;
intptr_t QOSCreateHandle(void) { return (intptr_t)(0); }
void* __imp_QOSCreateHandle = (void*)&QOSCreateHandle;
intptr_t QOSRemoveSocketFromFlow(void) { return (intptr_t)(0); }
void* __imp_QOSRemoveSocketFromFlow = (void*)&QOSRemoveSocketFromFlow;
intptr_t QOSSetFlow(void) { return (intptr_t)(0); }
void* __imp_QOSSetFlow = (void*)&QOSSetFlow;
intptr_t SHGetKnownFolderPath(void) { return (intptr_t)((long)0x80004005 /* E_FAIL */); }
void* __imp_SHGetKnownFolderPath = (void*)&SHGetKnownFolderPath;
intptr_t SetClipboardData(void) { return (intptr_t)(0); }
void* __imp_SetClipboardData = (void*)&SetClipboardData;
intptr_t ShellExecuteW(void) { return (intptr_t)(2 /* <= 32: failed */); }
void* __imp_ShellExecuteW = (void*)&ShellExecuteW;
intptr_t __C_specific_handler(void) { return (intptr_t)(1 /* ExceptionContinueSearch */); }
void* __imp___C_specific_handler = (void*)&__C_specific_handler;
