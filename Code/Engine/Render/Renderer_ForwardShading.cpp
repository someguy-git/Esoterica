#include "Renderer_ForwardShading.h"
#include "Engine/Render/RenderViewport.h"
#include "Engine/Render/RenderSystem.h"
#include "Engine/Render/Systems/WorldSystem_Render.h"
#include "Engine/Render/Settings/ViewportSettings_Render.h"
#include "Engine/Render/Components/Component_Lights.h" // TODO: Move light components to DeviceRenderWorld
#include "Engine/Render/DebugMesh/DebugMeshRegistry.h"
#include "Engine/UpdateContext.h"
#include "Engine/Entity/EntityWorld.h"
#include "Base/Profiling.h"
#include "Base/Render/Settings/Settings_Render.h"
#include "Base/Render/RenderWindow.h"
#include "Base/Render/RHI.h"
#include "Base/Math/ViewVolume.h"
#include "EASTL/bit.h"

#include "Engine/Render/Shaders/Renderer/RendererTypes.esh"
#include "Engine/Render/Shaders/Picking/Picking.esh"

//-------------------------------------------------------------------------

namespace EE::Render
{
    template<typename F>
    inline void ForwardShadingRenderer::ForEachRenderBucket( uint32_t numCascadedShadowPasses, bool includeEditorOutline, F fn )
    {
        uint32_t renderViewIndex = 0;

        for ( DeviceRenderView& renderView : m_renderPass_GlobalEnvironmentMap.m_renderViews )
        {
            renderView.ForEachRenderBucket( fn );
            renderViewIndex++;
        }

        for ( uint32_t cascadedShadowPassIndex = 0; cascadedShadowPassIndex < numCascadedShadowPasses; ++cascadedShadowPassIndex )
        {
            for ( DeviceRenderView& renderView : m_renderPass_CascadedShadows[cascadedShadowPassIndex].m_renderViews )
            {
                renderView.ForEachRenderBucket( fn );
                renderViewIndex++;
            }
        }

        m_renderPass_ForwardShading.m_renderView.ForEachRenderBucket( fn );
        renderViewIndex++;

        #if EE_DEVELOPMENT_TOOLS
        if ( includeEditorOutline )
        {
            m_renderPass_EditorOutline.m_renderView.ForEachRenderBucket( fn );
            renderViewIndex++;
        }
        #endif

        EE_ASSERT( renderViewIndex == GlobalEnvironmentMapPass::NumRenderViews + numCascadedShadowPasses * CascadedShadowPass::NumShadowCascades + 1 + ( includeEditorOutline ? 1 : 0 ) );
    }

    template <typename F>
    inline void ForwardShadingRenderer::ForEachRenderPass( F fn )
    {
        fn( m_renderPass_ForwardShading );

        for ( CascadedShadowPass& cascadedShadowPass : m_renderPass_CascadedShadows )
        {
            fn( cascadedShadowPass );
        }

        fn( m_renderPass_DepthDownsample );
        fn( m_renderPass_GlobalEnvironmentMap );
        fn( m_renderPass_SMAA );
        fn( m_renderPass_GTAO );
        fn( m_renderPass_PostProcess );

        #if EE_DEVELOPMENT_TOOLS
        fn( m_renderPass_DebugDraw );
        fn( m_renderPass_EditorOutline );
        #endif
    }

    //-------------------------------------------------------------------------

    void ShaderCullingBucket::Initialize( RHI::Context* pContextRHI, const char* shaderName )
    {
        m_instanceVisibilityBuffer.Initialize( pContextRHI, false );
        m_clusterCullingWorkBuffer.Initialize( pContextRHI, false );
        m_cullingArgumentBuffer.Initialize( pContextRHI, false );
        m_drawCompactionArgumentBuffer.Initialize( pContextRHI, false );
        m_drawClusterBuffer.Initialize( pContextRHI, false );

        RHI::BufferParameters cullingCounterBufferParameters = {};
        cullingCounterBufferParameters.m_bufferSize = sizeof( uint32_t );
        cullingCounterBufferParameters.m_bufferStride = sizeof( uint32_t );
        cullingCounterBufferParameters.m_format = RHI::DataFormat::R32_UInt;
        cullingCounterBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
        cullingCounterBufferParameters.m_debugName.sprintf( "%s Culling Work Counter Buffer", shaderName );

        m_pCullingCounterBuffer = RHI::CreateBuffer( pContextRHI, cullingCounterBufferParameters );

        uint32_t const numDrawClusterBufferEntries = uint32_t( EE_MAX_CULLING_VIEWS * DeviceRenderView::s_NumRenderBucketsPerViewBucket );

        RHI::BufferParameters drawClusterCountersBufferParameters = {};
        drawClusterCountersBufferParameters.m_bufferSize = sizeof( uint32_t ) * numDrawClusterBufferEntries;
        drawClusterCountersBufferParameters.m_bufferStride = sizeof( uint32_t );
        drawClusterCountersBufferParameters.m_format = RHI::DataFormat::R32_UInt;
        drawClusterCountersBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
        drawClusterCountersBufferParameters.m_debugName.sprintf( "%s Draw Cluster Counters Buffer", shaderName );

        m_pDrawClusterCountersBuffer = RHI::CreateBuffer( pContextRHI, drawClusterCountersBufferParameters );

        RHI::BufferParameters drawClusterScatterOffsetsBufferParameters = {};
        drawClusterScatterOffsetsBufferParameters.m_bufferSize = sizeof( uint32_t ) * numDrawClusterBufferEntries;
        drawClusterScatterOffsetsBufferParameters.m_bufferStride = sizeof( uint32_t );
        drawClusterScatterOffsetsBufferParameters.m_format = RHI::DataFormat::R32_UInt;
        drawClusterScatterOffsetsBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
        drawClusterScatterOffsetsBufferParameters.m_debugName.sprintf( "%s Draw Cluster Scatter Offsets Buffer", shaderName );

        m_pDrawClusterScatterOffsetsBuffer = RHI::CreateBuffer( pContextRHI, drawClusterScatterOffsetsBufferParameters );

        RHI::BufferParameters drawClusterBaseOffsetsBufferParameters = {};
        drawClusterBaseOffsetsBufferParameters.m_bufferSize = sizeof( uint32_t ) * numDrawClusterBufferEntries;
        drawClusterBaseOffsetsBufferParameters.m_bufferStride = sizeof( uint32_t );
        drawClusterBaseOffsetsBufferParameters.m_format = RHI::DataFormat::R32_UInt;
        drawClusterBaseOffsetsBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
        drawClusterBaseOffsetsBufferParameters.m_debugName.sprintf( "%s Draw Cluster Base Offsets Buffer", shaderName );

        m_pDrawClusterBaseOffsetsBuffer = RHI::CreateBuffer( pContextRHI, drawClusterBaseOffsetsBufferParameters );
    }

    void ShaderCullingBucket::Shutdown( RHI::Context* pContextRHI )
    {
        m_instanceVisibilityBuffer.Shutdown( pContextRHI );
        m_clusterCullingWorkBuffer.Shutdown( pContextRHI );
        m_cullingArgumentBuffer.Shutdown( pContextRHI );
        m_drawCompactionArgumentBuffer.Shutdown( pContextRHI );
        m_drawClusterBuffer.Shutdown( pContextRHI );

        RHI::DestroyBuffer( pContextRHI, eastl::move( m_pCullingCounterBuffer ) );
        RHI::DestroyBuffer( pContextRHI, eastl::move( m_pDrawClusterCountersBuffer ) );
        RHI::DestroyBuffer( pContextRHI, eastl::move( m_pDrawClusterScatterOffsetsBuffer ) );
        RHI::DestroyBuffer( pContextRHI, eastl::move( m_pDrawClusterBaseOffsetsBuffer ) );
    }

    //-------------------------------------------------------------------------

    void ForwardShadingRenderer::Initialize( SystemRegistry* pSystemRegistry, RenderSettings const& renderSettings )
    {
        m_pRenderSystem = pSystemRegistry->GetSystem<RenderSystem>();
        m_pRenderGlobalSettings = &renderSettings;

        // Pipelines
        //-------------------------------------------------------------------------

        static StringID const s_InstanceCullingShaderID( "InstanceCulling" );
        static StringID const s_CullingCompactionShaderID( "CullingCompaction" );
        static StringID const s_CullingArgumentGenerationShaderID( "CullingArgumentGeneration" );
        static StringID const s_DrawCompactionShaderID( "DrawCompaction" );
        static StringID const s_ClusterCullingShaderID( "ClusterCulling" );
        static StringID const s_DrawArgumentGenerationShaderID( "DrawArgumentGeneration" );
        static StringID const s_LightCulling_CullLightsShaderID( "LightCulling_CullLights" );

        m_pInstanceCullingShader = m_pRenderSystem->FindComputeShader( s_InstanceCullingShaderID );
        m_pCullingCompactionShader = m_pRenderSystem->FindComputeShader( s_CullingCompactionShaderID );
        m_pCullingArgumentGenerationShader = m_pRenderSystem->FindComputeShader( s_CullingArgumentGenerationShaderID );
        m_pDrawCompactionShader = m_pRenderSystem->FindComputeShader( s_DrawCompactionShaderID );
        m_pClusterCullingShader = m_pRenderSystem->FindComputeShader( s_ClusterCullingShaderID );
        m_pDrawArgumentGenerationShader = m_pRenderSystem->FindComputeShader( s_DrawArgumentGenerationShaderID );
        m_pLightCulling_CullLightsShader = m_pRenderSystem->FindComputeShader( s_LightCulling_CullLightsShaderID );

        #if EE_DEVELOPMENT_TOOLS
        static StringID const s_InstancePickingResolveShaderID( "InstancePickingResolve" );

        m_pInstancePickingResolveShader = m_pRenderSystem->FindComputeShader( s_InstancePickingResolveShaderID );
        #endif

        m_materialShaderPipelineBuckets = ForwardShadingPass::InitializeMaterialShaderBuckets( m_pRenderSystem );

        // Culling buckets - one per material shader, shared by all viewports
        //-------------------------------------------------------------------------

        m_shaderCullingBuckets.resize( m_materialShaderPipelineBuckets.size() );
        for ( size_t shaderIndex = 0; shaderIndex < m_shaderCullingBuckets.size(); ++shaderIndex )
        {
            m_shaderCullingBuckets[shaderIndex].Initialize( m_pRenderSystem->GetContextRHI(), m_materialShaderPipelineBuckets[shaderIndex].m_shaderName.data() );
        }

        m_LightCulling_SpatialHash.Initialize( m_pRenderSystem->GetContextRHI(), "LightCulling", m_pRenderGlobalSettings->m_spatialHashTableSize, 0 );

        // Render passes
        //-------------------------------------------------------------------------

        ForEachRenderPass( [this] ( auto& renderPass )
        {
            RenderPassContext context = { m_pRenderSystem, m_pRenderGlobalSettings, m_materialShaderPipelineBuckets, };
            renderPass.Initialize( context );
        } );

        #if EE_DEVELOPMENT_TOOLS
        m_renderPass_DebugDraw.SetDebugMeshRegistry( pSystemRegistry->GetSystem<DebugMeshRegistry>() );
        #endif
    }

    void ForwardShadingRenderer::Shutdown()
    {
        #if EE_DEVELOPMENT_TOOLS
        m_renderPass_DebugDraw.ClearDebugMeshRegistry();
        #endif

        for ( ForwardShadingMaterialShaderPipelineBucket& materialShaderPipelineBucket : m_materialShaderPipelineBuckets )
        {
            materialShaderPipelineBucket.Shutdown( m_pRenderSystem->GetContextRHI() );
        }
        m_materialShaderPipelineBuckets.clear();

        for ( ShaderCullingBucket& shaderCullingBucket : m_shaderCullingBuckets )
        {
            shaderCullingBucket.Shutdown( m_pRenderSystem->GetContextRHI() );
        }
        m_shaderCullingBuckets.clear();

        ForEachRenderPass( [this] ( auto& renderPass )
        {
            renderPass.Shutdown( m_pRenderSystem );
        } );

        m_renderPass_CascadedShadows.clear();

        m_LightCulling_SpatialHash.Shutdown( m_pRenderSystem->GetContextRHI() );
    }

    void ForwardShadingRenderer::UpdateDeviceResources( UpdateContext const& ctx )
    {
        EE_PROFILE_FUNCTION_RENDER();

        uint32_t            frameIndex = m_pRenderSystem->GetFrameIndex();
        RHI::Context*       pContextRHI = m_pRenderSystem->GetContextRHI();

        //-------------------------------------------------------------------------

        m_renderPass_GlobalEnvironmentMap.UpdateDeviceResources_DFG( pContextRHI );
    }

    void ForwardShadingRenderer::UpdateViewportDeviceResources( UpdateContext const& ctx, RenderViewport* pRenderViewport, EntityWorld* pWorld )
    {
        EE_PROFILE_FUNCTION_RENDER();

        uint32_t frameIndex = m_pRenderSystem->GetFrameIndex();
        RHI::Context* pContextRHI = m_pRenderSystem->GetContextRHI();
        RenderWorldSystem* pRenderWorldSystem = pWorld->GetWorldSystem<RenderWorldSystem>();

        //-------------------------------------------------------------------------

        #if EE_DEVELOPMENT_TOOLS
        pRenderWorldSystem->UpdateViewportPickingData( pRenderViewport );
        #endif

        bool const enableAsyncCompute = m_pRenderGlobalSettings->m_enableAsyncCompute;
        bool const enableSMAA = m_pRenderGlobalSettings->m_enableSMAA;
        bool const enableSSAO = m_pRenderGlobalSettings->m_enableSSAO;
        bool const enableSSAOLowResolution = m_pRenderGlobalSettings->m_enableSSAOLowResolution;

        #if EE_DEVELOPMENT_TOOLS
        bool const enableEditorOutline = pRenderViewport->IsPickingEnabled();
        #else
        bool const enableEditorOutline = false;
        #endif

        bool const enableDepthDownsample = ( enableSSAO && enableSSAOLowResolution );

        //-------------------------------------------------------------------------

        m_renderPass_ForwardShading.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );

        if ( enableSMAA )
        {
            m_renderPass_SMAA.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );
        }

        if ( enableDepthDownsample )
        {
            m_renderPass_DepthDownsample.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );
        }

        if ( enableSSAO )
        {
            m_renderPass_GTAO.m_enableLowResolution = enableSSAOLowResolution;
            m_renderPass_GTAO.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );
        }

        m_renderPass_PostProcess.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );

        #if EE_DEVELOPMENT_TOOLS
        m_renderPass_DebugDraw.UpdateViewportDeviceResources
        (
            m_pRenderSystem,
            pRenderWorldSystem->m_deviceRenderWorld,
            pWorld->GetDebugDrawSystem(),
            ctx.GetDeltaTime(),
            pRenderViewport
        );

        if ( enableEditorOutline )
        {
            m_renderPass_EditorOutline.UpdateViewportDeviceResources( m_pRenderSystem, pRenderViewport );
        }
        #endif

        // Light culling spatial hash
        //-------------------------------------------------------------------------

        uint32_t const spatialHashPayloadStride = DeviceSpatialHash::ComputePayloadStride
        (
            pRenderWorldSystem->m_deviceRenderWorld.GetNumPointLightPages(),
            pRenderWorldSystem->m_deviceRenderWorld.GetNumSpotLightPages()
        );

        m_LightCulling_SpatialHash.UpdateBuffers( m_pRenderSystem, frameIndex, spatialHashPayloadStride );

        m_LightCulling_SpatialHash.m_numLODs = Math::Clamp( m_pRenderGlobalSettings->m_spatialHashNumLODs, 1u, DeviceSpatialHash::MaxLODs );

        m_LightCulling_SpatialHash.m_borderCells[1] = m_pRenderGlobalSettings->m_spatialHashBorderLOD1;
        m_LightCulling_SpatialHash.m_borderCells[2] = m_pRenderGlobalSettings->m_spatialHashBorderLOD2;
        m_LightCulling_SpatialHash.m_borderCells[3] = m_pRenderGlobalSettings->m_spatialHashBorderLOD3;
        m_LightCulling_SpatialHash.m_borderCells[4] = m_pRenderGlobalSettings->m_spatialHashBorderLOD4;
        m_LightCulling_SpatialHash.m_borderCells[5] = m_pRenderGlobalSettings->m_spatialHashBorderLOD5;

        // Compute how much render views we need
        //-------------------------------------------------------------------------

        uint32_t currentRenderViewOffset = 0;

        pRenderViewport->m_numRenderViewBucketsPerView = uint32_t( m_materialShaderPipelineBuckets.size() * DeviceRenderView::s_NumRenderBucketsPerViewBucket );

        pRenderViewport->m_numGlobalEnvironmentMapRenderViews = GlobalEnvironmentMapPass::NumRenderViews;
        pRenderViewport->m_globalEnvironmentMapRenderViewsOffset = currentRenderViewOffset;
        currentRenderViewOffset += pRenderViewport->m_numGlobalEnvironmentMapRenderViews;

        pRenderViewport->m_numCascadedShadowRenderViews = pRenderWorldSystem->m_numShadowCastingDirectionalLights * CascadedShadowPass::NumShadowCascades;
        pRenderViewport->m_cascadedShadowRenderViewsOffset = currentRenderViewOffset;
        currentRenderViewOffset += pRenderViewport->m_numCascadedShadowRenderViews;

        pRenderViewport->m_numForwardShadingRenderViews = 1;
        pRenderViewport->m_forwardShadingRenderViewsOffset = currentRenderViewOffset;
        currentRenderViewOffset += pRenderViewport->m_numForwardShadingRenderViews;

        pRenderViewport->m_numEditorOutlineRenderViews = enableEditorOutline ? 1 : 0;
        pRenderViewport->m_editorOutlineRenderViewsOffset = currentRenderViewOffset;
        currentRenderViewOffset += pRenderViewport->m_numEditorOutlineRenderViews;

        pRenderViewport->m_numRenderViews = pRenderViewport->m_numGlobalEnvironmentMapRenderViews + pRenderViewport->m_numCascadedShadowRenderViews + pRenderViewport->m_numForwardShadingRenderViews + pRenderViewport->m_numEditorOutlineRenderViews;
        pRenderViewport->m_numRenderBuckets = pRenderViewport->m_numRenderViews * pRenderViewport->m_numRenderViewBucketsPerView;

        EE_ASSERT( pRenderViewport->m_numRenderViews <= EE_MAX_CULLING_VIEWS );
        EE_ASSERT( currentRenderViewOffset == pRenderViewport->m_numRenderViews );

        //-------------------------------------------------------------------------

        for ( uint32_t shaderIndex = 0; shaderIndex < m_materialShaderPipelineBuckets.size(); ++shaderIndex )
        {
            uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );

            auto UpdateBuffer_DrawClusterBuffer = [this, shaderIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                m_pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters drawClusterBufferParameters = {};
                drawClusterBufferParameters.m_bufferSize = newBufferSize;
                drawClusterBufferParameters.m_bufferStride = sizeof( uint32_t );
                drawClusterBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
                drawClusterBufferParameters.m_debugName.sprintf( "Shader Culling Bucket Draw Cluster Buffer %u", shaderIndex );

                return RHI::CreateBuffer( m_pRenderSystem->GetContextRHI(), drawClusterBufferParameters );
            };

            m_shaderCullingBuckets[shaderIndex].m_drawClusterBuffer.UpdateDeviceResources
            (
                Math::Max( 1ULL, size_t( clusterCapacity ) * pRenderViewport->m_numRenderViews * sizeof( uint32_t ) ),
                UpdateBuffer_DrawClusterBuffer
            );
        }

        // Global Parameters Buffer
        //-------------------------------------------------------------------------

        if ( !pRenderViewport->m_globalParametersBuffers[frameIndex] )
        {
            RHI::BufferParameters globalParametersBufferParameters = {};
            globalParametersBufferParameters.m_bufferSize = sizeof( ShaderTypes::GlobalParameters );
            globalParametersBufferParameters.m_bufferStride = sizeof( ShaderTypes::GlobalParameters );
            globalParametersBufferParameters.m_memoryType = RHI::ResourceMemoryType::HostToDevice;
            globalParametersBufferParameters.m_descriptorTypes = RHI::DescriptorTypeFlags::ConstantBuffer;
            globalParametersBufferParameters.m_flags = RHI::BufferFlags::PersistentMap;
            globalParametersBufferParameters.m_debugName.sprintf( "Global Parameters Buffer %i", frameIndex );

            pRenderViewport->m_globalParametersBuffers[frameIndex] = RHI::CreateBuffer( pContextRHI, globalParametersBufferParameters );
        }

        // Render views
        //-------------------------------------------------------------------------

        size_t const renderViewBufferSize = pRenderViewport->m_numRenderViews * sizeof( ShaderTypes::RenderView );
        if ( !pRenderViewport->m_renderViewBuffers[frameIndex] || pRenderViewport->m_renderViewBuffers[frameIndex]->m_size < renderViewBufferSize )
        {
            RHI::DestroyBuffer( pContextRHI, eastl::move( pRenderViewport->m_renderViewBuffers[frameIndex] ) );

            RHI::BufferParameters renderViewBufferParameters = {};
            renderViewBufferParameters.m_debugName.sprintf( "RenderView Buffer %i", frameIndex );
            renderViewBufferParameters.m_bufferSize = renderViewBufferSize;
            renderViewBufferParameters.m_bufferStride = sizeof( ShaderTypes::RenderView );
            renderViewBufferParameters.m_memoryType = RHI::ResourceMemoryType::HostToDevice;
            renderViewBufferParameters.m_flags = RHI::BufferFlags::PersistentMap;

            pRenderViewport->m_renderViewBuffers[frameIndex] = RHI::CreateBuffer( pContextRHI, renderViewBufferParameters );
        }

        size_t const renderBucketBufferSize = pRenderViewport->m_numRenderBuckets * sizeof( ShaderTypes::RenderBucket );
        if ( !pRenderViewport->m_renderBucketBuffers[frameIndex] || pRenderViewport->m_renderBucketBuffers[frameIndex]->m_size < renderBucketBufferSize )
        {
            RHI::DestroyBuffer( pContextRHI, eastl::move( pRenderViewport->m_renderBucketBuffers[frameIndex] ) );

            RHI::BufferParameters renderBucketBufferParameters = {};
            renderBucketBufferParameters.m_bufferSize = renderBucketBufferSize;
            renderBucketBufferParameters.m_bufferStride = sizeof( ShaderTypes::RenderBucket );
            renderBucketBufferParameters.m_memoryType = RHI::ResourceMemoryType::HostToDevice;
            renderBucketBufferParameters.m_debugName = "RenderViewport RenderBucket Buffer";
            renderBucketBufferParameters.m_flags = RHI::BufferFlags::PersistentMap;

            pRenderViewport->m_renderBucketBuffers[frameIndex] = RHI::CreateBuffer( pContextRHI, renderBucketBufferParameters );
        }

        // Cascaded shadows
        //-------------------------------------------------------------------------
        size_t const cascadedShadowsBufferSize = Math::Max( pRenderViewport->m_numCascadedShadowRenderViews, 1U ) * sizeof( ShaderTypes::CascadedShadow );
        if ( !pRenderViewport->m_cascadedShadowBuffers[frameIndex] || pRenderViewport->m_cascadedShadowBuffers[frameIndex]->m_size < cascadedShadowsBufferSize )
        {
            RHI::DestroyBuffer( pContextRHI, eastl::move( pRenderViewport->m_cascadedShadowBuffers[frameIndex] ) );

            RHI::BufferParameters cascadedShadowBufferParameters = {};
            cascadedShadowBufferParameters.m_bufferSize = cascadedShadowsBufferSize;
            cascadedShadowBufferParameters.m_bufferStride = sizeof( ShaderTypes::CascadedShadow );
            cascadedShadowBufferParameters.m_memoryType = RHI::ResourceMemoryType::HostToDevice;
            cascadedShadowBufferParameters.m_debugName = "RenderViewport CascadedShadow Buffer";
            cascadedShadowBufferParameters.m_flags = RHI::BufferFlags::PersistentMap;

            pRenderViewport->m_cascadedShadowBuffers[frameIndex] = RHI::CreateBuffer( pContextRHI, cascadedShadowBufferParameters );
        }

        // Update render views for all passes
        //-------------------------------------------------------------------------
        {
            TArrayView<ShaderTypes::RenderView> renderViews_WriteCombined = TArrayView<ShaderTypes::RenderView>( static_cast<ShaderTypes::RenderView*>( pRenderViewport->m_renderViewBuffers[frameIndex]->m_pMappedAddress_WriteCombined ), pRenderViewport->m_numRenderViews );
            TArrayView<ShaderTypes::RenderView> globalEnvironmentMapRenderViews_WriteCombined = renderViews_WriteCombined.subspan( pRenderViewport->m_globalEnvironmentMapRenderViewsOffset, pRenderViewport->m_numGlobalEnvironmentMapRenderViews );
            m_renderPass_GlobalEnvironmentMap.UpdateRenderViews( globalEnvironmentMapRenderViews_WriteCombined );

            TArrayView<ShaderTypes::RenderView> forwardShadingRenderViews_WriteCombined = renderViews_WriteCombined.subspan( pRenderViewport->m_forwardShadingRenderViewsOffset, pRenderViewport->m_numForwardShadingRenderViews );
            m_renderPass_ForwardShading.UpdateRenderViews( pRenderViewport, forwardShadingRenderViews_WriteCombined );

            #if EE_DEVELOPMENT_TOOLS
            if ( pRenderViewport->m_numEditorOutlineRenderViews > 0 )
            {
                TArrayView<ShaderTypes::RenderView> editorOutlineRenderViews_WriteCombined = renderViews_WriteCombined.subspan( pRenderViewport->m_editorOutlineRenderViewsOffset, pRenderViewport->m_numEditorOutlineRenderViews );
                m_renderPass_EditorOutline.UpdateRenderViews( pRenderViewport, editorOutlineRenderViews_WriteCombined );
            }
            #endif
        }

        {
            TArrayView<ShaderTypes::RenderView> renderViews_WriteCombined = TArrayView<ShaderTypes::RenderView>( static_cast<ShaderTypes::RenderView*>( pRenderViewport->m_renderViewBuffers[frameIndex]->m_pMappedAddress_WriteCombined ), pRenderViewport->m_numRenderViews );
            TArrayView<ShaderTypes::RenderView> cascadedShadowMapRenderViews_WriteCombined = renderViews_WriteCombined.subspan( pRenderViewport->m_cascadedShadowRenderViewsOffset, pRenderViewport->m_numCascadedShadowRenderViews );
            TArrayView<ShaderTypes::CascadedShadow> cascadedShadows_WriteCombined = TArrayView<ShaderTypes::CascadedShadow>( static_cast<ShaderTypes::CascadedShadow*>( pRenderViewport->m_cascadedShadowBuffers[frameIndex]->m_pMappedAddress_WriteCombined ), pRenderViewport->m_numCascadedShadowRenderViews );

            size_t cascadedShadowPassIndex = 0;
            for ( DirectionalLightComponent const* pDirectionalLightComponent : pRenderWorldSystem->m_directionalLightComponents )
            {
                if ( pDirectionalLightComponent->GetShadowed() )
                {
                    Float3 lightDirection = -pDirectionalLightComponent->GetLightDirection();

                    m_renderPass_CascadedShadows[cascadedShadowPassIndex].UpdateRenderViews
                    (
                        pRenderViewport,
                        lightDirection,
                        &cascadedShadows_WriteCombined[cascadedShadowPassIndex],
                        cascadedShadowMapRenderViews_WriteCombined.subspan( cascadedShadowPassIndex * CascadedShadowPass::NumShadowCascades, CascadedShadowPass::NumShadowCascades )
                    );

                    cascadedShadowPassIndex++;
                }
            }

            EE_ASSERT( cascadedShadowPassIndex == pRenderWorldSystem->m_numShadowCastingDirectionalLights );
        }

        // Initialize render buckets
        //-------------------------------------------------------------------------
        TArrayView<ShaderTypes::RenderBucket> renderBucketMemory_WriteCombined = TArrayView<ShaderTypes::RenderBucket>( static_cast<ShaderTypes::RenderBucket*>( pRenderViewport->m_renderBucketBuffers[frameIndex]->m_pMappedAddress_WriteCombined ), pRenderViewport->m_numRenderBuckets );

        uint32_t renderBucketIndex = 0;
        ForEachRenderBucket( pRenderWorldSystem->m_numShadowCastingDirectionalLights, enableEditorOutline, [renderBucketMemory_WriteCombined, &renderBucketIndex] ( MaterialShaderRenderBucket& renderBucket )
        {
            ShaderTypes::RenderBucket deviceRenderBucket = {};
            deviceRenderBucket.m_drawCounterBuffer = RHI::GetBufferHandle( renderBucket.m_pDrawCounterBuffer, RHI::DescriptorTypeFlags::RWBuffer );
            deviceRenderBucket.m_drawArgumentBuffer = RHI::GetBufferHandle( renderBucket.m_drawArgumentBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );

            renderBucketMemory_WriteCombined[renderBucketIndex] = deviceRenderBucket;
            renderBucketIndex++;
        } );

        EE_ASSERT( renderBucketIndex == pRenderViewport->m_numRenderBuckets );

        // Update constant buffers
        //-------------------------------------------------------------------------

        Math::ViewVolume const& mainCameraViewVolume = pRenderViewport->GetViewVolume();

        alignas( 32 ) ShaderTypes::GlobalParameters globalParameters = {};

        mainCameraViewVolume.GetViewPosition().StoreFloat3( globalParameters.m_cameraPosition );
        mainCameraViewVolume.GetViewForwardVector().StoreFloat3( globalParameters.m_cameraForwardDirection );
        mainCameraViewVolume.GetViewRightVector().StoreFloat3( globalParameters.m_cameraRightDirection );
        mainCameraViewVolume.GetViewUpVector().StoreFloat3( globalParameters.m_cameraUpDirection );

        globalParameters.m_rendererGlobalFlags = ShaderTypes::RENDERER_GLOBAL_FLAG_NONE;

        globalParameters.m_viewportSize[0] = float( pRenderViewport->GetSize().m_x );
        globalParameters.m_viewportSize[1] = float( pRenderViewport->GetSize().m_y );

        globalParameters.m_viewportSize[2] = 1.0F / globalParameters.m_viewportSize[0];
        globalParameters.m_viewportSize[3] = 1.0F / globalParameters.m_viewportSize[1];

        globalParameters.m_skinningTransformBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetSkinningTransformBufferHandle();
        globalParameters.m_renderBucketBuffer = RHI::GetBufferHandle( pRenderViewport->m_renderBucketBuffers[frameIndex], RHI::DescriptorTypeFlags::Buffer );

        globalParameters.m_shaderDataBuffer = m_pRenderSystem->GetShaderDataBufferHandle();
        globalParameters.m_renderViewBuffer = RHI::GetBufferHandle( pRenderViewport->m_renderViewBuffers[frameIndex], RHI::DescriptorTypeFlags::Buffer );

        globalParameters.m_meshInstanceRootBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceRootBufferHandle();
        globalParameters.m_meshInstanceRootPageBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceRootPageBufferHandle( frameIndex );
        globalParameters.m_cascadedShadowBuffer = RHI::GetBufferHandle( pRenderViewport->m_cascadedShadowBuffers[frameIndex], RHI::DescriptorTypeFlags::Buffer );
        if ( enableSSAO )
        {
            globalParameters.m_ssaoTexture = RHI::GetTextureHandle( pRenderViewport->m_GTAO_resultTexture, RHI::DescriptorTypeFlags::Texture, 0 );
        }
        globalParameters.m_dfgTexture = m_renderPass_GlobalEnvironmentMap.GetDFGTextureHandle();
        globalParameters.m_radianceTexture = pRenderWorldSystem->GetRadianceTextureHandle();

        // Directional lights
        globalParameters.m_directionalLightBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetDirectionalLightBufferHandle();
        globalParameters.m_directionalLightPageBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetDirectionalLightPageBufferHandle( frameIndex );
        globalParameters.m_numDirectionalLightPages = pRenderWorldSystem->m_deviceRenderWorld.GetNumDirectionalLightPages();

        // Point lights
        globalParameters.m_pointLightBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetPointLightBufferHandle();
        globalParameters.m_pointLightPageBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetPointLightPageBufferHandle( frameIndex );
        globalParameters.m_numPointLightPages = pRenderWorldSystem->m_deviceRenderWorld.GetNumPointLightPages();

        // Spot lights
        globalParameters.m_spotLightBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetSpotLightBufferHandle();
        globalParameters.m_spotLightPageBuffer = pRenderWorldSystem->m_deviceRenderWorld.GetSpotLightPageBufferHandle( frameIndex );
        globalParameters.m_numSpotLightPages = pRenderWorldSystem->m_deviceRenderWorld.GetNumSpotLightPages();

        // Light culling
        globalParameters.m_lightCulling_MinCellSize = m_pRenderGlobalSettings->m_lightCullingMinCellSize;
        globalParameters.m_lightCulling_SpatialHashLow = m_LightCulling_SpatialHash.GetPackedHandleLow();
        globalParameters.m_lightCulling_SpatialHashHigh = m_LightCulling_SpatialHash.GetPackedHandleHigh( m_pRenderGlobalSettings->m_lightCullingMinCellSize );

        // Picking
        #if EE_DEVELOPMENT_TOOLS
        if ( pRenderViewport->IsPickingEnabled() )
        {
            auto UpdateInstancePickingDistancesBuffer = [pContextRHI, this] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                m_pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters instancePickingDistanceBufferParameters = {};
                instancePickingDistanceBufferParameters.m_bufferSize = newBufferSize;
                instancePickingDistanceBufferParameters.m_format = RHI::DataFormat::R32_SFloat;
                instancePickingDistanceBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
                instancePickingDistanceBufferParameters.m_debugName.sprintf( "Instance Root Picking Distances Buffer" );

                return RHI::CreateBuffer( pContextRHI, instancePickingDistanceBufferParameters );
            };

            size_t instanceDistancesBufferSize = size_t( pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceRootPages() ) * 64 * sizeof( float );
            pRenderViewport->m_instancePickingDistancesBuffer.UpdateDeviceResources( instanceDistancesBufferSize, UpdateInstancePickingDistancesBuffer );

            pRenderViewport->m_instancePickingResultsBuffer.UpdateBuffers( m_pRenderSystem, frameIndex, sizeof( ShaderTypes::PickingResult ), RHI::DescriptorTypeFlags::RWBuffer );

            globalParameters.m_instancePickingResultsBuffer = pRenderViewport->m_instancePickingResultsBuffer.GetAppendBufferHandle();
            globalParameters.m_instancePickingDistancesBuffer = RHI::GetBufferHandle( pRenderViewport->m_instancePickingDistancesBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );

            Vector mouseClipSpace = pRenderViewport->ScreenSpaceToClipSpace( pRenderViewport->m_lastKnownPickingMousePosition );
            float pickingRadiusClipSpace = float( pRenderViewport->m_lastKnownPickingPixelRadius ) / pRenderViewport->GetDimensions().GetMax();

            globalParameters.m_pickingInput[0] = mouseClipSpace.GetX();
            globalParameters.m_pickingInput[1] = mouseClipSpace.GetY();
            globalParameters.m_pickingInput[2] = pickingRadiusClipSpace;
            globalParameters.m_pickingInput[3] = 0.0F;

            globalParameters.m_pickingEnabled = 1;
        }
        else
        {
            globalParameters.m_pickingEnabled = 0;
        }
        #else
        globalParameters.m_pickingEnabled = 0;
        #endif

        globalParameters.m_mainCameraRenderView = pRenderViewport->m_forwardShadingRenderViewsOffset;
        globalParameters.m_numRenderViewBucketsPerView = pRenderViewport->m_numRenderViewBucketsPerView;
        globalParameters.m_numRenderViews = pRenderViewport->m_numRenderViews;
        globalParameters.m_numRenderBuckets = pRenderViewport->m_numRenderBuckets;

        // Editor selection outline
        #if EE_DEVELOPMENT_TOOLS
        globalParameters.m_meshInstanceRootOutlineBuffer = pRenderWorldSystem->GetMeshInstanceRootOutlineBufferHandle();
        globalParameters.m_editorOutlineRenderViewIndex = pRenderViewport->m_editorOutlineRenderViewsOffset;
        globalParameters.m_editorOutlineEnabled = enableEditorOutline ? 1 : 0;
        #else
        globalParameters.m_editorOutlineEnabled = 0;
        #endif

        // Misc
        globalParameters.m_irradianceTexture = pRenderWorldSystem->GetIrradianceTextureHandle();
        globalParameters.m_radianceTextureMipLevels = pRenderWorldSystem->GetRadianceTextureMipLevels();
        globalParameters.m_deviceAddress = pRenderViewport->m_globalParametersBuffers[frameIndex]->m_deviceAddress;

        if ( !enableSSAO )
        {
            globalParameters.m_rendererGlobalFlags |= ShaderTypes::RENDERER_GLOBAL_FLAG_DISABLE_SSAO;
        }

        #if EE_DEVELOPMENT_TOOLS
        globalParameters.m_shaderDebugDrawBuffer = RHI::GetBufferHandle( pRenderViewport->m_shaderDebugDrawBuffers[frameIndex], RHI::DescriptorTypeFlags::Buffer );
        #endif

        #if EE_DEVELOPMENT_TOOLS
        auto const* pVisSettings = pRenderViewport->GetViewportSettings<RenderViewportSettings>();
        globalParameters.m_rendererDebugVisualizationMode = uint8_t( pVisSettings->m_visualizationMode );

        globalParameters.m_rendererDebugFlags = ShaderTypes::RENDERER_DEBUG_FLAG_NONE;
        if ( pVisSettings->m_showWireframe )
        {
            globalParameters.m_rendererDebugFlags |= ShaderTypes::RENDERER_DEBUG_FLAG_SHOW_WIREFRAME;
        }
        if ( pVisSettings->m_showShadowCascades )
        {
            globalParameters.m_rendererDebugFlags |= ShaderTypes::RENDERER_DEBUG_FLAG_SHOW_SHADOW_CASCADES;
        }
        #endif

        Memory::CopyToWriteCombined( pRenderViewport->m_globalParametersBuffers[frameIndex]->m_pMappedAddress_WriteCombined, &globalParameters, sizeof( globalParameters ) );
    }

    void ForwardShadingRenderer::UpdateWorldDeviceResources( UpdateContext const& ctx, EntityWorld* pWorld )
    {
        EE_PROFILE_FUNCTION_RENDER();

        RenderWorldSystem* pRenderWorldSystem = pWorld->GetWorldSystem<RenderWorldSystem>();

        // Update world
        //-------------------------------------------------------------------------

        pRenderWorldSystem->UpdateDeviceResources();

        // Update directional lights
        //-------------------------------------------------------------------------

        size_t numShadowCastingDirectionalLights = 0;
        for ( DirectionalLightComponent const* pDirectionalLightComponent : pRenderWorldSystem->m_directionalLightComponents )
        {
            if ( pDirectionalLightComponent->GetShadowed() )
            {
                CascadedShadowPass* pCascadedShadowPass = nullptr;
                if ( numShadowCastingDirectionalLights >= m_renderPass_CascadedShadows.size() )
                {
                    RenderPassContext context = { m_pRenderSystem, m_pRenderGlobalSettings, m_materialShaderPipelineBuckets, };

                    pCascadedShadowPass = &m_renderPass_CascadedShadows.emplace_back();
                    pCascadedShadowPass->Initialize( context );
                }
                else
                {
                    pCascadedShadowPass = &m_renderPass_CascadedShadows[numShadowCastingDirectionalLights];
                }

                numShadowCastingDirectionalLights++;
            }
        }
        EE_ASSERT( numShadowCastingDirectionalLights == pRenderWorldSystem->m_numShadowCastingDirectionalLights );

        // Update all render passes
        //-------------------------------------------------------------------------

        m_renderPass_GlobalEnvironmentMap.UpdateDeviceResources( m_pRenderSystem, m_materialShaderPipelineBuckets, pRenderWorldSystem->m_deviceRenderWorld );

        for ( size_t cascadedShadowPassIndex = 0; cascadedShadowPassIndex < numShadowCastingDirectionalLights; ++cascadedShadowPassIndex )
        {
            m_renderPass_CascadedShadows[cascadedShadowPassIndex].UpdateDeviceResources( m_pRenderSystem, pRenderWorldSystem->m_deviceRenderWorld );
        }
        m_renderPass_ForwardShading.UpdateDeviceResources( m_pRenderSystem, m_materialShaderPipelineBuckets, pRenderWorldSystem->m_deviceRenderWorld );

        #if EE_DEVELOPMENT_TOOLS
        m_renderPass_DebugDraw.UpdateDeviceResources( m_pRenderSystem );

        m_renderPass_EditorOutline.UpdateDeviceResources( m_pRenderSystem, pRenderWorldSystem->m_deviceRenderWorld );
        #endif

        // Per-shader culling buffers
        //-------------------------------------------------------------------------

        EE_ASSERT( m_materialShaderPipelineBuckets.size() == pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools() );

        RenderSystem* pRenderSystem = m_pRenderSystem;

        for ( uint32_t shaderIndex = 0; shaderIndex < m_materialShaderPipelineBuckets.size(); ++shaderIndex )
        {
            uint32_t const instanceCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceCapacity( shaderIndex );
            uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );

            //-------------------------------------------------------------------------

            auto UpdateBuffer_InstanceVisibility = [pRenderSystem, shaderIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters instanceVisibilityBufferParameters = {};
                instanceVisibilityBufferParameters.m_bufferSize = newBufferSize;
                instanceVisibilityBufferParameters.m_bufferStride = sizeof( uint64_t );
                instanceVisibilityBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
                instanceVisibilityBufferParameters.m_debugName.sprintf( "MaterialShaderBucket Instance Visibility Buffer %u", shaderIndex );

                return RHI::CreateBuffer( pRenderSystem->GetContextRHI(), instanceVisibilityBufferParameters );
            };

            m_shaderCullingBuckets[shaderIndex].m_instanceVisibilityBuffer.UpdateDeviceResources
            (
                Math::Max( 1ULL, size_t( instanceCapacity ) * sizeof( uint64_t ) ),
                UpdateBuffer_InstanceVisibility
            );

            //-------------------------------------------------------------------------

            auto UpdateBuffer_CullingArgument = [pRenderSystem, shaderIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters cullingArgumentBufferParameters = {};
                cullingArgumentBufferParameters.m_alignment = RHI::IndirectCommandAlignment;
                cullingArgumentBufferParameters.m_descriptorTypes = TBitFlags<RHI::DescriptorTypeFlags>( RHI::DescriptorTypeFlags::IndirectArgumentBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                cullingArgumentBufferParameters.m_bufferSize = newBufferSize;
                cullingArgumentBufferParameters.m_bufferStride = sizeof( ShaderTypes::ClusterCullingArgument );
                cullingArgumentBufferParameters.m_debugName.sprintf( "MaterialShaderBucket ClusterCullingArgument Buffer %u", shaderIndex );

                return RHI::CreateBuffer( pRenderSystem->GetContextRHI(), cullingArgumentBufferParameters );
            };

            size_t const maxNumCullingArguments = size_t( clusterCapacity ) / ( size_t( RHI::Limits::MaxDispatchSize ) * 128 ) + 1;

            m_shaderCullingBuckets[shaderIndex].m_cullingArgumentBuffer.UpdateDeviceResources
            (
                Math::Max( 1ULL, maxNumCullingArguments * sizeof( ShaderTypes::ClusterCullingArgument ) ),
                UpdateBuffer_CullingArgument
            );

            //-------------------------------------------------------------------------

            auto UpdateBuffer_ClusterCullingWorkBuffer = [pRenderSystem, shaderIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters cullingWorkBufferParameters = {};
                cullingWorkBufferParameters.m_bufferSize = newBufferSize;
                cullingWorkBufferParameters.m_bufferStride = sizeof( ShaderTypes::ClusterCullingWorkEntry );
                cullingWorkBufferParameters.m_descriptorTypes.SetMultipleFlags( RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
                cullingWorkBufferParameters.m_debugName.sprintf( "Shader Culling Bucket Cluster Culling Work Buffer %u", shaderIndex );

                return RHI::CreateBuffer( pRenderSystem->GetContextRHI(), cullingWorkBufferParameters );
            };

            m_shaderCullingBuckets[shaderIndex].m_clusterCullingWorkBuffer.UpdateDeviceResources
            (
                Math::Max( 1ULL, size_t( clusterCapacity ) * sizeof( ShaderTypes::ClusterCullingWorkEntry ) ),
                UpdateBuffer_ClusterCullingWorkBuffer
            );

            //-------------------------------------------------------------------------

            auto UpdateBuffer_DrawCompactionArgument = [pRenderSystem, shaderIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
            {
                pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                RHI::BufferParameters drawCompactionArgumentBufferParameters = {};
                drawCompactionArgumentBufferParameters.m_alignment = RHI::IndirectCommandAlignment;
                drawCompactionArgumentBufferParameters.m_descriptorTypes = TBitFlags<RHI::DescriptorTypeFlags>( RHI::DescriptorTypeFlags::IndirectArgumentBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                drawCompactionArgumentBufferParameters.m_bufferSize = newBufferSize;
                drawCompactionArgumentBufferParameters.m_bufferStride = sizeof( ShaderTypes::DrawCompactionArgument );
                drawCompactionArgumentBufferParameters.m_debugName.sprintf( "MaterialShaderBucket DrawCompactionArgument Buffer %u", shaderIndex );

                return RHI::CreateBuffer( pRenderSystem->GetContextRHI(), drawCompactionArgumentBufferParameters );
            };

            m_shaderCullingBuckets[shaderIndex].m_drawCompactionArgumentBuffer.UpdateDeviceResources
            (
                Math::Max( 1ULL, maxNumCullingArguments * sizeof( ShaderTypes::DrawCompactionArgument ) ),
                UpdateBuffer_DrawCompactionArgument
            );
        }
    }

    uint64_t ForwardShadingRenderer::DispatchWorld( UpdateContext const& updateContext, RenderViewport const* pRenderViewport, EntityWorld* pWorld, uint64_t waitSemaphore )
    {
        EE_PROFILE_FUNCTION_RENDER();

        //-------------------------------------------------------------------------

        bool const enableAsyncCompute = m_pRenderGlobalSettings->m_enableAsyncCompute;

        bool const enableSMAA = m_pRenderGlobalSettings->m_enableSMAA;

        bool const enableSSAO = m_pRenderGlobalSettings->m_enableSSAO;
        bool const enableSSAOLowResolution = m_pRenderGlobalSettings->m_enableSSAOLowResolution;
        bool const enableDepthDownsample = ( enableSSAO && enableSSAOLowResolution ) || false;

        uint32_t const frameIndex = m_pRenderSystem->GetFrameIndex();
        RHI::CommandBuffer* pCommandBuffer = pRenderViewport->m_pWindow->GetActiveCommandBuffer( frameIndex );

        if ( enableAsyncCompute )
        {
            pCommandBuffer = pRenderViewport->m_pWindow->AcquireComputeCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
        }

        RenderWorldSystem* pRenderWorldSystem = pWorld->GetWorldSystem<RenderWorldSystem>();
        pRenderWorldSystem->m_deviceRenderWorld.DispatchWorldUpdate( pCommandBuffer, frameIndex );
        pRenderWorldSystem->m_deviceRenderWorld.WaitForCopyTasks( m_pRenderSystem );

        if ( enableAsyncCompute )
        {
            // Wait for the PREVIOUS frame's shading to finish, allows to overlap with workload after previous frame shading
            RHI::QueueDeviceWait( m_pRenderSystem->GetComputeQueue(), m_pRenderSystem->GetGraphicsQueue(), m_signalSemaphores_ShadingPass[( frameIndex + RHI::MaxPendingFrames - 1 ) % RHI::MaxPendingFrames] );

            // Wait for the previous world rendering to be completed when we have multiple viewports
            RHI::QueueDeviceWait( m_pRenderSystem->GetComputeQueue(), m_pRenderSystem->GetGraphicsQueue(), waitSemaphore );

            m_signalSemaphores_WorldUpdate[frameIndex] = SubmitComputeCommandBuffer( eastl::move( pCommandBuffer ) );

            return m_signalSemaphores_WorldUpdate[frameIndex];
        }

        return 0;
    }

    uint64_t ForwardShadingRenderer::DrawWorldToViewport( UpdateContext const& ctx, RenderViewport const* pRenderViewport, EntityWorld const* pWorld, uint64_t waitSemaphore )
    {
        EE_ASSERT( pRenderViewport != nullptr );
        EE_ASSERT( pWorld != nullptr );
        EE_ASSERT( pRenderViewport->IsValid() );

        EE_PROFILE_FUNCTION_RENDER();

        RenderWorldSystem const* pRenderWorldSystem = pWorld->GetWorldSystem<RenderWorldSystem>();

        //-------------------------------------------------------------------------

        uint32_t const frameIndex = m_pRenderSystem->GetFrameIndex();

        //-------------------------------------------------------------------------

        bool const enableAsyncCompute = m_pRenderGlobalSettings->m_enableAsyncCompute;

        bool const enableSMAA = m_pRenderGlobalSettings->m_enableSMAA;

        bool const enableSSAO = m_pRenderGlobalSettings->m_enableSSAO;
        bool const enableSSAOLowResolution = m_pRenderGlobalSettings->m_enableSSAOLowResolution;

        bool const enableDepthDownsample = ( enableSSAO && enableSSAOLowResolution );

        //-------------------------------------------------------------------------

        RHI::BufferHandle   renderViewBufferHandle = RHI::GetBufferHandle( pRenderViewport->m_renderViewBuffers[frameIndex], RHI::DescriptorTypeFlags::Buffer );
        RHI::Buffer*        pGlobalParametersBuffer = pRenderViewport->m_globalParametersBuffers[frameIndex];

        //-------------------------------------------------------------------------

        Memory::WriteCombinedBarrier();

        RHI::CommandBuffer* pCommandBuffer_GeometryCulling = pRenderViewport->m_pWindow->GetActiveCommandBuffer( frameIndex );
        uint64_t signalSemaphore_GeometryCulling = 0;

        if ( enableAsyncCompute )
        {
            pCommandBuffer_GeometryCulling = pRenderViewport->m_pWindow->AcquireComputeCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
        }

        //-------------------------------------------------------------------------

        EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Clear Buffers" );

            #if EE_DEVELOPMENT_TOOLS
            m_renderPass_DebugDraw.ClearBuffers( pCommandBuffer_GeometryCulling, frameIndex );
            #endif

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                RHI::CmdClearBuffer( pCommandBuffer_GeometryCulling, m_shaderCullingBuckets[shaderIndex].m_pCullingCounterBuffer, 0 );
                RHI::CmdClearBuffer( pCommandBuffer_GeometryCulling, m_shaderCullingBuckets[shaderIndex].m_pDrawClusterCountersBuffer, 0 );
                RHI::CmdClearBuffer( pCommandBuffer_GeometryCulling, m_shaderCullingBuckets[shaderIndex].m_pDrawClusterScatterOffsetsBuffer, 0 );
            }

            ForEachRenderBucket( pRenderWorldSystem->m_numShadowCastingDirectionalLights, pRenderViewport->m_numEditorOutlineRenderViews > 0, [pCommandBuffer_GeometryCulling] ( MaterialShaderRenderBucket& renderBucket )
            {
                RHI::CmdClearBuffer( pCommandBuffer_GeometryCulling, renderBucket.m_pDrawCounterBuffer, 0 );
            } );

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::AllShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Instance Culling" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pInstanceCullingShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const instanceCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceCapacity( shaderIndex );
                EE_ASSERT( instanceCapacity > 0 );

                ShaderTypes::InstanceCullingRootConstants instanceCullingRootConstants = {};
                instanceCullingRootConstants.m_instanceCapacity = instanceCapacity;
                instanceCullingRootConstants.m_instanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );
                instanceCullingRootConstants.m_instancePageBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstancePageBuffer( shaderIndex, frameIndex ), RHI::DescriptorTypeFlags::Buffer );
                instanceCullingRootConstants.m_instanceVisibilityBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_instanceVisibilityBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );

                uint32_t const numInstanceGroups = ( instanceCapacity + 127 ) / 128;
                uint32_t const numDispatchSplits = ( numInstanceGroups + RHI::Limits::MaxDispatchSize - 1 ) / RHI::Limits::MaxDispatchSize;
                for ( uint32_t dispatchIndex = 0; dispatchIndex < numDispatchSplits; ++dispatchIndex )
                {
                    uint32_t const groupOffset = dispatchIndex * RHI::Limits::MaxDispatchSize;

                    instanceCullingRootConstants.m_instanceOffset = groupOffset * 128;

                    RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, &instanceCullingRootConstants, sizeof( instanceCullingRootConstants ) );
                    RHI::CmdSetRootParameter( pCommandBuffer_GeometryCulling, 1, pGlobalParametersBuffer, 0 );
                    RHI::CmdDispatchCompute( pCommandBuffer_GeometryCulling, Math::Min( numInstanceGroups - groupOffset, uint32_t( RHI::Limits::MaxDispatchSize ) ), 1, 1 );
                }
            }
        }

        RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Instance Compaction" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pCullingCompactionShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const instanceCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceCapacity( shaderIndex );
                EE_ASSERT( instanceCapacity > 0 );

                ShaderTypes::CullingCompactionRootConstants compactionRootConstants = {};
                compactionRootConstants.m_instanceCapacity = instanceCapacity;
                compactionRootConstants.m_instanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );
                compactionRootConstants.m_instanceVisibilityBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_instanceVisibilityBuffer.m_pBuffer, RHI::DescriptorTypeFlags::Buffer );
                compactionRootConstants.m_cullingWorkBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_clusterCullingWorkBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                compactionRootConstants.m_workCounterBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pCullingCounterBuffer, RHI::DescriptorTypeFlags::RWBuffer );

                uint32_t const numInstanceGroups = ( instanceCapacity + 127 ) / 128;
                uint32_t const numDispatches = ( numInstanceGroups + RHI::Limits::MaxDispatchSize - 1 ) / RHI::Limits::MaxDispatchSize;
                for ( uint32_t dispatchIndex = 0; dispatchIndex < numDispatches; ++dispatchIndex )
                {
                    uint32_t const groupOffset = dispatchIndex * RHI::Limits::MaxDispatchSize;

                    compactionRootConstants.m_instanceOffset = groupOffset * 128;

                    RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, &compactionRootConstants, sizeof( compactionRootConstants ) );
                    RHI::CmdDispatchCompute( pCommandBuffer_GeometryCulling, Math::Min( numInstanceGroups - groupOffset, uint32_t( RHI::Limits::MaxDispatchSize ) ), 1, 1 );
                }
            }

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Culling Argument Generation" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pCullingArgumentGenerationShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );
                EE_ASSERT( clusterCapacity > 0 );

                size_t const maxNumCullingArguments = size_t( clusterCapacity ) / ( size_t( RHI::MaxDispatchSize ) * 128 ) + 1;

                ShaderTypes::CullingArgumentGenerationRootConstants argumentGenerationRootConstants = {};
                argumentGenerationRootConstants.m_instanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );
                argumentGenerationRootConstants.m_instanceVisibilityBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_instanceVisibilityBuffer.m_pBuffer, RHI::DescriptorTypeFlags::Buffer );
                argumentGenerationRootConstants.m_cullingWorkBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_clusterCullingWorkBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_workCounterBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pCullingCounterBuffer, RHI::DescriptorTypeFlags::Buffer );
                argumentGenerationRootConstants.m_clusterToInstanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetClusterToInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );
                argumentGenerationRootConstants.m_cullingArgumentBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_cullingArgumentBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_drawCompactionArgumentBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_drawCompactionArgumentBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_drawClusterBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_drawClusterBuffer.m_pBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_drawClusterBaseOffsetsBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pDrawClusterBaseOffsetsBuffer, RHI::DescriptorTypeFlags::Buffer );
                argumentGenerationRootConstants.m_drawClusterCountersBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pDrawClusterCountersBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_drawClusterScatterOffsetsBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pDrawClusterScatterOffsetsBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                argumentGenerationRootConstants.m_shaderIndex = shaderIndex;
                argumentGenerationRootConstants.m_numArguments = uint32_t( maxNumCullingArguments );

                RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, &argumentGenerationRootConstants, sizeof( argumentGenerationRootConstants ) );
                RHI::CmdSetRootParameter( pCommandBuffer_GeometryCulling, 1, pGlobalParametersBuffer, 0 );
                RHI::CmdDispatchCompute( pCommandBuffer_GeometryCulling, uint32_t( ( maxNumCullingArguments + 63 ) / 64 ), 1, 1 );
            }

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Cluster Culling" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pClusterCullingShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                RHI::Buffer* const pCullingArgumentBuffer = m_shaderCullingBuckets[shaderIndex].m_cullingArgumentBuffer.m_pBuffer;
                RHI::CmdBarrier( pCommandBuffer_GeometryCulling, pCullingArgumentBuffer, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ExecuteIndirect, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::IndirectArgument );
            }

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );
                EE_ASSERT( clusterCapacity > 0 );

                size_t const maxNumCullingArguments = size_t( clusterCapacity ) / ( size_t( RHI::MaxDispatchSize ) * 128 ) + 1;

                RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, nullptr, sizeof( ShaderTypes::ClusterCullingRootConstants ) );
                RHI::CmdExecuteIndirect
                (
                    pCommandBuffer_GeometryCulling, m_pClusterCullingShader->m_pCommandSignature,
                    uint32_t( maxNumCullingArguments ),
                    m_shaderCullingBuckets[shaderIndex].m_cullingArgumentBuffer.m_pBuffer, 0,
                    nullptr, 0
                );
            }

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Draw Argument Generation" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pDrawArgumentGenerationShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );
                EE_ASSERT( clusterCapacity > 0 );

                ShaderTypes::DrawArgumentGenerationRootConstants drawArgumentGenerationRootConstants = {};
                drawArgumentGenerationRootConstants.m_shaderIndex = shaderIndex;
                drawArgumentGenerationRootConstants.m_drawClusterCountersBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pDrawClusterCountersBuffer, RHI::DescriptorTypeFlags::Buffer );
                drawArgumentGenerationRootConstants.m_drawClusterBaseOffsetsBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_pDrawClusterBaseOffsetsBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                drawArgumentGenerationRootConstants.m_drawClusterBuffer = RHI::GetBufferHandle( m_shaderCullingBuckets[shaderIndex].m_drawClusterBuffer.m_pBuffer, RHI::DescriptorTypeFlags::Buffer );
                drawArgumentGenerationRootConstants.m_clusterToInstanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetClusterToInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );
                drawArgumentGenerationRootConstants.m_instanceBuffer = RHI::GetBufferHandle( pRenderWorldSystem->m_deviceRenderWorld.GetMeshInstanceBuffer( shaderIndex ), RHI::DescriptorTypeFlags::Buffer );

                RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, &drawArgumentGenerationRootConstants, sizeof( drawArgumentGenerationRootConstants ) );
                RHI::CmdSetRootParameter( pCommandBuffer_GeometryCulling, 1, pGlobalParametersBuffer, 0 );
                RHI::CmdDispatchCompute( pCommandBuffer_GeometryCulling, 1, 1, 1 );
            }

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        //-------------------------------------------------------------------------

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GeometryCulling, "Draw Compaction" );

            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetPipeline( pCommandBuffer_GeometryCulling, m_pDrawCompactionShader->m_pPipeline );

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                RHI::Buffer* const pDrawCompactionArgumentBuffer = m_shaderCullingBuckets[shaderIndex].m_drawCompactionArgumentBuffer.m_pBuffer;
                RHI::CmdBarrier( pCommandBuffer_GeometryCulling, pDrawCompactionArgumentBuffer, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ExecuteIndirect, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::IndirectArgument );
            }

            for ( uint32_t shaderIndex = 0; shaderIndex < pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceShaderPools(); ++shaderIndex )
            {
                uint32_t const clusterCapacity = pRenderWorldSystem->m_deviceRenderWorld.GetClusterCapacity( shaderIndex );
                EE_ASSERT( clusterCapacity > 0 );

                size_t const maxNumCullingArguments = size_t( clusterCapacity ) / ( size_t( RHI::MaxDispatchSize ) * 128 ) + 1;

                RHI::CmdSetRootConstants( pCommandBuffer_GeometryCulling, 0, nullptr, sizeof( ShaderTypes::DrawCompactionRootConstants ) );
                RHI::CmdExecuteIndirect
                (
                    pCommandBuffer_GeometryCulling, m_pDrawCompactionShader->m_pCommandSignature,
                    uint32_t( maxNumCullingArguments ),
                    m_shaderCullingBuckets[shaderIndex].m_drawCompactionArgumentBuffer.m_pBuffer, 0,
                    nullptr, 0
                );
            }

            RHI::CmdBarrier( pCommandBuffer_GeometryCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::UnorderedAccess );
        }

        if ( enableAsyncCompute )
        {
            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            // Wait for the PREVIOUS frame's shading to finish, allows to overlap with workload after previous frame shading
            RHI::QueueDeviceWait( m_pRenderSystem->GetComputeQueue(), m_pRenderSystem->GetGraphicsQueue(), m_signalSemaphores_ShadingPass[( frameIndex + RHI::MaxPendingFrames - 1 ) % RHI::MaxPendingFrames] );

            // Wait for the previous world rendering to be completed when we have multiple viewports
            RHI::QueueDeviceWait( m_pRenderSystem->GetComputeQueue(), m_pRenderSystem->GetGraphicsQueue(), waitSemaphore );

            signalSemaphore_GeometryCulling = SubmitComputeCommandBuffer( eastl::move( pCommandBuffer_GeometryCulling ) );
        }

        // Spatial hash light culling - overlap with depth prepass
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_LightCulling = pRenderViewport->m_pWindow->GetActiveCommandBuffer( frameIndex );
        uint64_t signalSemaphore_LightCulling = 0;

        if ( enableAsyncCompute )
        {
            pCommandBuffer_LightCulling = pRenderViewport->m_pWindow->AcquireComputeCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
        }

        {
            EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_LightCulling, "Light Culling" );

            m_LightCulling_SpatialHash.Clear( pCommandBuffer_LightCulling, frameIndex );

            RHI::CmdBarrier( pCommandBuffer_LightCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::UnorderedAccess );

            DeviceSpatialHashDispatchParameters dispatchParameters[DeviceSpatialHash::MaxLODs] = {};
            DeviceSpatialHash::ComputeDispatchParameters
            (
                m_pRenderGlobalSettings->m_lightCullingInitialDispatchX,
                m_pRenderGlobalSettings->m_lightCullingInitialDispatchY,
                m_pRenderGlobalSettings->m_lightCullingInitialDispatchZ,
                m_LightCulling_SpatialHash.m_numLODs,
                m_LightCulling_SpatialHash.m_borderCells,
                dispatchParameters
            );

            for ( int lod = int( m_LightCulling_SpatialHash.m_numLODs - 1 ); lod >= 0; --lod )
            {
                ShaderTypes::LightCulling_CullLightsRootConstants cullLightsRootConstants = {};
                cullLightsRootConstants.m_cellLevel = uint32_t( lod );
                cullLightsRootConstants.m_cellOffsetX = dispatchParameters[lod].m_dispatchOffset[0];
                cullLightsRootConstants.m_cellOffsetY = dispatchParameters[lod].m_dispatchOffset[1];
                cullLightsRootConstants.m_cellOffsetZ = dispatchParameters[lod].m_dispatchOffset[2];
                cullLightsRootConstants.m_numLODs = m_LightCulling_SpatialHash.m_numLODs;

                RHI::CmdSetPipeline( pCommandBuffer_LightCulling, m_pLightCulling_CullLightsShader->m_pPipeline );
                RHI::CmdSetRootConstants( pCommandBuffer_LightCulling, 0, &cullLightsRootConstants, sizeof( cullLightsRootConstants ) );
                RHI::CmdSetRootParameter( pCommandBuffer_LightCulling, 1, pGlobalParametersBuffer, 0 );
                RHI::CmdDispatchCompute( pCommandBuffer_LightCulling, dispatchParameters[lod].m_dispatchSize[0], dispatchParameters[lod].m_dispatchSize[1], dispatchParameters[lod].m_dispatchSize[2] );

                if ( lod > 0 )
                {
                    RHI::CmdBarrier( pCommandBuffer_LightCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::UnorderedAccess );
                }
            }

            RHI::CmdBarrier( pCommandBuffer_LightCulling, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::AllShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::UnorderedAccess );
        }

        if ( enableAsyncCompute )
        {
            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            signalSemaphore_LightCulling = SubmitComputeCommandBuffer( eastl::move( pCommandBuffer_LightCulling ) );
        }


        // Wait for all cluster and culling dispatches
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_DepthPass = pRenderViewport->m_pWindow->GetActiveCommandBuffer( frameIndex );

        {
            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            // Clear picking buffers
            #if EE_DEVELOPMENT_TOOLS
            if ( pRenderViewport->IsPickingEnabled() )
            {
                RHI::CmdClearBuffer( pCommandBuffer_DepthPass, pRenderViewport->m_instancePickingDistancesBuffer.m_pBuffer, eastl::bit_cast<uint32_t>( 1.5E+10F ) );

                pRenderViewport->m_instancePickingResultsBuffer.Clear( pCommandBuffer_DepthPass, frameIndex );
            }
            #endif

            ForEachRenderBucket( pRenderWorldSystem->m_numShadowCastingDirectionalLights, pRenderViewport->m_numEditorOutlineRenderViews > 0, [pCommandBuffer_DepthPass] ( MaterialShaderRenderBucket& renderBucket )
            {
                RHI::CmdBarrier( pCommandBuffer_DepthPass, renderBucket.m_pDrawCounterBuffer, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ExecuteIndirect, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::IndirectArgument );
                RHI::CmdBarrier( pCommandBuffer_DepthPass, renderBucket.m_drawArgumentBuffer.m_pBuffer, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::ExecuteIndirect, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::IndirectArgument );
            } );
            RHI::CmdBarrier( pCommandBuffer_DepthPass, RHI::PipelineStage::ComputeShader, RHI::PipelineStage::AllShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::ShaderResource );
        }

        // Global environment map pass
        //-------------------------------------------------------------------------

        m_renderPass_GlobalEnvironmentMap.PrecomputeDFG( pCommandBuffer_DepthPass );
        if ( pRenderWorldSystem->m_needUpdateGlobalEnvironmentMap )
        {
            m_renderPass_GlobalEnvironmentMap.DrawAndFilterGlobalEnvironmentMap
            (
                m_materialShaderPipelineBuckets,
                pGlobalParametersBuffer,
                pCommandBuffer_DepthPass,
                pRenderWorldSystem->m_pRadianceTexture,
                pRenderWorldSystem->m_pIrradianceTexture,
                renderViewBufferHandle,
                pRenderViewport->m_globalEnvironmentMapRenderViewsOffset
            );

            // TODO: Implement the concept of device generated resources in the engine.
            // Until then we const_cast the system to update the dirty flag.
            RenderWorldSystem* pRenderWorldSystemMutable = const_cast<RenderWorldSystem*>( pRenderWorldSystem );
            pRenderWorldSystemMutable->m_needUpdateGlobalEnvironmentMap = false;
        }

        // Forward shading depth pass
        //-------------------------------------------------------------------------

        m_renderPass_ForwardShading.DepthOnlyPass
        (
            m_materialShaderPipelineBuckets,
            pRenderViewport,
            m_resourceStates,
            pCommandBuffer_DepthPass
        );

        if ( enableDepthDownsample )
        {
            m_renderPass_DepthDownsample.DrawToViewport( pRenderViewport, m_resourceStates, pCommandBuffer_DepthPass, pRenderViewport->m_forwardShading_depthTexture );
        }

        #if EE_DEVELOPMENT_TOOLS
        if ( pRenderViewport->m_numEditorOutlineRenderViews > 0 )
        {
            m_renderPass_EditorOutline.DrawToViewport
            (
                m_materialShaderPipelineBuckets,
                pRenderViewport,
                m_resourceStates,
                pCommandBuffer_DepthPass
            );

            m_renderPass_DebugDraw.DrawOutlineToViewport
            (
                pRenderViewport,
                renderViewBufferHandle,
                pRenderViewport->m_forwardShadingRenderViewsOffset,
                pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceRootPages() * 64,
                m_resourceStates,
                pCommandBuffer_DepthPass,
                frameIndex
            );
        }
        #endif

        uint64_t signalSemaphore_DepthPass = 0;
        if ( enableAsyncCompute )
        {
            if ( enableSSAO )
            {
                EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

                if ( enableSSAOLowResolution )
                {
                    m_resourceStates.ReadOnly( pRenderViewport->m_depthDownsample4, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
                }
                else
                {
                    m_resourceStates.ReadOnly( pRenderViewport->m_forwardShading_depthTexture, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
                }

                m_resourceStates.Writeable( pRenderViewport->m_GTAO_prefilterDepthTexture, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::TextureState::UnorderedAccess );
                m_resourceStates.Writeable( pRenderViewport->m_GTAO_resultTextureNoisy0, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::TextureState::UnorderedAccess );
                m_resourceStates.Writeable( pRenderViewport->m_GTAO_resultTextureNoisy1, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::TextureState::UnorderedAccess );
                m_resourceStates.Writeable( pRenderViewport->m_GTAO_resultTexture, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::TextureState::UnorderedAccess );
                m_resourceStates.FlushBarriers( pCommandBuffer_DepthPass );
            }

            RHI::QueueDeviceWait( m_pRenderSystem->GetGraphicsQueue(), m_pRenderSystem->GetComputeQueue(), m_signalSemaphores_WorldUpdate[frameIndex] );
            RHI::QueueDeviceWait( m_pRenderSystem->GetGraphicsQueue(), m_pRenderSystem->GetComputeQueue(), signalSemaphore_GeometryCulling );
            signalSemaphore_DepthPass = SubmitGraphicsCommandBuffer( eastl::move( pCommandBuffer_DepthPass ) );
        }

        // SSAO
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_GTAO = nullptr;
        uint64_t signalSemaphore_GTAO = 0;

        if ( enableSSAO )
        {
            pCommandBuffer_GTAO = pCommandBuffer_DepthPass;

            if ( enableAsyncCompute )
            {
                pCommandBuffer_GTAO = pRenderViewport->m_pWindow->AcquireComputeCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
            }

            {
                EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE( pCommandBuffer_GTAO, "XeGTAO" );

                m_renderPass_GTAO.PrefilterDepth( pRenderViewport, m_resourceStates, pCommandBuffer_GTAO, frameIndex );
                m_renderPass_GTAO.ComputeNoisyResult( pRenderViewport, m_resourceStates, pCommandBuffer_GTAO, renderViewBufferHandle, pRenderViewport->m_forwardShadingRenderViewsOffset, frameIndex );
                m_renderPass_GTAO.Denoise( pRenderViewport, m_resourceStates, pCommandBuffer_GTAO, enableAsyncCompute, frameIndex );
            }

            if ( enableAsyncCompute )
            {
                EE_ASSERT( !m_resourceStates.HasPendingBarriers() );
                if ( enableSSAOLowResolution )
                {
                    m_resourceStates.ReadOnly( pRenderViewport->m_GTAO_resultTextureNoisy1, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
                }
                else
                {
                    m_resourceStates.ReadOnly( pRenderViewport->m_GTAO_resultTexture, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
                }
                m_resourceStates.FlushBarriers( pCommandBuffer_GTAO );

                RHI::QueueDeviceWait( m_pRenderSystem->GetComputeQueue(), m_pRenderSystem->GetGraphicsQueue(), signalSemaphore_DepthPass );
                signalSemaphore_GTAO = SubmitComputeCommandBuffer( eastl::move( pCommandBuffer_GTAO ) );
            }
        }

        // Cascaded shadows and directional lights
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_CascadedShadows = pCommandBuffer_DepthPass;
        uint64_t signalSemaphore_CascadedShadows = 0;

        if ( enableAsyncCompute )
        {
            pCommandBuffer_CascadedShadows = pRenderViewport->m_pWindow->AcquireGraphicsCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
        }

        size_t numCascadedShadowPasses = 0;

        for ( DirectionalLightComponent const* pDirectionalLightComponent : pRenderWorldSystem->m_directionalLightComponents )
        {
            if ( pDirectionalLightComponent->GetShadowed() )
            {
                CascadedShadowPass& cascadedShadowPass = m_renderPass_CascadedShadows[numCascadedShadowPasses];

                cascadedShadowPass.DrawShadowCascades
                (
                    m_materialShaderPipelineBuckets,
                    pRenderViewport,
                    m_resourceStates,
                    pCommandBuffer_CascadedShadows
                );

                numCascadedShadowPasses++;
            }
        }

        EE_ASSERT( numCascadedShadowPasses == pRenderWorldSystem->m_numShadowCastingDirectionalLights );

        if ( enableAsyncCompute )
        {
            signalSemaphore_CascadedShadows = SubmitGraphicsCommandBuffer( eastl::move( pCommandBuffer_CascadedShadows ) );
        }

        // Forward shading pass
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_ShadingPass = pCommandBuffer_DepthPass;
        if ( enableAsyncCompute )
        {
            pCommandBuffer_ShadingPass = pRenderViewport->m_pWindow->AcquireGraphicsCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );
        }

        if ( enableSSAO && enableSSAOLowResolution )
        {
            m_renderPass_GTAO.Upsample( pRenderViewport, m_resourceStates, pCommandBuffer_ShadingPass, frameIndex );
        }

        // Barriers
        //-------------------------------------------------------------------------
        EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

        if ( enableSSAO )
        {
            m_resourceStates.ReadOnly( pRenderViewport->m_GTAO_resultTexture, RHI::PipelineStage::PixelShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
        }

        for ( size_t cascadedShadowPassIndex = 0; cascadedShadowPassIndex < numCascadedShadowPasses; ++cascadedShadowPassIndex )
        {
            m_resourceStates.ReadOnly( m_renderPass_CascadedShadows[cascadedShadowPassIndex].m_depthTargetArray, RHI::PipelineStage::PixelShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
        }

        m_resourceStates.FlushBarriers( pCommandBuffer_ShadingPass );

        m_renderPass_ForwardShading.ShadingPass
        (
            m_materialShaderPipelineBuckets,
            pRenderViewport,
            m_resourceStates,
            pCommandBuffer_ShadingPass
        );

        // Resolve picking after shading pass
        #if EE_DEVELOPMENT_TOOLS
        if ( pRenderViewport->IsPickingEnabled() )
        {
            RHI::CmdBarrier( pCommandBuffer_ShadingPass, RHI::PipelineStage::AllShader, RHI::PipelineStage::ComputeShader, RHI::ResourceAccess::UnorderedAccess, RHI::ResourceAccess::UnorderedAccess );

            RHI::CmdSetPipeline( pCommandBuffer_ShadingPass, m_pInstancePickingResolveShader->m_pPipeline );
            RHI::CmdSetRootParameter( pCommandBuffer_ShadingPass, 0, pGlobalParametersBuffer, 0 );
            RHI::CmdDispatchCompute( pCommandBuffer_ShadingPass, pRenderWorldSystem->m_deviceRenderWorld.GetNumMeshInstanceRootPages(), 1, 1 );

            pRenderViewport->m_instancePickingResultsBuffer.CopyResults( pCommandBuffer_ShadingPass, frameIndex );
            pRenderViewport->m_instancePickingResultsBuffer.Barrier( pCommandBuffer_ShadingPass, frameIndex );
        }
        #endif

        if ( enableAsyncCompute )
        {
            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            RHI::CmdSetRenderTargets( pCommandBuffer_ShadingPass, {}, nullptr );

            RHI::QueueDeviceWait( m_pRenderSystem->GetGraphicsQueue(), m_pRenderSystem->GetComputeQueue(), signalSemaphore_LightCulling );
            RHI::QueueDeviceWait( m_pRenderSystem->GetGraphicsQueue(), m_pRenderSystem->GetComputeQueue(), signalSemaphore_GTAO );

            m_signalSemaphores_ShadingPass[frameIndex] = SubmitGraphicsCommandBuffer( eastl::move( pCommandBuffer_ShadingPass ) );
        }

        // Post processing
        //-------------------------------------------------------------------------

        RHI::CommandBuffer* pCommandBuffer_PostProcessing = pCommandBuffer_ShadingPass;

        if ( enableAsyncCompute )
        {
            EE_ASSERT( !m_resourceStates.HasPendingBarriers() );

            pCommandBuffer_PostProcessing = pRenderViewport->m_pWindow->AcquireGraphicsCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );

            Float2 const viewportSize = pRenderViewport->GetSize();
            RHI::CmdSetViewport( pCommandBuffer_PostProcessing, 0.0F, 0.0F, viewportSize.m_x, viewportSize.m_y, 0.0F, 1.0F );
            RHI::CmdSetScissor( pCommandBuffer_PostProcessing, 0, 0, uint32_t( viewportSize.m_x ), uint32_t( viewportSize.m_y ) );
        }

        if ( enableSMAA )
        {
            m_renderPass_SMAA.DrawToViewport
            (
                pRenderViewport,
                m_resourceStates,
                pCommandBuffer_PostProcessing,
                m_pRenderSystem->GetSMAAAreaTexture(),
                m_pRenderSystem->GetSMAASearchTexture()
            );
        }

        RHI::LoadAction postProcessLoadAction = {};
        postProcessLoadAction.m_loadActionsColor[0] = RHI::LoadActionType::Clear;
        postProcessLoadAction.m_colorClearValues[0] = pRenderViewport->m_finalTexture->m_clearValue;

        EE_ASSERT( !m_resourceStates.HasPendingBarriers() );
        m_resourceStates.Writeable( pRenderViewport->m_finalTexture, RHI::PipelineStage::Draw, RHI::ResourceAccess::RenderTarget, RHI::TextureState::RenderTarget );
        m_resourceStates.FlushBarriers( pCommandBuffer_PostProcessing );

        RHI::CmdSetRenderTargets( pCommandBuffer_PostProcessing, { &pRenderViewport->m_finalTexture.m_pTexture, 1 }, nullptr, &postProcessLoadAction );

        m_renderPass_PostProcess.DrawToViewport
        (
            m_resourceStates,
            pCommandBuffer_PostProcessing,
            m_pRenderSystem->GetTonemapLUT(),
            enableSMAA ? pRenderViewport->m_SMAA_resultTexture : pRenderViewport->m_forwardShading_colorTexture,
            pRenderViewport->m_forwardShading_depthTexture
        );

        // Debug draw and outlines
        //-------------------------------------------------------------------------

        #if EE_DEVELOPMENT_TOOLS
        m_renderPass_DebugDraw.DrawToViewport
        (
            pRenderViewport,
            pRenderWorldSystem->m_deviceRenderWorld,
            pRenderViewport->m_finalTexture,
            renderViewBufferHandle,
            pRenderViewport->m_forwardShadingRenderViewsOffset,
            m_resourceStates,
            pCommandBuffer_PostProcessing,
            frameIndex
        );

        if ( pRenderViewport->m_numEditorOutlineRenderViews > 0 )
        {
            m_renderPass_EditorOutline.ResolveToViewport( pRenderViewport, m_resourceStates, pCommandBuffer_PostProcessing );
        }

        if ( !pRenderViewport->IsStandalone() )
        {
            m_resourceStates.ReadOnly( pRenderViewport->m_finalTexture, RHI::PipelineStage::PixelShader, RHI::ResourceAccess::ShaderResource, RHI::TextureState::ShaderResource );
        }
        #endif

        // End frame
        //-------------------------------------------------------------------------

        RHI::CmdSetRenderTargets( pCommandBuffer_PostProcessing, {}, nullptr );
        m_resourceStates.FlushBarriers( pCommandBuffer_PostProcessing );

        if ( enableAsyncCompute )
        {
            SubmitGraphicsCommandBuffer( eastl::move( pCommandBuffer_PostProcessing ) );

            pRenderViewport->m_pWindow->AcquireGraphicsCommandBuffer( m_pRenderSystem->GetContextRHI(), frameIndex );

            return m_signalSemaphores_ShadingPass[frameIndex];
        }

        return 0;
    }

    uint64_t ForwardShadingRenderer::SubmitGraphicsCommandBuffer( RHI::CommandBuffer*&& pCommandBuffer )
    {
        EE_PROFILE_FUNCTION_RENDER();

        //-------------------------------------------------------------------------

        RHI::EndCommandBuffer( pCommandBuffer );

        uint64_t semaphore = RHI::QueueSubmit( m_pRenderSystem->GetContextRHI(), m_pRenderSystem->GetGraphicsQueue(), { &pCommandBuffer, 1 } );

        pCommandBuffer = nullptr;

        return semaphore;
    }

    uint64_t ForwardShadingRenderer::SubmitComputeCommandBuffer( RHI::CommandBuffer*&& pCommandBuffer )
    {
        EE_PROFILE_FUNCTION_RENDER();

        //-------------------------------------------------------------------------

        RHI::EndCommandBuffer( pCommandBuffer );

        uint64_t semaphore = RHI::QueueSubmit( m_pRenderSystem->GetContextRHI(), m_pRenderSystem->GetComputeQueue(), { &pCommandBuffer, 1 } );

        pCommandBuffer = nullptr;

        return semaphore;
    }
}
