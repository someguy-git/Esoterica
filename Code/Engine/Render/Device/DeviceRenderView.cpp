
#include "DeviceRenderView.h"
#include "DeviceRenderWorld.h"
#include "base/Render/RHI.h"

#include "Engine/Render/RenderSystem.h"
#include "Engine/Render/Shaders/Renderer/RendererTypes.esh"

namespace EE::Render
{
    void MaterialShaderRenderBucket::Initialize( RenderSystem* pRenderSystem )
    {
        m_drawArgumentBuffer.Initialize( pRenderSystem->GetContextRHI(), false );
    }

    void MaterialShaderRenderBucket::Shutdown( RenderSystem* pRenderSystem )
    {
        RHI::DestroyBuffer( pRenderSystem->GetContextRHI(), eastl::move( m_pDrawCounterBuffer ) );

        m_drawArgumentBuffer.Shutdown( pRenderSystem->GetContextRHI() );
    }

    //-------------------------------------------------------------------------

    void DeviceRenderViewBucket::Initialize( RenderSystem* pRenderSystem )
    {
        m_opaqueBucket.Initialize( pRenderSystem );
        m_alphaTestBucket.Initialize( pRenderSystem );
        m_alphaBlendBucket.Initialize( pRenderSystem );
    }

    void DeviceRenderViewBucket::Shutdown( RenderSystem* pRenderSystem )
    {
        m_opaqueBucket.Shutdown( pRenderSystem );
        m_alphaTestBucket.Shutdown( pRenderSystem );
        m_alphaBlendBucket.Shutdown( pRenderSystem );
    }

    //-------------------------------------------------------------------------

    void DeviceRenderView::Initialize( RenderSystem* pRenderSystem, size_t numMaterialShaderPipelineBuckets )
    {
        m_renderViewBuckets.reserve( numMaterialShaderPipelineBuckets );
        for ( size_t bucketIndex = 0; bucketIndex < numMaterialShaderPipelineBuckets; ++bucketIndex )
        {
            DeviceRenderViewBucket renderViewBucket = {};
            renderViewBucket.Initialize( pRenderSystem );

            renderViewBucket.m_opaqueBucket.m_bucketName = StringID( "Opaque" );
            renderViewBucket.m_alphaTestBucket.m_bucketName = StringID( "AlphaTest" );
            renderViewBucket.m_alphaBlendBucket.m_bucketName = StringID( "AlphaBlend" );

            m_renderViewBuckets.emplace_back( eastl::move( renderViewBucket ) );
        }
    }

    void DeviceRenderView::Shutdown( RenderSystem* pRenderSystem )
    {
        for ( DeviceRenderViewBucket& renderViewBucket : m_renderViewBuckets )
        {
            renderViewBucket.Shutdown( pRenderSystem );
        }
        m_renderViewBuckets.clear();
    }

    void DeviceRenderView::UpdateDeviceResources( RenderSystem* pRenderSystem, DeviceRenderWorld const& deviceRenderWorld )
    {
        size_t renderBucketIndex = 0;
        for ( size_t shaderIndex = 0; shaderIndex < m_renderViewBuckets.size(); ++shaderIndex )
        {
            DeviceRenderViewBucket& bucket = m_renderViewBuckets[shaderIndex];

            bucket.ForEachRenderBucket( [&renderBucketIndex, shaderIndex, pRenderSystem, &deviceRenderWorld] ( MaterialShaderRenderBucket& renderBucket )
            {
                uint32_t const clustersCapacity = deviceRenderWorld.GetClusterCapacity( shaderIndex );
                EE_ASSERT( clustersCapacity > 0 );

                uint32_t const maxNumDrawArguments = ( clustersCapacity + RHI::Limits::MaxDispatchSize - 1 ) / RHI::Limits::MaxDispatchSize;
                size_t const drawArgumentBufferSizeWorstCase = maxNumDrawArguments * sizeof( ShaderTypes::DrawArgument );

                //-------------------------------------------------------------------------

                if ( !renderBucket.m_pDrawCounterBuffer )
                {
                    RHI::BufferParameters countBufferParameters = {};
                    countBufferParameters.m_descriptorTypes = TBitFlags<RHI::DescriptorTypeFlags>( RHI::DescriptorTypeFlags::IndirectArgumentBuffer, RHI::DescriptorTypeFlags::Buffer, RHI::DescriptorTypeFlags::RWBuffer );
                    countBufferParameters.m_bufferSize = sizeof( uint32_t );
                    countBufferParameters.m_format = RHI::DataFormat::R32_UInt;
                    countBufferParameters.m_debugName.sprintf( "%s DrawCounter Buffer %i", renderBucket.m_bucketName.c_str(), renderBucketIndex );

                    renderBucket.m_pDrawCounterBuffer = RHI::CreateBuffer( pRenderSystem->GetContextRHI(), countBufferParameters );
                }

                //-------------------------------------------------------------------------

                auto UpdateBuffer_DrawArgument = [pRenderSystem, &renderBucket, renderBucketIndex] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
                {
                    pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );

                    RHI::BufferParameters drawArgumentBufferParameters = {};
                    drawArgumentBufferParameters.m_alignment = RHI::IndirectCommandAlignment;
                    drawArgumentBufferParameters.m_descriptorTypes = TBitFlags<RHI::DescriptorTypeFlags>( RHI::DescriptorTypeFlags::IndirectArgumentBuffer, RHI::DescriptorTypeFlags::RWBuffer );
                    drawArgumentBufferParameters.m_bufferSize = newBufferSize;
                    drawArgumentBufferParameters.m_bufferStride = sizeof( ShaderTypes::DrawArgument );
                    drawArgumentBufferParameters.m_debugName.sprintf( "%s DrawArgument Buffer %i", renderBucket.m_bucketName.c_str(), renderBucketIndex );

                    return RHI::CreateBuffer( pRenderSystem->GetContextRHI(), drawArgumentBufferParameters );
                };

                renderBucket.m_drawArgumentBuffer.UpdateDeviceResources( drawArgumentBufferSizeWorstCase, UpdateBuffer_DrawArgument );

                //-------------------------------------------------------------------------

                renderBucketIndex++;
            } );
        }
    }
}
