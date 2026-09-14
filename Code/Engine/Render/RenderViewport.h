#pragma once

#include "Engine/_Module/API.h"
#include "Engine/Viewport/Viewport.h"
#include "Engine/Viewport/ViewportPicking.h"
#include "Engine/Render/Device/DeviceResourceState.h"
#include "Engine/Render/Device/DeviceAppendBuffer.h"
#include "Engine/Render/Device/DeviceResizeBuffer.h"
#include "Base/Render/RHI.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    class Window;

    //-------------------------------------------------------------------------

    class EE_ENGINE_API RenderViewport final : public Viewport
    {

    public:

        virtual bool IsValid() const override;

        void Initialize( RHI::Context* pContextRHI, Render::Window* pWindow );
        void Shutdown( RHI::Context* pContextRHI );

        void UpdateRenderWindow( Render::Window* pWindow );

        inline bool IsStandalone() const
        {
            #if EE_DEVELOPMENT_TOOLS
            return m_isStandalone;
            #else
            return true;
            #endif
        }

        #if EE_DEVELOPMENT_TOOLS
        inline void SetPickingEnabled( bool enabled ) { m_isPickingEnabled = enabled; }
        inline bool IsPickingEnabled() const { return m_isPickingEnabled; }
        #endif

        inline bool TextureNeedsResize( RHI::Texture* pTexture ) const
        {
            if ( !pTexture || pTexture->m_width != uint32_t( m_size.m_x ) || pTexture->m_height != uint32_t( m_size.m_y ) )
            {
                return true;
            }
            return false;
        }

    public:

        Render::Window*                                     m_pWindow = nullptr;

        // TODO: Bunch of mutable stuff here, we don't have/need multithreaded command buffer recording right now so it's a later problem.
        // Renderer is recording very small command buffers so it's not a performance issue, all culling work is done on the GPU.
        mutable DeviceTextureState                          m_forwardShading_depthTexture = {};
        mutable DeviceTextureState                          m_forwardShading_colorTexture = {};

        mutable DeviceTextureState                          m_depthDownsample2 = {};
        mutable DeviceTextureState                          m_depthDownsample4 = {};
        mutable DeviceTextureState                          m_depthDownsample8 = {};

        mutable DeviceTextureState                          m_SMAA_stencilTexture = {};
        mutable DeviceTextureState                          m_SMAA_edgesTexture = {};
        mutable DeviceTextureState                          m_SMAA_blendTexture = {};
        mutable DeviceTextureState                          m_SMAA_resultTexture = {};

        mutable DeviceTextureState                          m_GTAO_resultTextureNoisy0 = {};
        mutable DeviceTextureState                          m_GTAO_resultTextureNoisy1 = {};

        mutable DeviceTextureState                          m_GTAO_resultTextureHalfResolution = {};
        mutable DeviceTextureState                          m_GTAO_resultTexture = {};
        mutable DeviceTextureState                          m_GTAO_edgesTexture = {};
        mutable DeviceTextureState                          m_GTAO_prefilterDepthTexture = {};

        mutable DeviceTextureState                          m_finalTexture = {};

        #if EE_DEVELOPMENT_TOOLS
        mutable DeviceTextureState                          m_debugDraw_depthTexture = {};

        mutable DeviceTextureState                          m_editorOutline_depthTexture = {};
        mutable DeviceTextureState                          m_editorOutline_idTexture = {};
        mutable DeviceTextureState                          m_editorOutline_JFA_Texture0 = {};
        mutable DeviceTextureState                          m_editorOutline_JFA_Texture1 = {};
        #endif

        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_GTAO_parametersBuffers = {};
        uint32_t                                            m_GTAO_noiseIndex = 0;

        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_globalParametersBuffers = {};
        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_renderViewBuffers = {};
        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_renderBucketBuffers = {};
        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_cascadedShadowBuffers = {};

        uint32_t                                            m_numGlobalEnvironmentMapRenderViews = 0;
        uint32_t                                            m_numCascadedShadowRenderViews = 0;
        uint32_t                                            m_numForwardShadingRenderViews = 0;
        uint32_t                                            m_numEditorOutlineRenderViews = 0;

        uint32_t                                            m_globalEnvironmentMapRenderViewsOffset = 0;
        uint32_t                                            m_cascadedShadowRenderViewsOffset = 0;
        uint32_t                                            m_forwardShadingRenderViewsOffset = 0;
        uint32_t                                            m_editorOutlineRenderViewsOffset = 0;

        uint32_t                                            m_numRenderViews = 0;
        uint32_t                                            m_numRenderBuckets = 0;
        uint32_t                                            m_numRenderViewBucketsPerView = 0;

        #if EE_DEVELOPMENT_TOOLS
        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_shaderDebugDrawBuffers = {};
        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_debugCommandsBuffers = {};
        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_debugCommandsBuffersOutline = {};
        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_debugParametersBuffers = {};

        TArray<RHI::Buffer*, RHI::MaxPendingFrames>         m_meshArgumentCounterBuffers = {};
        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_meshArgumentBuffers = {};
        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_meshParametersBuffers = {};

        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_debugMeshArgumentBuffersOutline = {};
        TArray<DeviceResizeBuffer, RHI::MaxPendingFrames>   m_debugMeshParametersBuffersOutline = {};

        uint32_t                                            m_numCommands_transparentDepthOnWrite = 0;
        uint32_t                                            m_numCommands_transparentDepthOnNoWrite = 0;
        uint32_t                                            m_numCommands_transparentDepthSeparateWrite = 0;
        uint32_t                                            m_numCommands_outline = 0;
        uint32_t                                            m_numMeshCommands_outline = 0;

        DeviceAppendBuffer<PickingResult>                   m_instancePickingResultsBuffer;
        DeviceResizeBuffer                                  m_instancePickingDistancesBuffer;
        DeviceAppendBuffer<PickingResult>                   m_debugDrawPickingResultsBuffer;

        Float2                                              m_lastKnownPickingMousePosition = Float2::Zero;
        uint32_t                                            m_lastKnownPickingPixelRadius = 2;

        bool                                                m_isStandalone = true;
        bool                                                m_isPickingEnabled = false;
        #endif
    };
}
