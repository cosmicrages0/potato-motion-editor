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
// ExportEngine — zero-RAM-spike 4K MP4 exporter.
//
// The design guarantee (from SOFTWARE_SPEC.md and PROJECT_BRIEFING Section 11):
// no more than ONE frame of raw pixels lives in RAM at any moment, regardless
// of clip length or resolution. Even a 4K frame is only 3840*2160*4 = 32 MB.
//
// How it works:
//   1. Spawn ffmpeg.exe via _popen(..., "wb") -- BINARY mode is critical:
//      text mode would CR/LF-corrupt any 0x0A byte in the pixel stream.
//   2. Allocate exactly two textures at export resolution:
//        - render target (GPU-only, RGBA/BGRA)
//        - CPU-readable staging texture (D3D11_USAGE_STAGING)
//   3. Per frame:
//        - Rasterize the composition into the render target
//        - CopyResource -> staging
//        - Map -> fwrite -> Unmap
//        The mapped pointer is written straight into the pipe. No malloc, no
//        intermediate buffer, no memcpy into a std::vector.
//   4. On Stop/Cancel: fflush + _pclose the pipe, release the two textures.
//
// UI responsiveness note:
//   D3D11 immediate contexts are NOT thread-safe by default, and this app
//   doesn't create the device with the multithread flag. So the export runs
//   on the main thread and pumps ImGui between frames -- the progress bar
//   ticks visibly even during a long export. Task 6.5 (post-usability-pass)
//   can move this to a deferred-context worker if anyone actually needs
//   faster-than-realtime exports.
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

    // Owned OS resource: pipe to ffmpeg.exe
    FILE*    ffmpegPipe_ = nullptr;

    Settings settings_;
    Status   status_;
    // Wall-clock start for ETA computation (QueryPerformanceCounter units).
    LARGE_INTEGER startTicks_ = { 0 };
    LARGE_INTEGER freq_       = { 0 };
};
