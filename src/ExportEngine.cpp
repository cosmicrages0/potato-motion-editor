#include "ExportEngine.h"

#include <iostream>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>

// Windows-specific _popen / _pclose live in <stdio.h> under underscore-prefixed
// names on MSVC; there is no <process.h> for the pipe variants.

ExportEngine::ExportEngine() {
    QueryPerformanceFrequency(&freq_);
}

ExportEngine::~ExportEngine() {
    End(true);
}

std::string ExportEngine::BuildFfmpegCommand() const {
    // -y                        : overwrite output without asking
    // -f rawvideo -vcodec rawvideo : input is raw uncompressed pixels
    // -s WxH                    : frame size we're feeding
    // -pix_fmt bgra             : matches DXGI_FORMAT_B8G8R8A8_UNORM byte order
    // -r fps                    : input framerate
    // -i -                      : read from stdin (that's what _popen gives us)
    // -c:v libx264              : encode with H.264
    // -pix_fmt yuv420p          : maximum player compatibility
    // -b:v Nk                   : target bitrate in kbps
    // "path"                    : output file
    //
    // Quoting: on Windows _popen invokes cmd.exe, so we double-quote paths
    // that may contain spaces. If the user's ffmpegPath itself contains a
    // space we quote it too.
    std::ostringstream oss;
    oss << "\"" << settings_.ffmpegPath << "\""
        << " -y -f rawvideo -vcodec rawvideo"
        << " -s " << settings_.width << "x" << settings_.height
        << " -pix_fmt bgra"
        << " -r " << settings_.fps
        << " -i -"
        << " -c:v libx264 -pix_fmt yuv420p"
        << " -b:v " << settings_.bitrateKbps << "k"
        << " \"" << settings_.outputPath << "\"";
    return oss.str();
}

bool ExportEngine::AllocateTextures(ID3D11Device* device) {
    if (!device) return false;

    // Render target (BGRA to match what ffmpeg expects with -pix_fmt bgra).
    D3D11_TEXTURE2D_DESC rtd = {};
    rtd.Width          = (UINT)settings_.width;
    rtd.Height         = (UINT)settings_.height;
    rtd.MipLevels      = 1;
    rtd.ArraySize      = 1;
    rtd.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtd.SampleDesc.Count = 1;
    rtd.Usage          = D3D11_USAGE_DEFAULT;
    rtd.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&rtd, nullptr, &renderTex_);
    if (FAILED(hr) || !renderTex_) {
        status_.errorMsg = "Failed to allocate export render texture";
        return false;
    }
    hr = device->CreateRenderTargetView(renderTex_, nullptr, &rtv_);
    if (FAILED(hr)) {
        status_.errorMsg = "Failed to create export RTV";
        return false;
    }
    hr = device->CreateShaderResourceView(renderTex_, nullptr, &srv_);
    if (FAILED(hr)) {
        // Non-fatal for export itself, but we like to have it for the preview.
        srv_ = nullptr;
    }

    // Staging (CPU-readable) texture, same size + format.
    D3D11_TEXTURE2D_DESC sd = rtd;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    hr = device->CreateTexture2D(&sd, nullptr, &stagingTex_);
    if (FAILED(hr) || !stagingTex_) {
        status_.errorMsg = "Failed to allocate CPU staging texture";
        return false;
    }
    return true;
}

void ExportEngine::ReleaseTextures() {
    if (srv_)        { srv_->Release();        srv_        = nullptr; }
    if (rtv_)        { rtv_->Release();        rtv_        = nullptr; }
    if (renderTex_)  { renderTex_->Release();  renderTex_  = nullptr; }
    if (stagingTex_) { stagingTex_->Release(); stagingTex_ = nullptr; }
}

bool ExportEngine::Begin(ID3D11Device* device, ID3D11DeviceContext* context, const Settings& s) {
    if (status_.active) {
        status_.errorMsg = "Export already in progress";
        return false;
    }
    if (!device || !context) {
        status_.errorMsg = "No D3D device available";
        return false;
    }

    // Reset status
    status_ = Status{};
    settings_ = s;
    // Defensive clamps so an insane UI value doesn't OOM us.
    if (settings_.width  < 16)     settings_.width  = 16;
    if (settings_.width  > 7680)   settings_.width  = 7680;   // 8K sanity cap
    if (settings_.height < 16)     settings_.height = 16;
    if (settings_.height > 4320)   settings_.height = 4320;
    if (settings_.fps    < 1)      settings_.fps    = 1;
    if (settings_.fps    > 120)    settings_.fps    = 120;
    if (settings_.bitrateKbps < 100)     settings_.bitrateKbps = 100;
    if (settings_.bitrateKbps > 200000)  settings_.bitrateKbps = 200000;
    if (settings_.totalFrames < 1) settings_.totalFrames = 1;

    device_  = device;
    context_ = context;

    if (!AllocateTextures(device_)) {
        ReleaseTextures();
        status_.error = true;
        return false;
    }

    // Spawn ffmpeg. BINARY MODE ("wb") is critical -- text mode on Windows
    // would translate any 0x0A byte in the pixel stream into 0x0D 0x0A,
    // corrupting every frame that contains a matching pixel value.
    const std::string cmd = BuildFfmpegCommand();
    std::cerr << "[Export] Launching: " << cmd << std::endl;
    ffmpegPipe_ = _popen(cmd.c_str(), "wb");
    if (!ffmpegPipe_) {
        status_.error   = true;
        status_.errorMsg = "Could not launch ffmpeg. Place ffmpeg.exe in PATH "
                           "or set the FFmpeg path in the export panel.";
        ReleaseTextures();
        return false;
    }

    status_.active      = true;
    status_.finished    = false;
    status_.error       = false;
    status_.frameIndex  = 0;
    status_.totalFrames = settings_.totalFrames;
    QueryPerformanceCounter(&startTicks_);
    return true;
}

bool ExportEngine::WriteCurrentFrame() {
    if (!status_.active)                       return false;
    if (!ffmpegPipe_)                          return false;
    if (!context_ || !renderTex_ || !stagingTex_) return false;

    // Defensive: don't write past the requested frame count.
    if (status_.frameIndex >= status_.totalFrames) {
        End(false);
        return true;
    }

    // GPU->staging copy is a full resource dance; the CopyResource is
    // asynchronous but Map with default flags (no D3D11_MAP_FLAG_DO_NOT_WAIT)
    // will block until the copy finishes -- which is what we want here.
    context_->CopyResource(stagingTex_, renderTex_);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(stagingTex_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        status_.error    = true;
        status_.errorMsg = "Failed to map staging texture for readback";
        End(true);
        return false;
    }

    // fwrite one row at a time because RowPitch may be > width*4 (D3D pads
    // rows for alignment). Writing the full mapped block would insert
    // garbage stride bytes into the video stream.
    const size_t rowBytes = (size_t)settings_.width * 4; // BGRA
    const unsigned char* base = (const unsigned char*)mapped.pData;
    for (int y = 0; y < settings_.height; ++y) {
        const unsigned char* row = base + (size_t)y * mapped.RowPitch;
        const size_t written = std::fwrite(row, 1, rowBytes, ffmpegPipe_);
        if (written != rowBytes) {
            context_->Unmap(stagingTex_, 0);
            status_.error    = true;
            status_.errorMsg = "FFmpeg pipe closed unexpectedly (encoder died?)";
            End(true);
            return false;
        }
    }

    context_->Unmap(stagingTex_, 0);

    ++status_.frameIndex;

    // Update elapsed for the ETA display.
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (freq_.QuadPart > 0) {
        status_.secondsElapsed = (double)(now.QuadPart - startTicks_.QuadPart) / (double)freq_.QuadPart;
    }

    // Finish naturally if we've written the last frame.
    if (status_.frameIndex >= status_.totalFrames) {
        End(false);
    }
    return true;
}

void ExportEngine::End(bool cancelled) {
    if (ffmpegPipe_) {
        std::fflush(ffmpegPipe_);
        // _pclose returns the child's exit code; libx264 may print stats to
        // stderr but a healthy exit is 0. We don't gate the UI on this.
        _pclose(ffmpegPipe_);
        ffmpegPipe_ = nullptr;
    }
    ReleaseTextures();

    if (status_.active) {
        status_.active   = false;
        status_.finished = !status_.error && !cancelled;
        if (cancelled && !status_.error) {
            status_.errorMsg = "Cancelled";
        }
    }
    device_  = nullptr;
    context_ = nullptr;
}
