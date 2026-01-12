#pragma once

/**
 * ImGui Overlay for configuration
 */

#ifndef FIVEM_FRAMEGEN_OVERLAY_H
#define FIVEM_FRAMEGEN_OVERLAY_H

#include <Windows.h>
#include <d3d11.h>
#include "../include/fivem_framegen.h"

namespace FiveMFrameGen {
namespace Overlay {

/**
 * ImGui-based configuration overlay
 */
class ImGuiOverlay {
public:
    ImGuiOverlay();
    ~ImGuiOverlay();
    
    /**
     * Initialize ImGui
     */
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, HWND window);
    
    /**
     * Shutdown ImGui
     */
    void Shutdown();
    
    /**
     * Toggle visibility
     */
    void Toggle();
    
    /**
     * Check if visible
     */
    bool IsVisible() const { return m_Visible; }
    
    /**
     * Render the overlay
     */
    void Render(Config& config, const Stats& stats);
    
    /**
     * Set render target for drawing
     */
    void SetRenderTarget(ID3D11RenderTargetView* rtv) { m_RenderTargetView = rtv; }

private:
    /**
     * Render the main configuration window
     */
    void RenderConfigWindow(Config& config, const Stats& stats);
    
    /**
     * Render performance graph
     */
    void RenderPerformanceGraph(const Stats& stats);
    
    /**
     * Window procedure for input handling
     */
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool m_Initialized = false;
    bool m_Visible = false;
    
    HWND m_Window = nullptr;
    WNDPROC m_OriginalWndProc = nullptr;
    
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    ID3D11RenderTargetView* m_RenderTargetView = nullptr;
    
    // Performance history for graph
    float m_FPSHistory[120] = {};
    int m_FPSHistoryIndex = 0;
    
    static ImGuiOverlay* s_Instance;
};

} // namespace Overlay
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_OVERLAY_H
