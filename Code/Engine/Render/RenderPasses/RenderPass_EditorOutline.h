#pragma once

#if EE_DEVELOPMENT_TOOLS

#include "Base/Render/RHI.h"
#include "Engine/Render/Device/DeviceRenderView.h"
#include "Engine/Render/RenderPasses/RenderPass.h"

namespace EE::Render
{
    namespace ShaderTypes
    {
        struct RenderView;
    }

    class RenderSystem;
    class RenderViewport;

    //-------------------------------------------------------------------------

    struct EditorOutlineRenderPass
    {
        DeviceRenderView                            m_renderView;

        RHI::Pipeline*                              m_pInitializePipeline = nullptr;
        RHI::Pipeline*                              m_pJumpFloodPipeline = nullptr;
        RHI::Pipeline*                              m_pCompositePipeline = nullptr;

        RenderSettings const*                       m_pRenderSettings = nullptr;

        //-------------------------------------------------------------------------

        void Initialize( RenderPassContext const& context );
        void Shutdown( RenderSystem* pRenderSystem );

        void UpdateDeviceResources( RenderSystem* pRenderSystem, DeviceRenderWorld const& deviceRenderWorld );

        void UpdateViewportDeviceResources( RenderSystem* pRenderSystem, RenderViewport* pRenderViewport );

        void UpdateRenderViews( RenderViewport const* pRenderViewport, TArrayView<ShaderTypes::RenderView> dstRenderViews_WriteCombined ) const;

        void DrawToViewport
        (
            TArrayView<ForwardShadingMaterialShaderPipelineBucket const>    materialShaderBuckets,
            RenderViewport const*                                           pRenderViewport,
            DeviceResourceStates&                                           resourceStates,
            RHI::CommandBuffer*                                             pCommandBuffer
        ) const;

        void ResolveToViewport( RenderViewport const* pRenderViewport, DeviceResourceStates& resourceStates, RHI::CommandBuffer* pCommandBuffer ) const;
    };
}

#endif
