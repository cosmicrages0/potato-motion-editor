# Engine Specification: Native Motion Graphics Editor (Potato PC Edition)

## 1. Executive Summary
A ultra-lightweight, native Windows C++20 motion graphics editor built with DirectX 11 and Dear ImGui.
Designed to replicate After Effects UI/layer workflow, CapCut fast FX, and Alight Motion 3D camera system while operating on low-end hardware (Dual-Core CPUs, 4GB RAM, Integrated Graphics).

## 2. Tech Stack & Dependencies
*   **Language:** C++20 (MSVC Compiler)
*   **Target Architecture:** Windows x64 (DirectX 11 with DX9 feature level fallback)
*   **Windowing & Input:** SDL2
*   **UI System:** Dear ImGui (`docking` branch)
*   **Math Library:** DirectXMath / DirectXToolKit
*   **Video I/O:** FFmpeg C API (libavcodec, libavformat, libswscale)
*   **Build System:** CMake 3.20+ via GitHub Actions (`windows-latest`)

## 3. Core Architectural Modules

### A. Rendering Engine (`RenderEngine.cpp`)
*   Direct3D 11 device/context initialization.
*   Offscreen Framebuffer rendering (`ID3D11RenderTargetView`) for composition viewports.
*   GPU Texture quad rendering with hardware matrix transforms.

### B. Graph & Keyframe Engine (`AnimationEngine.cpp`)
*   Evaluates property interpolation using Cubic Bézier Curves:
    $$B(t) = (1-t)^3 P_0 + 3(1-t)^2 t P_1 + 3(1-t) t^2 P_2 + t^3 P_3$$
*   **Overshoot Feature:** Graph handles ($P_1, P_2$) are allowed to go above $1.0$ ($100\%$) and below $0.0$ ($0\%$) to generate elastic slingshot / rebound effects without extending clip duration.

### C. Alight Motion Camera Model (`Camera3D.cpp`)
*   Single-node 3D Camera system using Field of View (FOV), Pitch, Yaw, Roll, and Z-Depth.
*   Transforms 2D/3D layers via World-View-Projection (WVP) matrix math:
    $$v_{\text{clip}} = P_{\text{camera}} \cdot V_{\text{camera}} \cdot M_{\text{world}} \cdot v_{\text{local}}$$

### D. HLSL Shader Stack (`Shaders.hlsl`)
*   Pixel shaders executed on GPU textures for zero-CPU rendering:
    *   **Motion Tile / Mirror Edge:** UV wrapping via $\text{abs}(\text{frac}(\text{UV} \cdot N) \cdot 2.0 - 1.0)$.
    *   **Directional Motion Blur:** Vector pixel sampling along trajectory angles.
    *   **Chromatic Aberration:** Distance-based RGB channel displacement.
    *   **Blending Modes:** Additive, Multiply, Screen, Overlay, Color Dodge.

### E. Proxy & Low-RAM 4K Export Engine (`ExportEngine.cpp`)
*   **Edit Mode:** Uses $1280\times720$ MJPEG proxy video files to ensure $0\%$ CPU decoding lag.
*   **Export Mode:** Swaps media handles to $3840\times2160$ original source files.
*   **Memory Guarantee:** Uses a single-frame stream pipe directly to FFmpeg (`_popen` raw BGRA buffer write). Never allocates more than **1 frame** of 4K memory in RAM at any time (~33 MB RAM footprint).

## 4. UI Docking Layout Architecture
*   **Top:** Menu Bar (File, Edit, Layer, Effect, Export).
*   **Left:** Project Assets / Media Bin.
*   **Center:** Composition Viewport with interactive transform gizmos.
*   **Right:** Inspector / Effect Control Panel (AE Property Tree).
*   **Bottom Left:** Layer Hierarchy Timeline (`[3D]`, `[Solo]`, `[Mute]`, `[Parent]` flags).
*   **Bottom Right:** Bézier Curve Graph Editor.
