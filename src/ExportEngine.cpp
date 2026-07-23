#include "ExportEngine.h"

#include <iostream>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

// Windows-specific _popen / _pclose live in <stdio.h> under underscore-prefixed
// names on MSVC; there is no <process.h> for the pipe variants. Kept for
// TestFfmpegBinary (safe — text-mode read, no pixel data). Begin / End /
// WriteCurrentFrame moved to CreateProcess + CreatePipe in Task 6.1.

// Task 6.1: Windows helpers — derive the log file path from the output MP4
// path so the log always sits next to the artifact it describes. Falls back
// to CWD if the caller passes a bare filename.
static std::string DeriveLogPath(const std::string& outputPath) {
    // Find the last '/' or '\'; everything before it is the directory.
    size_t sepPos = outputPath.find_last_of("/\\");
    if (sepPos == std::string::npos) return "ffmpeg_last.log";
    return outputPath.substr(0, sepPos + 1) + "ffmpeg_last.log";
}

// Task 6.1: read the last N bytes of a text file and return them as a
// std::string, trimming the leading incomplete line. Used to surface the
// most-recent ffmpeg error into status_.errorMsg.
static std::string ReadLogTail(const std::string& path, size_t maxBytes) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    LARGE_INTEGER off{};
    if ((LONGLONG)maxBytes < sz.QuadPart) {
        off.QuadPart = sz.QuadPart - (LONGLONG)maxBytes;
        SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    }
    std::string out;
    out.resize((sz.QuadPart > (LONGLONG)maxBytes) ? maxBytes : (size_t)sz.QuadPart);
    DWORD read = 0;
    if (!out.empty()) ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    out.resize(read);
    CloseHandle(h);
    // If we truncated, drop the partial first line so the tail starts clean.
    if (sz.QuadPart > (LONGLONG)maxBytes) {
        auto nl = out.find('\n');
        if (nl != std::string::npos && nl + 1 < out.size()) {
            out.erase(0, nl + 1);
        }
    }
    return out;
}

ExportEngine::ExportEngine() {
    QueryPerformanceFrequency(&freq_);
}

// Task 5.0: one-click diagnostic. Runs `<ffmpegPath> -version` via _popen in
// TEXT mode this time (we're reading text output, not writing raw pixels).
// If _popen returns a valid pipe but the child immediately dies (ffmpeg not
// found, launched a shell that failed), fgets returns EOF right away and we
// report that. If ffmpeg is installed we get its banner starting with
// "ffmpeg version N.N.N ...".
bool ExportEngine::TestFfmpegBinary(const std::string& ffmpegPath, std::string& outResult) {
    // Build "<ffmpeg> -version 2>&1" so stderr (where ffmpeg prints its
    // banner on some builds) gets captured too.
    std::string cmd = "\"" + ffmpegPath + "\" -version 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        outResult = "Could not spawn a shell to test ffmpeg. This is unusual.";
        return false;
    }
    char buf[512] = {0};
    std::string all;
    // Read up to ~4KB of output.
    while (std::fgets(buf, sizeof(buf), pipe)) {
        all += buf;
        if (all.size() > 4096) break;
    }
    int rc = _pclose(pipe);
    if (all.empty()) {
        outResult = "No output from ffmpeg. It's probably not installed, "
                    "or the path is wrong. Install FFmpeg and add it to your "
                    "system PATH, or set an absolute path like "
                    "C:\\\\ffmpeg\\\\bin\\\\ffmpeg.exe";
        return false;
    }
    // Keep first ~300 chars to fit the UI cleanly.
    if (all.size() > 300) all = all.substr(0, 300) + "\n... (truncated)";
    outResult = all;
    // Exit code 0 = ok; some ffmpeg builds return non-zero on -version anyway,
    // so trust the presence of "ffmpeg version" text as the success signal.
    return all.find("ffmpeg version") != std::string::npos ||
           all.find("ffmpeg  version") != std::string::npos ||
           rc == 0;
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

    // ---------------------------------------------------------------------
    // Task 6.1: Windows-native CreateProcess + CreatePipe.
    //
    // Why not _popen("wb")? Two reasons:
    //   1. MSVCRT can text-translate writes containing 0x0A bytes in raw
    //      pixel data even with the "b" mode flag. CreatePipe is guaranteed
    //      binary-clean.
    //   2. _popen gives us stdin only. We need stderr so failures produce
    //      a real diagnostic instead of the generic "encoder died?" message.
    //
    // Handle inheritance dance:
    //   - CreatePipe with SECURITY_ATTRIBUTES.bInheritHandle = TRUE so the
    //     child gets both ends by default.
    //   - Mark our WRITE end non-inheritable via SetHandleInformation so the
    //     child doesn't accidentally inherit two copies (leaking a handle
    //     keeps the pipe alive after ffmpeg dies and hangs the writer).
    //   - Do the same for the log file handle: ffmpeg should NOT inherit our
    //     reader; only the STARTUPINFO-provided write handle for its stderr.
    //     Wait — we WANT ffmpeg to inherit the write handle. Just mark it
    //     inheritable via CreateFile SECURITY_ATTRIBUTES.
    // ---------------------------------------------------------------------
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hChildStdIn_Read = nullptr;
    if (!CreatePipe(&hChildStdIn_Read, &childStdIn_Write_, &sa, 0)) {
        status_.error    = true;
        status_.errorMsg = "CreatePipe failed for ffmpeg stdin.";
        ReleaseTextures();
        return false;
    }
    // Our WRITE end must NOT be inheritable — otherwise the child gets a
    // duplicate that keeps the pipe open forever.
    SetHandleInformation(childStdIn_Write_, HANDLE_FLAG_INHERIT, 0);

    // Open the log file for ffmpeg's stdout + stderr. Truncate every run so
    // the user gets only the most recent transcript. Inherit-safe via `sa`.
    logFilePath_ = DeriveLogPath(settings_.outputPath);
    logFile_ = CreateFileA(logFilePath_.c_str(),
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (logFile_ == INVALID_HANDLE_VALUE) {
        // Non-fatal — fall back to no log; export can still work.
        logFile_ = nullptr;
    }

    // Build command line. CreateProcessA wants a MUTABLE buffer because it
    // may modify it in place, so we duplicate into a std::vector<char>.
    const std::string cmd = BuildFfmpegCommand();
    std::cerr << "[Export] Launching: " << cmd << std::endl;
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = hChildStdIn_Read;
    si.hStdError   = logFile_ ? logFile_ : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput  = logFile_ ? logFile_ : GetStdHandle(STD_OUTPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(
        nullptr,           // application name — read from command line
        cmdBuf.data(),
        nullptr, nullptr,  // process / thread security attrs
        TRUE,              // inherit handles (needed for the pipe + log)
        CREATE_NO_WINDOW,  // don't flash a cmd window
        nullptr, nullptr,  // env / cwd — inherit both
        &si, &pi);

    // We're done with the child's END of the pipe — close ours so only
    // ffmpeg holds it. When ffmpeg exits, its handle goes too and our
    // WriteFile calls will fail immediately with ERROR_BROKEN_PIPE instead
    // of hanging forever.
    CloseHandle(hChildStdIn_Read);
    hChildStdIn_Read = nullptr;

    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(childStdIn_Write_); childStdIn_Write_ = nullptr;
        if (logFile_) { CloseHandle(logFile_); logFile_ = nullptr; }
        status_.error    = true;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Could not launch ffmpeg (GetLastError=%lu). "
                      "Verify the FFmpeg path in the panel above and that "
                      "ffmpeg.exe is on your system PATH.", (unsigned long)err);
        status_.errorMsg = buf;
        ReleaseTextures();
        return false;
    }

    ffmpegProcess_ = pi.hProcess;
    ffmpegThread_  = pi.hThread;

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
    if (!childStdIn_Write_)                    return false;
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

    // WriteFile one row at a time because RowPitch may be > width*4 (D3D
    // pads rows for alignment). Writing the full mapped block would insert
    // garbage stride bytes into the video stream.
    //
    // Task 6.1: WriteFile is binary-clean by construction; no MSVCRT
    // text-mode translation to worry about. A short write (bytesWritten <
    // rowBytes) is treated as a pipe failure — ffmpeg either accepts the
    // whole row or the pipe is broken. Loop-until-full would just mask a
    // partial delivery caused by ffmpeg dying mid-frame.
    const DWORD rowBytes = (DWORD)settings_.width * 4; // BGRA
    const unsigned char* base = (const unsigned char*)mapped.pData;
    for (int y = 0; y < settings_.height; ++y) {
        const unsigned char* row = base + (size_t)y * mapped.RowPitch;
        DWORD written = 0;
        const BOOL ok = WriteFile(childStdIn_Write_, row, rowBytes, &written, nullptr);
        if (!ok || written != rowBytes) {
            const DWORD err = GetLastError();
            context_->Unmap(stagingTex_, 0);
            status_.error = true;
            // Read the tail of the log for the real diagnostic. FlushFileBuffers
            // first so ffmpeg's last stderr line makes it to disk.
            if (logFile_) FlushFileBuffers(logFile_);
            const std::string tail = ReadLogTail(logFilePath_, 2048);
            char hdr[128];
            std::snprintf(hdr, sizeof(hdr),
                "FFmpeg pipe closed at frame %d (GetLastError=%lu). Log tail:\n",
                status_.frameIndex, (unsigned long)err);
            status_.errorMsg = std::string(hdr) +
                (tail.empty() ? std::string("(log empty or unreadable — see ") + logFilePath_ + ")"
                              : tail);
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
    // Task 6.1: signal EOF to ffmpeg by closing our WRITE end of the pipe.
    // ffmpeg then finalizes the MP4 container (moov atom, etc.) and exits.
    // We wait up to 5 s for a clean exit; on cancel we TerminateProcess to
    // guarantee the child dies quickly.
    if (childStdIn_Write_) {
        CloseHandle(childStdIn_Write_);
        childStdIn_Write_ = nullptr;
    }
    if (ffmpegProcess_) {
        if (cancelled) {
            TerminateProcess(ffmpegProcess_, 1);
        }
        const DWORD wait = WaitForSingleObject(ffmpegProcess_, 5000);
        if (wait == WAIT_TIMEOUT) {
            // ffmpeg went unresponsive — kill it so the app doesn't hang.
            TerminateProcess(ffmpegProcess_, 1);
            WaitForSingleObject(ffmpegProcess_, 1000);
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(ffmpegProcess_, &exitCode);
        // Non-zero exit code from a natural (not cancelled) end means the
        // encoder rejected something — surface the log tail.
        if (!cancelled && exitCode != 0 && !status_.error) {
            if (logFile_) FlushFileBuffers(logFile_);
            const std::string tail = ReadLogTail(logFilePath_, 2048);
            char hdr[160];
            std::snprintf(hdr, sizeof(hdr),
                "FFmpeg exited with code %lu after %d frames. Log tail:\n",
                (unsigned long)exitCode, status_.frameIndex);
            status_.error    = true;
            status_.errorMsg = std::string(hdr) +
                (tail.empty() ? std::string("(log empty — see ") + logFilePath_ + ")"
                              : tail);
        }
        CloseHandle(ffmpegProcess_); ffmpegProcess_ = nullptr;
    }
    if (ffmpegThread_) { CloseHandle(ffmpegThread_); ffmpegThread_ = nullptr; }
    if (logFile_)      { CloseHandle(logFile_);      logFile_      = nullptr; }

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
