
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cmath>
#include <utility>
#include <iterator>
#include <cstdint>
#include <vector>
#include <cwctype>
#include <cstdlib>
#include "VideoDecoder.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "AudioPlayer.h"
#include "Localization.h"
#include "Log.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9";

enum : UINT {
    IDM_OPEN=100, IDM_EXIT, IDM_EXPORT,
    IDM_PLAY=200, IDM_STOP, IDM_BACK10, IDM_FWD10, IDM_MUTE,
    IDM_DLSS=300, IDM_REHOOK, IDM_VIEW_FINAL, IDM_VIEW_INPUT, IDM_VIEW_COMPARE, IDM_VIEW_MV, IDM_VIEW_DEPTH, IDM_VIEW_MASK, IDM_DEPTH_MODE,
    IDM_QUALITY_AUTO=330, IDM_QUALITY_QUALITY, IDM_QUALITY_BALANCED, IDM_QUALITY_PERFORMANCE, IDM_QUALITY_ULTRAPERF, IDM_QUALITY_DLAA,
    IDM_ASPECT_FIT=400, IDM_ASPECT_FILL, IDM_FULLSCREEN, IDM_VIDEO_ADJUSTMENTS,
    IDM_LANG_BASE=500
};

static constexpr int HK_PLAY_PAUSE = 9001;
static constexpr int HK_BACK_10   = 9002;
static constexpr int HK_FORWARD_10 = 9003;
static constexpr int HK_MUTE      = 9004;

struct AppOptions {
    uint32_t maxW=3840, maxH=2160;
    NVSDK_NGX_PerfQuality_Value quality=NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    bool qualityExplicit=false;
    std::wstring file;
};

static AppOptions ParseArgs() {
    AppOptions o; int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return o;
    for (int i=1;i<argc;++i) {
        std::wstring a=argv[i];
        if ((a==L"--quality"||a==L"-q") && i+1<argc) {
            std::wstring v=argv[++i];
            if(v==L"quality")     { o.quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;       o.qualityExplicit=true; }
            else if(v==L"ultraperf")   { o.quality=NVSDK_NGX_PerfQuality_Value_UltraPerformance; o.qualityExplicit=true; }
            else if(v==L"balanced")    { o.quality=NVSDK_NGX_PerfQuality_Value_Balanced;     o.qualityExplicit=true; }
            else if(v==L"performance") { o.quality=NVSDK_NGX_PerfQuality_Value_MaxPerf;      o.qualityExplicit=true; }
            else if(v==L"ultraperf")   { o.quality=NVSDK_NGX_PerfQuality_Value_UltraPerformance; o.qualityExplicit=true; }
            else if(v==L"dlaa")        { o.quality=NVSDK_NGX_PerfQuality_Value_DLAA;         o.qualityExplicit=true; }
        } else if (a==L"--maxres" && i+2<argc) {
            o.maxW=uint32_t(_wtoi(argv[i+1])); o.maxH=uint32_t(_wtoi(argv[i+2])); i+=2;
        } else if (a[0]!=L'-') {
            if (std::filesystem::exists(a)) o.file=a;
        }
    }
    LocalFree(argv); return o;
}

static std::wstring QualityNameW(NVSDK_NGX_PerfQuality_Value v) {
    switch(v) {
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:       return L"Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced:         return L"Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:          return L"Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return L"Ultra Perf";
    case NVSDK_NGX_PerfQuality_Value_DLAA:             return L"DLAA";
    default: return L"Auto";
    }
}

static std::wstring FormatTime(double s) {
    int h=int(s)/3600, m=(int(s)%3600)/60, sec=int(s)%60;
    wchar_t buf[32];
    if (h>0) swprintf_s(buf,L"%d:%02d:%02d",h,m,sec);
    else     swprintf_s(buf,L"%d:%02d",m,sec);
    return buf;
}

// ============================================================
// PlayerApp - single window, D3D12 directly, Dear ImGui UI
// ============================================================
class PlayerApp {
public:
    explicit PlayerApp(AppOptions o) : m_opt(std::move(o)) {}
    ~PlayerApp() { Cleanup(); }

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();

        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES}; InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
        wc.style = CS_DBLCLKS | CS_OWNDC;
        wc.lpfnWndProc = WndProcStatic;
        wc.hInstance = hi;
        wc.lpszClassName = L"DLSSPlayerImGuiV1";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(101));
        wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);

        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,FALSE);
        m_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"DLSS Image Viewer",
            WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT,CW_USEDEFAULT,
            rc.right-rc.left, rc.bottom-rc.top, nullptr, nullptr, hi, this);
        if (!m_hwnd) return false;

        // Dark titlebar + rounded corners
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark));
        DWORD corner=2; DwmSetWindowAttribute(m_hwnd, 33, &corner, sizeof(corner));

        DragAcceptFiles(m_hwnd, TRUE);
        RegisterHotKey(m_hwnd, HK_PLAY_PAUSE, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(m_hwnd, HK_MUTE, MOD_NOREPEAT, VK_VOLUME_MUTE);

        if (!m_opt.file.empty()) Load(m_opt.file);
        else InitIdleRenderer();
        return true;
    }

    bool Running() const { return m_running; }
    bool NeedsRealtimeTick() const { return m_loaded && m_playing; }
    int  TickSleepMs() const { return 2; }

    void Tick() {
        if (!m_loaded) {
            auto now = Clock::now();
            if (std::chrono::duration<double>(now - m_lastStaticPresent).count() >= 1.0/30.0) {
                if (m_renderer) m_renderer->PresentCurrent();
                m_lastStaticPresent = now;
            }
            return;
        }
        if (m_seekPending) {
            double target = m_pendingSeekSec; bool resume = m_seekResumePlaying;
            m_seekPending = false; PerformSeek(target, resume); return;
        }
        if (!m_playing && !m_seeking && m_renderer) {
            auto now = Clock::now();
            if (std::chrono::duration<double>(now - m_lastStaticPresent).count() >= 1.0/60.0) {
                if (m_isStaticImage) RenderVideoFrame(m_staticFrame, false);
                else m_renderer->PresentCurrent();
                m_lastStaticPresent = now;
            }
            return;
        }
        if (!m_playing || !m_haveNext || m_seeking) return;
        double now = Position();
        const double frameDur = 1.0 / std::max(1.0, m_decoder.FrameRate());
        bool dropped = false;
        while (m_haveNext) {
            double due = double(m_next.timestamp100ns)*1e-7;
            if (now - due <= std::max(0.085, frameDur*2.25)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if (!m_decoder.ReadNext(m_next)) { m_haveNext=false; break; }
        }
        if (dropped) { m_guides.Reset(); m_guideReset=true; m_dlssReset=true; }
        if (!m_haveNext) { m_playing=false; m_audio.Pause(true); return; }
        double due = double(m_next.timestamp100ns)*1e-7;
        if (now+0.001 < due) return;
        if (RenderVideoFrame(m_next, m_next.discontinuity || m_guideReset)) {
            m_guideReset = false; m_dlssReset = false;
            m_currentSec = due;
            auto nowTP = Clock::now();
            ++m_fpsWindowFrames;
            double elapsed = std::chrono::duration<double>(nowTP - m_fpsWindowStart).count();
            if (elapsed >= 1.0) { m_submitFps=double(m_fpsWindowFrames)/elapsed; m_fpsWindowFrames=0; m_fpsWindowStart=nowTP; }
        }
        m_haveNext = m_decoder.ReadNext(m_next);
        if (!m_haveNext) { m_playing=false; m_audio.Pause(true); }
    }

private:
    // ---- ImGui idle renderer (before any video is loaded) ----
    void InitImGui() {
        if (!ImGui::GetCurrentContext()) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ApplyImGuiStyle();
            ImGui_ImplWin32_Init(m_hwnd);
        } else {
            ImGui_ImplDX12_Shutdown();
        }
        ImGui_ImplDX12_Init(
            m_renderer->GetDevice(), 3, DXGI_FORMAT_R8G8B8A8_UNORM,
            m_renderer->GetImGuiSrvHeap(),
            m_renderer->GetImGuiSrvHeap()->GetCPUDescriptorHandleForHeapStart(),
            m_renderer->GetImGuiSrvHeap()->GetGPUDescriptorHandleForHeapStart());
        m_renderer->SetUIHook([this](ID3D12GraphicsCommandList* cmd){ RenderImGui(cmd, m_loaded); });
    }

    void InitIdleRenderer() {
        if (m_renderer) return;
        RECT cr{}; GetClientRect(m_hwnd, &cr);
        uint32_t w = std::max(1L, cr.right-cr.left);
        uint32_t h = std::max(1L, cr.bottom-cr.top);

        m_renderer = std::make_unique<D3D12Renderer>();
        if (!m_renderer->Initialize(m_hwnd, 256,256, 256,256, 16,16, NVSDK_NGX_PerfQuality_Value_UltraPerformance)) {
            m_renderer.reset(); return;
        }
        m_renderer->SetBottomMargin(0);
        m_renderer->ResizeSwapchain(w, h);
        InitImGui();
    }

    // ---- Style ----
    static void ApplyImGuiStyle() {
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding  = 0.0f; s.FrameRounding = 4.0f; s.GrabRounding = 4.0f;
        s.WindowBorderSize = 0.0f; s.FrameBorderSize = 0.0f;
        s.Colors[ImGuiCol_WindowBg]        = ImVec4(0.08f,0.08f,0.09f,0.92f);
        s.Colors[ImGuiCol_MenuBarBg]        = ImVec4(0.10f,0.10f,0.12f,1.00f);
        s.Colors[ImGuiCol_PopupBg]          = ImVec4(0.10f,0.10f,0.12f,0.98f);
        s.Colors[ImGuiCol_Header]           = ImVec4(0.22f,0.40f,0.70f,0.55f);
        s.Colors[ImGuiCol_HeaderHovered]    = ImVec4(0.26f,0.50f,0.90f,0.80f);
        s.Colors[ImGuiCol_Button]           = ImVec4(0.16f,0.18f,0.22f,1.00f);
        s.Colors[ImGuiCol_ButtonHovered]    = ImVec4(0.26f,0.50f,0.90f,1.00f);
        s.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.16f,0.40f,0.80f,1.00f);
        s.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.45f,0.65f,1.00f,1.00f);
        s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.55f,0.75f,1.00f,1.00f);
        s.Colors[ImGuiCol_FrameBg]          = ImVec4(0.14f,0.14f,0.16f,1.00f);
        s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f,0.22f,0.26f,1.00f);
        s.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.12f,0.12f,0.14f,1.00f);
    }

    // ---- Window Proc ----
    static LRESULT CALLBACK WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
        PlayerApp* a = nullptr;
        if (m == WM_NCCREATE) {
            a = static_cast<PlayerApp*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
        } else {
            a = reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (a) {
            if (ImGui::GetCurrentContext()) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.WantCaptureMouse &&
                    (m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_RBUTTONDOWN||
                     m==WM_RBUTTONUP||m==WM_MOUSEWHEEL||m==WM_MOUSEMOVE)) return 0;
                if (io.WantCaptureKeyboard &&
                    (m==WM_KEYDOWN||m==WM_KEYUP||m==WM_SYSKEYDOWN||m==WM_SYSKEYUP||m==WM_CHAR)) return 0;
            }
            return a->WndProc(h, m, w, l);
        }
        return DefWindowProcW(h, m, w, l);
    }

    LRESULT WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps);
            if (!m_loaded && m_renderer) m_renderer->PresentCurrent();
            EndPaint(h,&ps);
            return 0;
        }
        case WM_DESTROY: m_running=false; PostQuitMessage(0); return 0;
        case WM_CLOSE:   DestroyWindow(h); return 0;
        case WM_SIZE: {
            UINT cw=LOWORD(l), ch=HIWORD(l);
            if (cw>0 && ch>0) {
                D3D12Renderer* rend = m_renderer.get();
                if (rend) {
                    if (ImGui::GetCurrentContext()) ImGui_ImplDX12_InvalidateDeviceObjects();
                    rend->ResizeSwapchain(cw, ch);
                    if (ImGui::GetCurrentContext()) ImGui_ImplDX12_CreateDeviceObjects();
                    if (!m_playing && rend->PresentCurrent()) {}
                }
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP d=reinterpret_cast<HDROP>(w);
            wchar_t p[32768]{};
            if (DragQueryFileW(d,0,p,UINT(std::size(p)))) Load(p);
            DragFinish(d); return 0;
        }
        case WM_MOUSEWHEEL: {
            if (m_loaded) {
                float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;
                m_volume=std::clamp(m_volume+step,0.0f,1.0f);
                m_audio.SetVolume(m_muted?0.0f:m_volume);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: ToggleFullscreen(); return 0;
        case WM_COMMAND: HandleCommand(LOWORD(w)); return 0;
        case WM_HOTKEY:  HandleHotkey(int(w)); return 0;
        case WM_KEYDOWN: {
            if ((GetKeyState(VK_CONTROL)&0x8000) && w=='O') { OpenFromDialog(); return 0; }
            if (w==VK_SPACE)  { TogglePause(); return 0; }
            if (w==VK_LEFT)   { RequestSeek(Position()-10); return 0; }
            if (w==VK_RIGHT)  { RequestSeek(Position()+10); return 0; }
            if (w==VK_F11)    { ToggleFullscreen(); return 0; }
            if (w==VK_F6)     { Rehook(); return 0; }
            if (w=='D')       { ToggleDLSS(); return 0; }
            if (w=='M')       { ToggleMute(); return 0; }
            if (w=='G')       { ToggleDepthMode(); return 0; }
            if (w=='1') { SetDebug(D3D12Renderer::DebugView::Final); return 0; }
            if (w=='2') { SetDebug(D3D12Renderer::DebugView::Input); return 0; }
            if (w=='3') { SetDebug(D3D12Renderer::DebugView::MotionVectors); return 0; }
            if (w=='4') { SetDebug(D3D12Renderer::DebugView::Depth); return 0; }
            if (w=='5') { SetDebug(D3D12Renderer::DebugView::BiasMask); return 0; }
            if (w==VK_ESCAPE && m_fullscreen) { ToggleFullscreen(); return 0; }
            break;
        }
        }
        return DefWindowProcW(h, m, w, l);
    }

    // ---- Load / Unload ----
    bool Load(const std::wstring& path) {
        if (path.empty()) return false;
        Unload();

        if (!m_decoder.Open(path)) {
            MessageBoxW(m_hwnd, L"Could not open video file.", L"DLSS Image Viewer", MB_ICONERROR);
            InitIdleRenderer(); return false;
        }

        RECT cr{}; GetClientRect(m_hwnd, &cr);
        uint32_t wW = uint32_t(std::max(1L, cr.right-cr.left));
        uint32_t wH = uint32_t(std::max(1L, cr.bottom-cr.top));
        uint32_t srcW = m_decoder.Width(), srcH = m_decoder.Height();
        
        // Calculate max scale factor that fits within maxW and maxH while preserving aspect ratio
        double scaleW = double(m_opt.maxW) / std::max(1u, srcW);
        double scaleH = double(m_opt.maxH) / std::max(1u, srcH);
        double scale = std::min({ 2.0, scaleW, scaleH }); 
        uint32_t ow = uint32_t(srcW * scale);
        uint32_t oh = uint32_t(srcH * scale);
        if (!m_opt.qualityExplicit) m_activeQuality = NVSDK_NGX_PerfQuality_Value_UltraPerformance;

        m_renderer = std::make_unique<D3D12Renderer>();
        auto [guideW, guideH] = TemporalGuideGenerator::AnalysisGrid(srcW, srcH, m_decoder.FrameRate());
        if (!m_renderer->Initialize(m_hwnd, srcW, srcH, ow, oh, guideW, guideH, m_activeQuality)) {
            MessageBoxW(m_hwnd, L"D3D12 Renderer init failed.", L"DLSS Image Viewer", MB_ICONERROR);
            m_renderer.reset(); m_decoder.Close(); InvalidateRect(m_hwnd,nullptr,FALSE); return false;
        }

        double dar = (srcH>0) ? double(srcW)/srcH : 16.0/9.0;
        m_dar = dar; m_renderer->SetDAR(dar);
        m_renderer->SetBottomMargin(72); // Reserve 72px for the UI bar at the bottom
        m_renderer->ResizeSwapchain(wW, wH);
        m_renderer->SetColorSettings(m_colorSettings);

        // Init or re-init ImGui on this renderer
        if (!ImGui::GetCurrentContext()) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ApplyImGuiStyle();
            ImGui_ImplWin32_Init(m_hwnd);
        }
        ImGui_ImplDX12_Init(
            m_renderer->GetDevice(), 3, DXGI_FORMAT_R8G8B8A8_UNORM,
            m_renderer->GetImGuiSrvHeap(),
            m_renderer->GetImGuiSrvHeap()->GetCPUDescriptorHandleForHeapStart(),
            m_renderer->GetImGuiSrvHeap()->GetGPUDescriptorHandleForHeapStart());

        m_renderer->SetUIHook([this](ID3D12GraphicsCommandList* cmd){ RenderImGui(cmd, true); });

        VideoFrame first;
        if (!m_decoder.ReadNext(first)) {
            MessageBoxW(m_hwnd, L"Could not read first frame.", L"DLSS Player", MB_ICONERROR);
            Unload(); InitIdleRenderer(); return false;
        }
        m_guides.Reset(); m_guideReset=true; m_dlssReset=true; m_lastRenderedTs=-1;
        RenderVideoFrame(first, true);
        m_currentSec = double(first.timestamp100ns)*1e-7;
        m_haveNext = m_decoder.ReadNext(m_next);
        if (!m_haveNext) { m_isStaticImage = true; m_staticFrame = first; } else { m_isStaticImage = false; }
        m_audio.Start(path, m_currentSec);
        m_audio.SetVolume(m_muted?0.0f:m_volume);
        if (m_haveNext) {
            m_playing=true; m_playStartSec=m_currentSec; m_playStart=Clock::now();
        } else {
            m_playing=false;
        }
        m_loaded=true; m_path=path; m_droppedFrames=0;
        m_seekPending=false; m_seeking=false;
        m_fpsWindowStart=Clock::now(); m_fpsWindowFrames=0; m_submitFps=0.0;
        UpdateTitle();
        return true;
    }

    void Unload() {
        m_seekPending=false; m_seeking=false;
        m_audio.Stop();
        if (m_renderer) {
            m_renderer->WaitGPU();
            if (ImGui::GetCurrentContext()) ImGui_ImplDX12_Shutdown();
            m_renderer.reset();
        }
        m_decoder.Close(); m_guides.Reset();
        m_haveNext=false; m_next=VideoFrame{};
        m_loaded=false; m_playing=false; m_isStaticImage=false;
        m_currentSec=0; m_lastRenderedTs=-1; m_path.clear();
        UpdateTitle();
    }

    void Cleanup() {
        Unload();
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd=nullptr; }
    }

    // ---- Rendering ----
    bool RenderVideoFrame(const VideoFrame& f, bool resetGuide) {
        if (!m_renderer) return false;
        GuideFrame g;
        if (!m_guides.Generate(f.bgra.data(), m_decoder.Width(), m_decoder.Height(),
                               m_renderer->DLSSInputW(), m_renderer->DLSSInputH(),
                               m_decoder.FrameRate(), resetGuide, g)) return false;
        return m_renderer->RenderFrame(f.bgra.data(), f.bgra.size(),
            g.guideGridRGBA32F.data(), g.guideGridRGBA32F.size()*sizeof(float),
            g.gridW, g.gridH, resetGuide||m_dlssReset,
            float(1000.0/std::max(1.0,m_decoder.FrameRate())));
    }

    void RenderImGui(ID3D12GraphicsCommandList* cmd, bool hasVideo) {
        if (!ImGui::GetCurrentContext()) return;
        D3D12Renderer* rend = m_renderer.get();
        if (!rend) return;

        RECT cr{}; GetClientRect(m_hwnd, &cr);
        float wW = float(cr.right-cr.left);
        float wH = float(cr.bottom-cr.top);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().DisplaySize = ImVec2(wW, wH);
        ImGui::NewFrame();

        if (hasVideo && rend && rend->GetDebugView() == D3D12Renderer::DebugView::SplitCompare) {
            auto [vp, sc] = rend->CalcLetterboxViewportScissor();
            float renderH = std::max(1.0f, wH > 40.0f ? wH - 40.0f : 1.0f);
            
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                if (mousePos.y < renderH) {
                    float localX = mousePos.x - vp.TopLeftX;
                    rend->SetSplitRatio(localX / std::max(1.0f, vp.Width));
                }
            }
            
            float lineX = vp.TopLeftX + vp.Width * rend->GetSplitRatio();
            ImGui::GetBackgroundDrawList()->AddLine(ImVec2(lineX, vp.TopLeftY), ImVec2(lineX, vp.TopLeftY + vp.Height), IM_COL32(255, 165, 0, 255), 3.0f);
        }

        DrawMenuBar(hasVideo);
        DrawControlBar(wW, wH, hasVideo);
        DrawLogWindow();

        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { rend->GetImGuiSrvHeap() };
        cmd->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
        // Restore (no heaps bound, renderer will rebind its own next frame)
        cmd->SetDescriptorHeaps(0, nullptr);
    }

    
    void DrawLogWindow() {
        if (!m_showLog) return;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 550.0f - 10.0f, 40.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(550.0f, 400.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Log Window", &m_showLog)) {
            auto history = Log::Get().GetHistory();
            for (const auto& s : history) {
                ImGui::TextUnformatted(s.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::End();
    }

    void DrawMenuBar(bool hasVideo) {
        if (!ImGui::BeginMainMenuBar()) return;

        if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Show Log", nullptr, &m_showLog);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenFromDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Export Image", "Ctrl+S")) { PostMessageW(m_hwnd, WM_COMMAND, IDM_EXPORT, 0); }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) DestroyWindow(m_hwnd);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Playback")) {
            bool canPlay = hasVideo;
            if (ImGui::MenuItem(m_playing?"Pause":"Play", "Space", false, canPlay)) PostMessageW(m_hwnd, WM_COMMAND, IDM_PLAY, 0);
            if (ImGui::MenuItem("Stop", nullptr, false, canPlay)) PostMessageW(m_hwnd, WM_COMMAND, IDM_STOP, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Back 10s", "Left", false, canPlay)) PostMessageW(m_hwnd, WM_COMMAND, IDM_BACK10, 0);
            if (ImGui::MenuItem("Forward 10s", "Right", false, canPlay)) PostMessageW(m_hwnd, WM_COMMAND, IDM_FWD10, 0);
            ImGui::Separator();
            if (ImGui::MenuItem(m_muted?"Unmute":"Mute", "M")) ToggleMute();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Video")) {
            if (ImGui::MenuItem("Aspect Fit",  nullptr, !m_fill)) { m_fill=false; UpdateDAR(); }
            if (ImGui::MenuItem("Aspect Fill", nullptr,  m_fill)) { m_fill=true;  UpdateDAR(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Fullscreen", "F11", m_fullscreen)) ToggleFullscreen();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("DLSS")) {
            bool dlssOn = m_renderer && m_renderer->DLSSEnabled();
            if (ImGui::MenuItem("Toggle DLSS", "D", dlssOn, hasVideo)) PostMessageW(m_hwnd, WM_COMMAND, IDM_DLSS, 0);
            ImGui::Separator();
            static const struct { const char* label; NVSDK_NGX_PerfQuality_Value val; UINT id; } kModes[] = {
                {"Quality",      NVSDK_NGX_PerfQuality_Value_MaxQuality, IDM_QUALITY_QUALITY},
                {"Balanced",     NVSDK_NGX_PerfQuality_Value_Balanced, IDM_QUALITY_BALANCED},
                {"Performance",  NVSDK_NGX_PerfQuality_Value_MaxPerf, IDM_QUALITY_PERFORMANCE},
                {"Ultra Perf",   NVSDK_NGX_PerfQuality_Value_UltraPerformance, IDM_QUALITY_ULTRAPERF},
                {"DLAA",         NVSDK_NGX_PerfQuality_Value_DLAA, IDM_QUALITY_DLAA},
            };
            for (auto& kv : kModes)
                if (ImGui::MenuItem(kv.label, nullptr, m_activeQuality==kv.val, hasVideo))
                    PostMessageW(m_hwnd, WM_COMMAND, kv.id, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Re-hook NGX", "F6", false, hasVideo)) PostMessageW(m_hwnd, WM_COMMAND, IDM_REHOOK, 0);
            ImGui::Separator();
            if (hasVideo && m_renderer) {
                static const struct { const char* label; D3D12Renderer::DebugView view; UINT id; } kViews[] = {
                    {"Final",        D3D12Renderer::DebugView::Final, IDM_VIEW_FINAL},
                    {"Input",        D3D12Renderer::DebugView::Input, IDM_VIEW_INPUT},
                    {"Split Compare", D3D12Renderer::DebugView::SplitCompare, IDM_VIEW_COMPARE},
                    {"Motion Vectors", D3D12Renderer::DebugView::MotionVectors, IDM_VIEW_MV},
                    {"Depth",        D3D12Renderer::DebugView::Depth, IDM_VIEW_DEPTH},
                    {"Bias Mask",    D3D12Renderer::DebugView::BiasMask, IDM_VIEW_MASK},
                };
                for (auto& kv : kViews)
                    if (ImGui::MenuItem(kv.label, nullptr, m_renderer->GetDebugView()==kv.view))
                        PostMessageW(m_hwnd, WM_COMMAND, kv.id, 0);
            }
            ImGui::EndMenu();
        }

        if (hasVideo && m_renderer) {
            char st[192];
            snprintf(st, sizeof(st), "  %dx%d -> %dx%d | %s | %.0f fps  ",
                m_decoder.NativeWidth(), m_decoder.NativeHeight(),
                m_renderer->OutputW(), m_renderer->OutputH(),
                m_renderer->DLSSEnabled() ? "DLSS ON" : "DLSS OFF",
                m_submitFps);
            float tw = ImGui::CalcTextSize(st).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - tw - 8.0f);
            ImGui::TextDisabled("%s", st);
        }
        ImGui::EndMainMenuBar();
    }

    void DrawControlBar(float wW, float wH, bool hasVideo) {
        if (!hasVideo) {
            // Idle splash â€” full window
            ImGui::SetNextWindowPos(ImVec2(0,0));
            ImGui::SetNextWindowSize(ImVec2(wW, wH));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f,0.07f,0.08f,1.0f));
            ImGui::Begin("##idle", nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoSavedSettings);
            float cx = wW*0.5f, cy = wH*0.5f;
            const char* h1 = "DLSS Image Viewer";
            const char* h2 = "Drop a video file here, or use  File > Open";
            ImGui::SetCursorPos(ImVec2(cx - ImGui::CalcTextSize(h1).x*0.5f, cy - 32.0f));
            ImGui::TextColored(ImVec4(0.88f,0.90f,1.0f,1.0f), "%s", h1);
            ImGui::SetCursorPos(ImVec2(cx - ImGui::CalcTextSize(h2).x*0.5f, cy + 4.0f));
            ImGui::TextDisabled("%s", h2);
            ImGui::SetCursorPos(ImVec2(cx - 80.0f, cy + 42.0f));
            if (ImGui::Button("Open File...", ImVec2(160,34))) PostMessageW(m_hwnd, WM_COMMAND, IDM_OPEN, 0);
            ImGui::End();
            ImGui::PopStyleColor();
            return;
        }

        // Bottom control overlay
        const float barH = 72.0f;
        ImGui::SetNextWindowPos(ImVec2(0, wH - barH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(wW, barH), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f,0.06f,0.07f,0.88f));
        ImGui::Begin("##controls", nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoSavedSettings);

        const float padX = 10.0f;

        // Seek bar
        double dur = m_decoder.DurationSeconds();
        float progress = (dur>0) ? float(Position()/dur) : 0.0f;
        ImGui::SetCursorPos(ImVec2(padX, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 12.0f);
        ImGui::SetNextItemWidth(wW - padX*2.0f);
        if (ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f, "")) {
            if (ImGui::IsItemActive()) RequestSeek(double(progress)*dur, m_playing);
        }
        ImGui::PopStyleVar();

        // Buttons row
        // Vertically center the buttons in the remaining space of the bar
        float buttonY = 40.0f;
        ImGui::SetCursorPos(ImVec2(padX, buttonY));

        if (ImGui::Button(m_playing ? " Pause " : "  Play ")) PostMessageW(m_hwnd, WM_COMMAND, IDM_PLAY, 0);
        ImGui::SameLine();
        if (ImGui::Button(" Stop "))   PostMessageW(m_hwnd, WM_COMMAND, IDM_STOP, 0);
        ImGui::SameLine();
        if (ImGui::Button(" <<10 "))   RequestSeek(Position()-10);
        ImGui::SameLine();
        if (ImGui::Button(" +10>> "))  RequestSeek(Position()+10);
        ImGui::SameLine();
        if (ImGui::Button(m_muted?" Unmute ":" Mute ")) ToggleMute();
        ImGui::SameLine();

        // Volume slider
        ImGui::SetNextItemWidth(70.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 8.0f);
        if (ImGui::SliderFloat("##vol", &m_volume, 0.0f, 1.0f, "")) {
            m_muted = false; m_audio.SetVolume(m_volume);
        }
        ImGui::PopStyleVar();
        ImGui::SameLine();

        // Timestamp
        std::wstring wp = FormatTime(Position()), wd = FormatTime(dur);
        char tbuf[64]; snprintf(tbuf, sizeof(tbuf), "%ls / %ls", wp.c_str(), wd.c_str());
        ImGui::SetCursorPosY(buttonY + 2.0f);
        ImGui::TextDisabled("%s", tbuf);
        
        // Right Side Controls
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 620.0f);
        ImGui::SetCursorPosY(buttonY);
        
        if (ImGui::Button(m_showLog ? " Hide Log " : " Show Log ")) {
            m_showLog = !m_showLog;
        }
        
        ImGui::SameLine();
        bool isCompare = m_renderer && m_renderer->GetDebugView() == D3D12Renderer::DebugView::SplitCompare;
        if (isCompare) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.1f, 1.0f));
        }
        if (ImGui::Button(" Compare ")) {
            if (m_renderer) {
                PostMessageW(m_hwnd, WM_COMMAND, isCompare ? IDM_VIEW_FINAL : IDM_VIEW_COMPARE, 0);
            }
        }
        if (isCompare) {
            ImGui::PopStyleColor(3);
        }
        
        ImGui::SameLine();
        bool dlssOn = m_renderer && m_renderer->DLSSEnabled();
        if (dlssOn) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.3f, 0.7f, 1.0f));
        }
        if (ImGui::Button(dlssOn ? " DLSS: ON " : " DLSS: OFF ")) {
            PostMessageW(m_hwnd, WM_COMMAND, IDM_DLSS, 0);
        }
        if (dlssOn) {
            ImGui::PopStyleColor(3);
        }
        
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        const char* preview = "Auto";
        switch (m_activeQuality) {
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: preview = "Quality"; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced: preview = "Balanced"; break;
            case NVSDK_NGX_PerfQuality_Value_MaxPerf: preview = "Performance"; break;
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance: preview = "Ultra Perf"; break;
            case NVSDK_NGX_PerfQuality_Value_DLAA: preview = "DLAA"; break;
        }
        if (ImGui::BeginCombo("##dlss_quality", preview)) {
            static const struct { const char* label; NVSDK_NGX_PerfQuality_Value val; UINT id; } kModes[] = {
                {"Quality",      NVSDK_NGX_PerfQuality_Value_MaxQuality, IDM_QUALITY_QUALITY},
                {"Balanced",     NVSDK_NGX_PerfQuality_Value_Balanced, IDM_QUALITY_BALANCED},
                {"Performance",  NVSDK_NGX_PerfQuality_Value_MaxPerf, IDM_QUALITY_PERFORMANCE},
                {"Ultra Perf",   NVSDK_NGX_PerfQuality_Value_UltraPerformance, IDM_QUALITY_ULTRAPERF},
                {"DLAA",         NVSDK_NGX_PerfQuality_Value_DLAA, IDM_QUALITY_DLAA},
            };
            for (auto& kv : kModes) {
                bool is_selected = (m_activeQuality == kv.val);
                if (ImGui::Selectable(kv.label, is_selected)) {
                    PostMessageW(m_hwnd, WM_COMMAND, kv.id, 0);
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        D3D12Renderer::DebugView curView = m_renderer ? m_renderer->GetDebugView() : D3D12Renderer::DebugView::Final;
        const char* viewPreview = "Final";
        switch (curView) {
            case D3D12Renderer::DebugView::Final: viewPreview = "Final"; break;
            case D3D12Renderer::DebugView::Input: viewPreview = "Input"; break;
            case D3D12Renderer::DebugView::SplitCompare: viewPreview = "Split Compare"; break;
            case D3D12Renderer::DebugView::MotionVectors: viewPreview = "Motion Vectors"; break;
            case D3D12Renderer::DebugView::Depth: viewPreview = "Depth"; break;
            case D3D12Renderer::DebugView::BiasMask: viewPreview = "Bias Mask"; break;
        }
        if (ImGui::BeginCombo("##debug_view", viewPreview)) {
            static const struct { const char* label; D3D12Renderer::DebugView view; UINT id; } kViews[] = {
                {"Final",        D3D12Renderer::DebugView::Final, IDM_VIEW_FINAL},
                {"Input",        D3D12Renderer::DebugView::Input, IDM_VIEW_INPUT},
                {"Split Compare", D3D12Renderer::DebugView::SplitCompare, IDM_VIEW_COMPARE},
                {"Motion Vectors", D3D12Renderer::DebugView::MotionVectors, IDM_VIEW_MV},
                {"Depth",        D3D12Renderer::DebugView::Depth, IDM_VIEW_DEPTH},
                {"Bias Mask",    D3D12Renderer::DebugView::BiasMask, IDM_VIEW_MASK},
            };
            for (auto& kv : kViews) {
                bool is_selected = (curView == kv.view);
                if (ImGui::Selectable(kv.label, is_selected)) {
                    PostMessageW(m_hwnd, WM_COMMAND, kv.id, 0);
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        
        ImGui::SameLine();
        if (ImGui::Button(" Export ")) {
            PostMessageW(m_hwnd, WM_COMMAND, IDM_EXPORT, 0);
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ---- Playback ----
    double Position() const {
        if (!m_playing) return m_currentSec;
        return m_playStartSec + std::chrono::duration<double>(Clock::now()-m_playStart).count();
    }

    void TogglePause() {
        if (!m_loaded) return;
        if (m_playing) { m_currentSec=Position(); m_playing=false; m_audio.Pause(true); }
        else { m_playStartSec=m_currentSec; m_playStart=Clock::now(); m_playing=true; m_audio.Pause(false); }
    }

    void StopPlayback() {
        if (!m_loaded) return;
        m_playing=false; m_audio.Pause(true); RequestSeek(0.0, false);
    }

    void RequestSeek(double sec, bool resume=false) {
        if (!m_loaded) return;
        sec = std::clamp(sec, 0.0, m_decoder.DurationSeconds());
        m_pendingSeekSec=sec; m_seekResumePlaying=resume||m_playing;
        m_playing=false; m_seekPending=true;
    }

    void PerformSeek(double sec, bool resume) {
        if (!m_loaded||!m_renderer) return;
        m_seeking=true; m_audio.Pause(true);
        m_decoder.SeekSeconds(sec);
        m_guides.Reset(); m_guideReset=true; m_dlssReset=true; m_lastRenderedTs=-1;
        VideoFrame f;
        if (m_decoder.ReadNext(f)) {
            RenderVideoFrame(f, true);
            m_currentSec=double(f.timestamp100ns)*1e-7;
            m_haveNext=m_decoder.ReadNext(m_next);
            if (!m_haveNext) { m_isStaticImage = true; m_staticFrame = f; } else { m_isStaticImage = false; }
        }
        m_seeking=false;
        if (resume && m_haveNext) { m_playStartSec=m_currentSec; m_playStart=Clock::now(); m_playing=true; m_audio.Seek(m_currentSec); m_audio.Pause(false); }
        else { m_playing=false; m_renderer->PresentCurrent(); }
    }

    void Export() {
        if (!m_renderer) return;
        CreateDirectoryW(L"exports", NULL);
        SYSTEMTIME st{}; GetLocalTime(&st);
        wchar_t buf[256];
        swprintf(buf, 256, L"exports\\DLSS_Export_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (m_renderer->ExportImage(buf)) {
            int sn = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL); std::string narrow(sn, 0); WideCharToMultiByte(CP_UTF8, 0, buf, -1, &narrow[0], sn, NULL, NULL); LOG("Started background export to " << narrow);
        }
    }

    void ToggleMute() { m_muted=!m_muted; m_audio.SetVolume(m_muted?0.0f:m_volume); }

    void ToggleDLSS() {
        if (!m_renderer) return;
        m_renderer->SetDLSS(!m_renderer->DLSSEnabled()); m_dlssReset=true;
        if (!m_playing) m_renderer->PresentCurrent();
    }

    void Rehook() { if (m_renderer) { m_renderer->RequestDLSSRecreate(); m_dlssReset=true; } }

    void SetDebug(D3D12Renderer::DebugView v) {
        if (m_renderer) { m_renderer->SetDebugView(v); if (!m_playing) m_renderer->PresentCurrent(); }
    }

    void ToggleFullscreen() {
        if (!m_fullscreen) {
            m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);
            GetWindowRect(m_hwnd,&m_savedRect);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&mi);
            SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU));
            SetWindowPos(m_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,
                mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
            m_fullscreen=true;
        } else {
            SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle);
            SetWindowPos(m_hwnd,nullptr,m_savedRect.left,m_savedRect.top,
                m_savedRect.right-m_savedRect.left,m_savedRect.bottom-m_savedRect.top,SWP_NOZORDER|SWP_FRAMECHANGED);
            m_fullscreen=false;
        }
    }

    void ToggleDepthMode() {
        auto n = m_guides.GetDepthMode()==TemporalGuideGenerator::DepthMode::Estimated
            ? TemporalGuideGenerator::DepthMode::Flat : TemporalGuideGenerator::DepthMode::Estimated;
        m_guides.SetDepthMode(n); m_guideReset=true; m_dlssReset=true;
    }

    void SetQualityMode(bool automatic, NVSDK_NGX_PerfQuality_Value q) {
        m_opt.qualityExplicit=!automatic; m_opt.quality=q; m_activeQuality=q;
        if (m_loaded&&!m_path.empty()) { double k=Position(); bool wp=m_playing; std::wstring p=m_path; if(Load(p)) RequestSeek(k,wp); }
    }

    void UpdateDAR() {
        if (!m_renderer||!m_loaded) return;
        double dar;
        if (m_fill) {
            RECT cr{}; GetClientRect(m_hwnd,&cr);
            dar=(cr.bottom>cr.top)?double(cr.right-cr.left)/double(cr.bottom-cr.top):16.0/9.0;
        } else {
            dar=(m_decoder.NativeHeight()>0)?double(m_decoder.NativeWidth())/m_decoder.NativeHeight():16.0/9.0;
        }
        m_dar=dar; m_renderer->SetDAR(dar);
    }

    void OpenFromDialog() {
        wchar_t buf[32768]{}; buf[0]=0;
        OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=m_hwnd;
        std::wstring filter=std::wstring(L"Video Files\0")+kVideoPatterns+L"\0"+L"All Files\0*.*\0\0";
        ofn.lpstrFilter=filter.c_str(); ofn.lpstrFile=buf; ofn.nMaxFile=DWORD(std::size(buf));
        ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) Load(buf);
    }

    void HandleCommand(UINT id) {
        switch(id) {
        case IDM_OPEN: OpenFromDialog(); break; case IDM_EXIT: DestroyWindow(m_hwnd); break; case IDM_EXPORT: Export(); break;
        case IDM_PLAY: TogglePause(); break; case IDM_STOP: StopPlayback(); break;
        case IDM_BACK10: RequestSeek(Position()-10); break; case IDM_FWD10: RequestSeek(Position()+10); break;
        case IDM_MUTE: ToggleMute(); break; case IDM_DLSS: ToggleDLSS(); break;
        case IDM_REHOOK: Rehook(); break; case IDM_FULLSCREEN: ToggleFullscreen(); break;
        case IDM_ASPECT_FIT: m_fill=false; UpdateDAR(); break;
        case IDM_ASPECT_FILL: m_fill=true; UpdateDAR(); break;
        case IDM_DEPTH_MODE: ToggleDepthMode(); break;
        case IDM_VIEW_FINAL: SetDebug(D3D12Renderer::DebugView::Final); break;
        case IDM_VIEW_INPUT: SetDebug(D3D12Renderer::DebugView::Input); break;
        case IDM_VIEW_COMPARE: SetDebug(D3D12Renderer::DebugView::SplitCompare); break;
        case IDM_VIEW_MV: SetDebug(D3D12Renderer::DebugView::MotionVectors); break;
        case IDM_VIEW_DEPTH: SetDebug(D3D12Renderer::DebugView::Depth); break;
        case IDM_VIEW_MASK: SetDebug(D3D12Renderer::DebugView::BiasMask); break;
        case IDM_QUALITY_AUTO:        SetQualityMode(true,NVSDK_NGX_PerfQuality_Value_MaxQuality); break;
        case IDM_QUALITY_QUALITY:     SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxQuality); break;
        case IDM_QUALITY_BALANCED:    SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_Balanced); break;
        case IDM_QUALITY_PERFORMANCE: SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxPerf); break;
        case IDM_QUALITY_ULTRAPERF:   SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_UltraPerformance); break;
        case IDM_QUALITY_DLAA:        SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_DLAA); break;
        }
    }

    void HandleHotkey(int id) {
        if (id==HK_PLAY_PAUSE) TogglePause();
        else if (id==HK_MUTE) ToggleMute();
        else if (id==HK_BACK_10) RequestSeek(Position()-10);
        else if (id==HK_FORWARD_10) RequestSeek(Position()+10);
    }

    void UpdateTitle() {
        if (!m_loaded) { SetWindowTextW(m_hwnd,L"DLSS Image Viewer"); return; }
        std::wstringstream s;
        s<<L"DLSS Image Viewer | "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();
        if(m_renderer) s<<L" | "<<QualityNameW(m_activeQuality)<<L" | "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH();
        SetWindowTextW(m_hwnd,s.str().c_str());
    }

    AppOptions m_opt; Localizer m_loc; HWND m_hwnd=nullptr;
    D3D12Renderer::ColorSettings m_colorSettings{};
    NVSDK_NGX_PerfQuality_Value m_activeQuality=NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false;
    bool m_fill=false,m_fullscreen=false,m_muted=false;
    bool m_seekPending=false,m_seekResumePlaying=false,m_seeking=false;
    bool m_showLog=false;
    bool m_guideReset=true,m_dlssReset=true;
    LONG m_savedStyle=0; RECT m_savedRect{};
    double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_pendingSeekSec=0;
    float m_volume=1.0f;
    int64_t m_lastRenderedTs=-1; uint64_t m_droppedFrames=0;
    double m_submitFps=0.0; uint64_t m_fpsWindowFrames=0;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now();
    Clock::time_point m_lastStaticPresent=Clock::now();
    std::wstring m_path;
    VideoDecoder m_decoder; VideoFrame m_next;
    VideoFrame m_staticFrame; bool m_isStaticImage = false;
    std::unique_ptr<D3D12Renderer> m_renderer;
    // idle renderer removed
    TemporalGuideGenerator m_guides; AudioPlayer m_audio;
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;
    if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}
    PlayerApp app(ParseArgs());
    if(!app.Create(hi)){MFShutdown();CoUninitialize();return 1;}
    // Show idle renderer right away
    MSG msg{};
    while(app.Running()){
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT)goto done;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(!app.Running())break;
        app.Tick();
        if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());
        else WaitMessage();
    }
done:
    MFShutdown();CoUninitialize();return 0;
}
