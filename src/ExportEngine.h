#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
#include <atomic>

// -----------------------------------------------------------------------------
// ExportEngine — zero-RAM-spike MP4 exporter (Windows-native CreateProcess).
//
// The design guarantee (from SOFTWARE_SPEC.md and PROJECT_BRIEFING Section 11):
// no more than ONE frame of raw pixels lives in RAM at any moment, regardless
// of clip length or resolution. Even a 4K frame is only 3840*2160*4 = 32 MB.
//
// Task 6.1 rework: moved off _popen("wb") and onto CreateProcess + CreatePipe
// so we can (a) guarantee zero text-mode CR/LF munging on the raw BGRA byte
// stream, (b) redirect ffmpeg's stderr into a log file so failures produce a
// real diagnostic instead of the generic "encoder died?" message. The child
// still reads its input from an anonymous pipe; our side owns the WRITE
// handle and calls WriteFile per frame.
//
// How it works:
//   1. CreatePipe() -> we get hChildStdIn_Write (owned), ffmpeg gets the READ
//      end via STARTUPINFO handle inheritance.
//   2. CreateFile(logPath, GENERIC_WRITE) for ffmpeg's stderr/stdout. Whole
//      transcript survives across the export for post-mortem inspection.
//   3. CreateProcessA() launches ffmpeg with CREATE_NO_WINDOW so no cmd
//      window flashes.
//   4. Allocate two textures at export resolution:
//        - render target (GPU-only, BGRA)
//        - CPU-readable staging texture (D3D11_USAGE_STAGING)
//   5. Per frame:
//        - Rasterize the composition into the render target
//        - CopyResource -> staging
//        - Map -> WriteFile the mapped pointer straight into the pipe -> Unmap
//        No malloc, no intermediate buffer, no memcpy into a std::vector.
//   6. On Stop/Cancel: CloseHandle(stdin write) signals EOF; wait for the
//      process to exit; read the tail of the log for the error message if
//      exit code != 0.
//
// UI responsiveness note (unchanged from Task 6.0):
//   D3D11 immediate contexts are NOT thread-safe by default, and this app
//   doesn't create the device with the multithread flag. So the export runs
//   on the main thread and pumps ImGui between frames — the progress bar
//   ticks visibly even during a long export.
// -----------------------------------------------------------------------------

class ExportEngine {
public:
    // Settings the user picks in the Render Queue panel.
    struct Settings {
        int         width       = 1920;
        int         height      = 1080;
        int         fps         = 30;
        int         bitrateKbps = 10000;   // 10 Mbps default
        int         totalFrames = 300;     // fps * seconds
        std::string outputPath  = "output.mp4";
        std::string ffmpegPath  = "ffmpeg"; // just "ffmpeg" relies on PATH
    };

    // Live state observable by the UI without touching the pipe.
    struct Status {
        bool         active    = false;
        bool         finished  = false;
        bool         error     = false;
        std::string  errorMsg;
        int          frameIndex = 0;
        int          totalFrames = 0;
        double       secondsElapsed = 0.0;
    };

    ExportEngine();
    ~ExportEngine();

    // Allocates the two textures + spawns ffmpeg via _popen. Returns false
    // on any failure (bad ffmpeg path, DX allocation error, etc.) with a
    // human-readable reason in Status.errorMsg.
    bool Begin(ID3D11Device* device, ID3D11DeviceContext* context, const Settings& s);

    // Called once per frame between Begin() and End(). The caller does its
    // own rendering into GetRenderTargetView() BEFORE calling this.
    // Returns false if the pipe write failed (broken FFmpeg process).
    bool WriteCurrentFrame();

    // Close the pipe and free textures. Idempotent.
    void End(bool cancelled = false);

    // Non-owning accessor so RenderEngine can bind the RTV during export
    // to render the composition at export resolution.
    ID3D11RenderTargetView* GetRenderTargetView() const { return rtv_; }
    ID3D11ShaderResourceView* GetShaderResourceView() const { return srv_; }
    int  Width()  const { return settings_.width;  }
    int  Height() const { return settings_.height; }

    // Query current live state (thread-unsafe by design; export runs on main thread).
    const Status& GetStatus() const { return status_; }
    const Settings& GetSettings() const { return settings_; }

    // Task 5.0 diagnostic: run `<ffmpegPath> -version` and capture the first
    // line of output. Returns true if the binary was found and produced any
    // output, false otherwise. Writes the captured output (or the failure
    // reason) into `outResult` so the UI can show it verbatim.
    // This is a synchronous call that blocks for up to ~500ms; only use from
    // a button click, never per-frame.
    static bool TestFfmpegBinary(const std::string& ffmpegPath, std::string& outResult);

private:
    void ReleaseTextures();
    bool AllocateTextures(ID3D11Device* device);
    std::string BuildFfmpegCommand() const;

    // Non-owning
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Owned GPU textures at export resolution
    ID3D11Texture2D*         renderTex_ = nullptr;
    ID3D11RenderTargetView*  rtv_       = nullptr;
    ID3D11ShaderResourceView* srv_      = nullptr;
    ID3D11Texture2D*         stagingTex_ = nullptr;

    // Task 6.1: Windows-native process + pipe handles. Replaces the old
    // FILE* ffmpegPipe_ from the _popen path. All owned; released in End().
    HANDLE       childStdIn_Write_ = nullptr;   // our write end of the pipe
    HANDLE       ffmpegProcess_    = nullptr;   // ffmpeg.exe process handle
    HANDLE       ffmpegThread_     = nullptr;   // main thread of ffmpeg
    HANDLE       logFile_          = nullptr;   // ffmpeg stderr/stdout capture
    std::string  logFilePath_;                  // absolute path to the log file

    Settings settings_;
    Status   status_;
    // Wall-clock start for ETA computation (QueryPerformanceCounter units).
    LARGE_INTEGER startTicks_ = { 0 };
    LARGE_INTEGER freq_       = { 0 };
};
