// WinRT ABI interfaces the MinGW headers don't ship, declared from the Windows metadata
// (IIDs and method order as in microsoft/windows-rs). Only what the Xbox app calls.
#pragma once

#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

namespace xwinrt
{
// Windows.Foundation.IAsyncInfo
MIDL_INTERFACE("00000036-0000-0000-C000-000000000046")
IAsyncInfo : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 * id) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(INT32 * status) = 0;  // 0 started, 1 done, 2 canceled, 3 error
  virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT * error) = 0;
  virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// Windows.Foundation.IAsyncOperation<T> for an interface T: the layout every instantiation
// shares. Called on pointers we already hold, so its (parameterized) IID isn't needed.
struct IAsyncOperationRaw : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown * handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown * *handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(IInspectable * *result) = 0;
};

// Windows.Foundation.Collections.IVector<HSTRING> (only Append is used).
struct IVectorHStringRaw : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetView(IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE IndexOf(HSTRING, UINT32*, boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetAt(UINT32, HSTRING) = 0;
  virtual HRESULT STDMETHODCALLTYPE InsertAt(UINT32, HSTRING) = 0;
  virtual HRESULT STDMETHODCALLTYPE RemoveAt(UINT32) = 0;
  virtual HRESULT STDMETHODCALLTYPE Append(HSTRING) = 0;
};

// Windows.Storage.Pickers.IFileOpenPicker
MIDL_INTERFACE("2CA8278A-12C5-4C5F-8977-94547793C241")
IFileOpenPicker : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_ViewMode(INT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ViewMode(INT32) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SettingsIdentifier(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SettingsIdentifier(HSTRING) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SuggestedStartLocation(INT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SuggestedStartLocation(INT32) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_CommitButtonText(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_CommitButtonText(HSTRING) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FileTypeFilter(IVectorHStringRaw**) = 0;
  virtual HRESULT STDMETHODCALLTYPE PickSingleFileAsync(IAsyncOperationRaw**) = 0;
  virtual HRESULT STDMETHODCALLTYPE PickMultipleFilesAsync(IInspectable**) = 0;
};

// WindowsStorageCOM.h: IStorageItemHandleAccess
enum : ULONG
{
  HAO_READ = 0x120089,
  HSO_SHARE_READ = 0x1,
  HO_NONE = 0x0,
  HO_RANDOM_ACCESS = 0x10000000,
};
MIDL_INTERFACE("5CA296B2-2C25-4D22-B785-B885C8201E6A")
IStorageItemHandleAccess : public IUnknown
{
  virtual HRESULT STDMETHODCALLTYPE Create(ULONG access, ULONG sharing, ULONG options,
                                           IUnknown * oplock_handler, HANDLE * handle) = 0;
};

// Windows.UI.Core.SystemNavigationManager (the Xbox B button raises BackRequested).
MIDL_INTERFACE("DC52B5CE-BEE0-4305-8C54-68228ED683B5")
ISystemNavigationManagerStatics : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE GetForCurrentView(IInspectable * *manager) = 0;
};
MIDL_INTERFACE("93023118-CF50-42A6-9706-69107FA122E1")
ISystemNavigationManager : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE add_BackRequested(IUnknown * handler, INT64 * token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_BackRequested(INT64 token) = 0;
};
MIDL_INTERFACE("D603D28A-E411-4A4E-BA41-6A327A8675BC")
IBackRequestedEventArgs : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_Handled(boolean * handled) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Handled(boolean handled) = 0;
};

// Windows.Graphics.Display.Core.HdmiDisplayInformation (the TV's output mode on Xbox).
MIDL_INTERFACE("6CE6B260-F42A-4A15-914C-7B8E2A5A65DF")
IHdmiDisplayInformationStatics : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE GetForCurrentView(IInspectable * *info) = 0;
};
MIDL_INTERFACE("130B3C0A-F565-476E-ABD5-EA05AEE74C69")
IHdmiDisplayInformation : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE GetSupportedDisplayModes(IInspectable * *modes) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentDisplayMode(IInspectable * *mode) = 0;
};
MIDL_INTERFACE("0C06D5AD-1B90-4F51-9981-EF5A1C0DDF66")
IHdmiDisplayMode : public IInspectable
{
  virtual HRESULT STDMETHODCALLTYPE get_ResolutionWidthInRawPixels(UINT32 * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ResolutionHeightInRawPixels(UINT32 * value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RefreshRate(double* value) = 0;
};
}  // namespace xwinrt

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(xwinrt::IAsyncInfo, 0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
__CRT_UUID_DECL(xwinrt::IFileOpenPicker, 0x2ca8278a, 0x12c5, 0x4c5f, 0x89, 0x77, 0x94, 0x54, 0x77, 0x93, 0xc2, 0x41)
__CRT_UUID_DECL(xwinrt::IStorageItemHandleAccess, 0x5ca296b2, 0x2c25, 0x4d22, 0xb7, 0x85, 0xb8, 0x85, 0xc8, 0x20, 0x1e, 0x6a)
__CRT_UUID_DECL(xwinrt::ISystemNavigationManagerStatics, 0xdc52b5ce, 0xbee0, 0x4305, 0x8c, 0x54, 0x68, 0x22, 0x8e, 0xd6, 0x83, 0xb5)
__CRT_UUID_DECL(xwinrt::ISystemNavigationManager, 0x93023118, 0xcf50, 0x42a6, 0x97, 0x06, 0x69, 0x10, 0x7f, 0xa1, 0x22, 0xe1)
__CRT_UUID_DECL(xwinrt::IBackRequestedEventArgs, 0xd603d28a, 0xe411, 0x4a4e, 0xba, 0x41, 0x6a, 0x32, 0x7a, 0x86, 0x75, 0xbc)
__CRT_UUID_DECL(xwinrt::IHdmiDisplayInformationStatics, 0x6ce6b260, 0xf42a, 0x4a15, 0x91, 0x4c, 0x7b, 0x8e, 0x2a, 0x5a, 0x65, 0xdf)
__CRT_UUID_DECL(xwinrt::IHdmiDisplayInformation, 0x130b3c0a, 0xf565, 0x476e, 0xab, 0xd5, 0xea, 0x05, 0xae, 0xe7, 0x4c, 0x69)
__CRT_UUID_DECL(xwinrt::IHdmiDisplayMode, 0x0c06d5ad, 0x1b90, 0x4f51, 0x99, 0x81, 0xef, 0x5a, 0x1c, 0x0d, 0xdf, 0x66)
#endif
