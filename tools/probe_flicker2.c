#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_TRACK 256
#define MAX_EV    6000
#define GW_ 128
#define GH_ 72
#define MAXS 6000

typedef struct {
    HWND hwnd; wchar_t cls[64], title[80];
    int last_z, z_changes, crossed, vis_flips, rect_changes;
    int last_above_bg; BOOL last_vis; RECT last_rect; int alive;
} Track;

typedef struct { double t; wchar_t s[150]; } Ev;

static Track tr[MAX_TRACK]; static int trn;
static Ev    ev_[MAX_EV];   static int evn;
static FILE *out;

typedef struct { RECT rc; HDC mem; HBITMAP bmp, old; BYTE *bits; BYTE prev[GW_*GH_]; int hp; } Mon;
static Mon mon[8]; static int monn;
static double ts[MAXS]; static int dif[8][MAXS]; static int ns;

static BOOL CALLBACK me(HMONITOR h, HDC d, LPRECT r, LPARAM l) {
    (void)d;(void)r;(void)l;
    if (monn >= 8) return TRUE;
    MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(h, &mi);
    mon[monn++].rc = mi.rcMonitor; return TRUE;
}

static void ev(double t, const wchar_t *f, ...) {
    if (evn >= MAX_EV) return;
    Ev *e = &ev_[evn++]; e->t = t;
    va_list ap; va_start(ap, f); _vsnwprintf(e->s, 149, f, ap); va_end(ap); e->s[149]=0;
}

static Track *tf(HWND h) {
    for (int i=0;i<trn;i++) if (tr[i].hwnd==h) return &tr[i];
    if (trn>=MAX_TRACK) return NULL;
    Track *t=&tr[trn++]; memset(t,0,sizeof *t); t->hwnd=h;
    GetClassNameW(h,t->cls,64); GetWindowTextW(h,t->title,80);
    t->last_z=-1; t->last_above_bg=-1; t->last_vis=IsWindowVisible(h);
    GetWindowRect(h,&t->last_rect); return t;
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR cmd, int c) {
    (void)a;(void)b;(void)c;
    int delay = 4, secs = 12;
    if (cmd && *cmd) swscanf(cmd, L"%d %d", &delay, &secs);
    Sleep(delay * 1000);

    out = _wfopen(L"C:\\Windows\\Temp\\flicker.txt", L"w, ccs=UTF-8");
    if (!out) return 1;

    EnumDisplayMonitors(NULL,NULL,me,0);
    HDC scr = GetDC(NULL);
    BITMAPINFO bi; memset(&bi,0,sizeof bi);
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=GW_;
    bi.bmiHeader.biHeight=-GH_; bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    for (int i=0;i<monn;i++){ Mon*m=&mon[i];
        m->mem=CreateCompatibleDC(scr);
        m->bmp=CreateDIBSection(scr,&bi,DIB_RGB_COLORS,(void**)&m->bits,NULL,0);
        m->old=SelectObject(m->mem,m->bmp); SetStretchBltMode(m->mem,COLORONCOLOR);
        fwprintf(out,L"monitor %d: %ldx%ld at %ld,%ld\n",i,
            m->rc.right-m->rc.left,m->rc.bottom-m->rc.top,m->rc.left,m->rc.top); }

    LARGE_INTEGER f,t0,nw; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    HWND snap[400]; int bgz_changes=0,last_bgz=-1;

    for (;;) {
        QueryPerformanceCounter(&nw);
        double t=(double)(nw.QuadPart-t0.QuadPart)/(double)f.QuadPart;
        if (t>=secs) break;

        int n=0;
        for (HWND h=GetTopWindow(NULL); h&&n<400; h=GetWindow(h,GW_HWNDNEXT)) snap[n++]=h;
        int bgz=-1;
        for (int i=0;i<n;i++){ wchar_t cl[64]; GetClassNameW(snap[i],cl,64);
            if (!wcscmp(cl,L"mshell_Background")){ bgz=i; break; } }
        if (bgz>=0&&last_bgz>=0&&bgz!=last_bgz){ bgz_changes++;
            ev(t,L"backdrop z %d -> %d (of %d)",last_bgz,bgz,n); }
        if (bgz>=0) last_bgz=bgz;

        for (int i=0;i<n;i++){
            Track *k=tf(snap[i]); if(!k) continue; k->alive=1;
            if (k->last_z>=0&&k->last_z!=i){ k->z_changes++;
                if (k->z_changes<=150) ev(t,L"z     %-22.22ls %d -> %d",k->cls,k->last_z,i); }
            k->last_z=i;
            if (bgz>=0){ int ab=(i<bgz);
                if (k->last_above_bg>=0&&ab!=k->last_above_bg){ k->crossed++;
                    if (k->crossed<=150) ev(t,L"CROSS %-22.22ls -> %ls backdrop  z=%d bg=%d",
                        k->cls, ab?L"ABOVE":L"below", i, bgz); }
                k->last_above_bg=ab; }
            BOOL v=IsWindowVisible(snap[i]);
            if (v!=k->last_vis){ k->vis_flips++;
                if (k->vis_flips<=150) ev(t,L"VIS   %-22.22ls -> %ls",k->cls,v?L"shown":L"hidden"); }
            k->last_vis=v;
            RECT r;
            if (GetWindowRect(snap[i],&r)){
                if (memcmp(&r,&k->last_rect,sizeof r)){ k->rect_changes++;
                    if (k->rect_changes<=80) ev(t,L"RECT  %-22.22ls %ld,%ld %ldx%ld",k->cls,
                        r.left,r.top,r.right-r.left,r.bottom-r.top); }
                k->last_rect=r; } }

        if (ns<MAXS){ ts[ns]=t;
            for (int i=0;i<monn;i++){ Mon*m=&mon[i];
                StretchBlt(m->mem,0,0,GW_,GH_,scr,m->rc.left,m->rc.top,
                    m->rc.right-m->rc.left,m->rc.bottom-m->rc.top,SRCCOPY|CAPTUREBLT);
                BYTE lum[GW_*GH_];
                for (int p=0;p<GW_*GH_;p++){ BYTE*px=m->bits+p*4;
                    lum[p]=(BYTE)((px[0]*29+px[1]*150+px[2]*77)>>8); }
                int d=0;
                if (m->hp) for (int p=0;p<GW_*GH_;p++){ int x=(int)lum[p]-(int)m->prev[p]; d+=x<0?-x:x; }
                if (m->hp) d/=(GW_*GH_);
                memcpy(m->prev,lum,sizeof lum); m->hp=1; dif[i][ns]=d; }
            ns++; }
        Sleep(6);
    }

    fwprintf(out,L"\n=== %d frames over %ds; backdrop z changed %d times ===\n",ns,secs,bgz_changes);
    for (int i=0;i<monn;i++){
        int sp=0,mx=0; double sum=0;
        for (int s=1;s<ns;s++){ int d=dif[i][s]; sum+=d; if(d>mx)mx=d; if(d>=2)sp++; }
        fwprintf(out,L"\nmonitor %d: mean %.2f max %d changed-frames %d/%d\n",
                 i,sum/(ns?ns:1),mx,sp,ns);
        fwprintf(out,L"  spikes:"); int sh=0; double last=-1;
        for (int s=1;s<ns&&sh<40;s++) if (dif[i][s]>=2){
            fwprintf(out,L" %.3f(%d)",ts[s],dif[i][s]);
            if(last>=0) fwprintf(out,L"[+%.0fms]",(ts[s]-last)*1000.0);
            last=ts[s]; if(++sh%4==0) fwprintf(out,L"\n         "); }
        fwprintf(out,L"\n"); }

    fwprintf(out,L"\n--- window churn ---\n");
    for (int p=0;p<20;p++){ int bi_=-1,bs=0;
        for (int i=0;i<trn;i++){ if(tr[i].alive<0) continue;
            int s=tr[i].z_changes+tr[i].crossed*50+tr[i].vis_flips*50+tr[i].rect_changes*10;
            if (s>bs){bs=s;bi_=i;} }
        if (bi_<0) break; Track*k=&tr[bi_];
        fwprintf(out,L"  z=%-4d cross=%-4d vis=%-4d rect=%-4d %-24.24ls | %.36ls\n",
            k->z_changes,k->crossed,k->vis_flips,k->rect_changes,k->cls,k->title);
        k->alive=-1; }

    fwprintf(out,L"\n--- events (%d total, first 200) ---\n",evn);
    for (int i=0;i<evn&&i<200;i++) fwprintf(out,L"  %7.3f  %ls\n",ev_[i].t,ev_[i].s);
    fclose(out);
    ReleaseDC(NULL,scr);
    return 0;
}
