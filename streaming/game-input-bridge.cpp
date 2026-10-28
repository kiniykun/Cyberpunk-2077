// Local browser input transport for this Windows development build.
// Feed original WM_INPUT processing; leave game rules, menus and rendering intact.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

struct Packet { DWORD version,type; LONG x,y,dx,dy; DWORD button,down,vk,scan; LONG wheel; };
static_assert(sizeof(Packet)==44);
static HWND gameWindow;
static WNDPROC previousProc;
static RAWINPUT pending;
static bool delivering=false,remoteActive=false;
static POINT remotePoint={0,0};
static UINT (WINAPI *originalRaw)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
static DWORD (WINAPI *originalMessagePos)();
static BOOL (WINAPI *originalCursorPos)(LPPOINT);
static const ULONG_PTR fakeHandle=0x4350424900000001ULL;

static UINT WINAPI bridgeRaw(HRAWINPUT handle,UINT command,LPVOID data,PUINT bytes,UINT header) {
    if (!delivering || ULONG_PTR(handle)!=fakeHandle) return originalRaw(handle,command,data,bytes,header);
    const UINT needed=sizeof(RAWINPUT);
    if (!data) {*bytes=needed;return 0;}
    if (*bytes<needed || command!=RID_INPUT || header!=sizeof(RAWINPUTHEADER)) return UINT(-1);
    memcpy(data,&pending,needed);*bytes=needed;return needed;
}
static DWORD WINAPI bridgeMessagePos() {
    if (!delivering) return originalMessagePos();
    POINT point=remotePoint;ClientToScreen(gameWindow,&point);
    return MAKELONG(SHORT(point.x),SHORT(point.y));
}
static BOOL WINAPI bridgeCursorPos(LPPOINT point) {
    if (!remoteActive) return originalCursorPos(point);
    *point=remotePoint;return ClientToScreen(gameWindow,point);
}
static bool patchImport(HMODULE module,const char* name,void* replacement,void** original) {
    auto base=reinterpret_cast<BYTE*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);
    for (;imports->Name;imports++) {
        if (!imports->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->OriginalFirstThunk);
        auto slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->FirstThunk);
        for (;names->u1.AddressOfData;names++,slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto entry=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<char*>(entry->Name),name)!=0) continue;
            DWORD protection;
            auto slot=reinterpret_cast<void**>(&slots->u1.Function);
            if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) return false;
            *original=*slot;InterlockedExchangePointer(slot,replacement);
            VirtualProtect(slot,sizeof(void*),protection,&protection);
            return true;
        }
    }
    return false;
}
static LRESULT CALLBACK bridgeProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_COPYDATA) {
        auto data=reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (data && data->dwData==0x43504249 && data->cbData==sizeof(Packet)) {
            Packet p=*reinterpret_cast<Packet*>(data->lpData);
            if (p.version!=1 || p.type<1 || p.type>4 || p.button>2 || p.down>1 || p.vk>255) return FALSE;
            RECT rect;GetClientRect(window,&rect);
            remotePoint.x=max(0L,min(rect.right-1,p.x));remotePoint.y=max(0L,min(rect.bottom-1,p.y));
            remoteActive=true;ZeroMemory(&pending,sizeof(pending));
            pending.header.dwSize=sizeof(RAWINPUT);
            pending.header.hDevice=reinterpret_cast<HANDLE>(0x43504249);
            if (p.type==4) {
                pending.header.dwType=RIM_TYPEKEYBOARD;
                pending.data.keyboard.VKey=USHORT(p.vk);pending.data.keyboard.MakeCode=USHORT(p.scan);
                pending.data.keyboard.Flags=p.down?0:RI_KEY_BREAK;
                if (p.vk>=VK_PRIOR && p.vk<=VK_DOWN) pending.data.keyboard.Flags|=RI_KEY_E0;
                pending.data.keyboard.Message=p.down?WM_KEYDOWN:WM_KEYUP;
            } else {
                pending.header.dwType=RIM_TYPEMOUSE;
                pending.data.mouse.lLastX=p.dx;pending.data.mouse.lLastY=p.dy;
                if (p.type==2) {
                    const USHORT presses[]={RI_MOUSE_LEFT_BUTTON_DOWN,RI_MOUSE_MIDDLE_BUTTON_DOWN,RI_MOUSE_RIGHT_BUTTON_DOWN};
                    const USHORT releases[]={RI_MOUSE_LEFT_BUTTON_UP,RI_MOUSE_MIDDLE_BUTTON_UP,RI_MOUSE_RIGHT_BUTTON_UP};
                    pending.data.mouse.usButtonFlags=p.down?presses[p.button]:releases[p.button];
                }
                if (p.type==3) {pending.data.mouse.usButtonFlags=RI_MOUSE_WHEEL;pending.data.mouse.usButtonData=USHORT(SHORT(p.wheel));}
            }
            delivering=true;
            CallWindowProcW(previousProc,window,WM_INPUT,RIM_INPUT,LPARAM(fakeHandle));
            delivering=false;return TRUE;
        }
    }
    return CallWindowProcW(previousProc,window,msg,wp,lp);
}
static BOOL CALLBACK findGameWindow(HWND window,LPARAM) {
    DWORD pid;GetWindowThreadProcessId(window,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(window)) {gameWindow=window;return FALSE;}
    return TRUE;
}
static DWORD WINAPI install(LPVOID) {
    HMODULE input=GetModuleHandleW(L"input.dll");
    EnumWindows(findGameWindow,0);
    bool raw=input && patchImport(input,"GetRawInputData",reinterpret_cast<void*>(bridgeRaw),reinterpret_cast<void**>(&originalRaw));
    bool position=input && patchImport(input,"GetMessagePos",reinterpret_cast<void*>(bridgeMessagePos),reinterpret_cast<void**>(&originalMessagePos));
    bool cursor=input && patchImport(input,"GetCursorPos",reinterpret_cast<void*>(bridgeCursorPos),reinterpret_cast<void**>(&originalCursorPos));
    bool installed=false;
    if (raw && position && cursor && gameWindow) {
        previousProc=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow,GWLP_WNDPROC));
        installed=SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(bridgeProc))!=0;
    }
    HANDLE file=CreateFileW(L"D:\\2077\\streaming\\bridge-status.json",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {
        char text[256];wsprintfA(text,"{\"installed\":%s,\"rawInputPatched\":%s,\"messagePositionPatched\":%s,\"cursorPositionPatched\":%s,\"gamePID\":%lu}",installed?"true":"false",raw?"true":"false",position?"true":"false",cursor?"true":"false",GetCurrentProcessId());
        DWORD written;WriteFile(file,text,lstrlenA(text),&written,nullptr);CloseHandle(file);
    }
    return installed?0:1;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {HANDLE thread=CreateThread(nullptr,0,install,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    return TRUE;
}
