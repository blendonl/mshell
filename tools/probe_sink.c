#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAXW 64
#define MAXEV 20000

typedef struct { HWND h; wchar_t cls[48], title[60]; int above; int flashes;
                 double first_flash; } W;

static W w[MAXW]; static int wn;
static FILE *out;
static LARGE_INTEGER qf, q0;
static volatile LONG fg_count;
static double fg_t[MAXEV]; static volatile LONG fg_n;
static wchar_t fg_cls[MAXEV][40];

static double nowt(void){ LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (double)(n.QuadPart-q0.QuadPart)/(double)qf.QuadPart; }

static void CALLBACK evp(HWINEVENTHOOK hk,DWORD e,HWND h,LONG o,LONG c,DWORD th,DWORD tm){
    (void)hk;(void)o;(void)c;(void)th;(void)tm;
    if (e!=EVENT_SYSTEM_FOREGROUND) return;
    LONG i = InterlockedIncrement(&fg_n)-1;
    if (i < MAXEV){ fg_t[i]=nowt(); GetClassNameW(h,fg_cls[i],39); }
    InterlockedIncrement(&fg_count);
}

static int seconds = 15;

static DWORD WINAPI poll_thread(LPVOID p){
    (void)p;
    double t_end = seconds;
    int bg_moves = 0; int last_bg = -1;
    double last_flash_t = -1; int flash_events = 0;
    double gaps[400]; int gn = 0;

    while (nowt() < t_end) {
        HWND snap[300]; int n=0;
        for (HWND h=GetTopWindow(NULL); h&&n<300; h=GetWindow(h,GW_HWNDNEXT)) snap[n++]=h;
        int bgz=-1;
        for (int i=0;i<n;i++){ wchar_t c[48]; GetClassNameW(snap[i],c,48);
            if(!wcscmp(c,L"mshell_Background")){bgz=i;break;} }
        if (bgz<0) { Sleep(2); continue; }
        if (last_bg>=0 && bgz!=last_bg) bg_moves++;
        last_bg = bgz;

        int any_flash = 0;
        for (int i=0;i<n;i++){
            if (i==bgz) continue;
            HWND h=snap[i];
            if (!IsWindowVisible(h)) continue;
            RECT r; if(!GetWindowRect(h,&r)) continue;
            if (r.right-r.left < 200 || r.bottom-r.top < 200) continue;
            wchar_t c[48]; GetClassNameW(h,c,48);
            if (!wcsncmp(c,L"mshell_",7)) continue;

            int k=-1;
            for (int j=0;j<wn;j++) if(w[j].h==h){k=j;break;}
            if (k<0){ if(wn>=MAXW) continue; k=wn++; w[k].h=h;
                GetClassNameW(h,w[k].cls,48); GetWindowTextW(h,w[k].title,60);
                w[k].above=-1; w[k].flashes=0; w[k].first_flash=-1; }
            int ab = (i<bgz);
            if (w[k].above==0 && ab==1){ w[k].flashes++; any_flash=1;
                if (w[k].first_flash<0) w[k].first_flash=nowt(); }
            w[k].above=ab;
        }
        if (any_flash){
            double t=nowt();
            if (last_flash_t>=0 && gn<400) gaps[gn++]=(t-last_flash_t)*1000.0;
            last_flash_t=t; flash_events++;
        }
        Sleep(2);
    }

    fwprintf(out,L"\n=== backdrop z-index moved %d times; %d flash events; "
                 L"%ld foreground changes ===\n", bg_moves, flash_events, fg_count);

    if (gn){ double s=0,mn=1e9,mx=0;
        for(int i=0;i<gn;i++){s+=gaps[i]; if(gaps[i]<mn)mn=gaps[i]; if(gaps[i]>mx)mx=gaps[i];}
        fwprintf(out,L"flash interval: mean %.0fms  min %.0fms  max %.0fms  (n=%d)\n",
                 s/gn,mn,mx,gn);
        fwprintf(out,L"first 30 gaps(ms):"); 
        for(int i=0;i<gn&&i<30;i++) fwprintf(out,L" %.0f",gaps[i]);
        fwprintf(out,L"\n"); }

    fwprintf(out,L"\n--- windows that rose ABOVE the backdrop (a flash) ---\n");
    for (int i=0;i<wn;i++) if (w[i].flashes)
        fwprintf(out,L"  flashes=%-5d %-24.24ls | %.40ls\n",w[i].flashes,w[i].cls,w[i].title);

    fwprintf(out,L"\n--- foreground changes (first 40) ---\n");
    for (int i=0;i<fg_n&&i<40;i++)
        fwprintf(out,L"  %7.3f %ls\n",fg_t[i],fg_cls[i]);

    fclose(out);
    PostQuitMessage(0);
    return 0;
}

int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR cmd,int c){
    (void)a;(void)b;(void)c;
    int delay=5;
    if (cmd&&*cmd) swscanf(cmd,L"%d %d",&delay,&seconds);
    Sleep(delay*1000);
    out=_wfopen(L"C:\\Windows\\Temp\\sink.txt",L"w, ccs=UTF-8");
    if(!out) return 1;
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    fwprintf(out,L"recording %ds\n",seconds);

    HWINEVENTHOOK hk=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
        NULL,evp,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    CreateThread(NULL,0,poll_thread,NULL,0,NULL);
    MSG m; while(GetMessageW(&m,NULL,0,0)>0){ TranslateMessage(&m); DispatchMessageW(&m); }
    if(hk) UnhookWinEvent(hk);
    return 0;
}
