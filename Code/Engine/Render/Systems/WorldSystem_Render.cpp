#include "WorldSystem_Render.h"
#include "Engine/Render/RenderSystem.h"
#include "Engine/Entity/Entity.h"
#include "Engine/Entity/EntityWorldUpdateContext.h"
#include "Engine/Render/Components/Component_EnvironmentMaps.h"
#include "Engine/Render/Components/Component_Lights.h"
#include "Engine/Render/Components/Component_SkeletalMesh.h"
#include "Engine/Render/Components/Component_StaticMesh.h"
#include "Engine/Render/Device/DeviceRenderWorld.h"
#include "Engine/Render/RenderViewport.h"
#include "Base/Types/Arrays.h"
#include "Base/Profiling.h"
#include "Base/Render/RHI.h"
#include "Base/Threading/TaskSystem.h"

#include "Engine/Render/Shaders/MeshInstance.esh"

//-------------------------------------------------------------------------

namespace EE::Render
{
    #if EE_DEVELOPMENT_TOOLS
    void RenderWorldSystem::UpdateViewportPickingData( RenderViewport* pViewport ) const
    {
        PickingData& pickingData = pViewport->GetPickingData();
        pickingData.clear();

        if ( !pViewport->IsPickingEnabled() )
        {
            return;
        }

        auto ResolvePickingData = [this] ( DeviceAppendBuffer<PickingResult> const& buffer, PickingData& pickingData )
        {
            auto TryResolvePickingID = [this] ( PickingResult const& pr )
            {
                if ( pr.m_hitTestID != PickingID::InvalidID )
                {
                    return PickingID( pr.m_hitTestID, PickingID::InvalidID, pr.m_sortPriority, pr.m_intersectionDistance );
                }

                for ( StaticMeshComponent const* pComponent : m_staticMeshComponents )
                {
                    HandleAllocator<uint32_t>::Handle const& instanceRootHandle = pComponent->m_meshInstanceRootProxy.m_instanceHandle;

                    if ( instanceRootHandle.IsValid() && ( instanceRootHandle.m_offset <= pr.m_instanceID ) && ( pr.m_instanceID < ( instanceRootHandle.m_offset + instanceRootHandle.m_size ) ) )
                    {
                        return PickingID( pComponent->GetEntityID().m_value, pComponent->GetID().m_value, pr.m_sortPriority, pr.m_intersectionDistance );
                    }
                }

                for ( SkeletalMeshComponent const* pComponent : m_skeletalMeshComponents )
                {
                    HandleAllocator<uint32_t>::Handle const& instanceRootHandle = pComponent->m_meshInstanceRootProxy.m_instanceHandle;

                    if ( instanceRootHandle.IsValid() && ( instanceRootHandle.m_offset <= pr.m_instanceID ) && ( pr.m_instanceID < ( instanceRootHandle.m_offset + instanceRootHandle.m_size ) ) )
                    {
                        return PickingID( pComponent->GetEntityID().m_value, pComponent->GetID().m_value, pr.m_sortPriority, pr.m_intersectionDistance );
                    }
                }

                return PickingID();
            };

            //-------------------------------------------------------------------------

            for ( PickingResult const& result : buffer.m_bufferData )
            {
                PickingID pickingID = TryResolvePickingID( result );
                if ( pickingID.IsSet() )
                {
                    pickingData.push_back( pickingID );
                }
            }
        };

        uint32_t frameIndex = m_pRenderSystem->GetFrameIndex();

        ResolvePickingData( pViewport->m_debugDrawPickingResultsBuffer, pickingData );
        ResolvePickingData( pViewport->m_instancePickingResultsBuffer, pickingData );

        pickingData.DeduplicateAndSort();
    }

    void RenderWorldSystem::SetOutlinedComponents( TArrayView<ComponentID> componentIDs )
    {
        // De-duplicate and check if the set of component IDs is actually different
        //-------------------------------------------------------------------------

        TVector<ComponentID> uniqueIDs;
        for ( ComponentID ID : componentIDs )
        {
            VectorEmplaceBackUnique( uniqueIDs, ID );
        }

        if ( uniqueIDs.size() == m_outlinedComponents.size() )
        {
            bool allElementsMatch = true;
            for ( ComponentID ID : uniqueIDs )
            {
                if ( !VectorContains( m_outlinedComponents, ID ) )
                {
                    allElementsMatch = false;
                    break;
                }
            }

            if ( allElementsMatch )
            {
                return;
            }
        }

        m_outlinedComponents.swap( uniqueIDs );

        // Set highlighted components
        //-------------------------------------------------------------------------

        if ( m_meshInstanceRootOutlineData.size() != m_deviceRenderWorld.GetNumMeshInstanceRootPages() )
        {
            m_meshInstanceRootOutlineData.resize( m_deviceRenderWorld.GetNumMeshInstanceRootPages(), 0 );
        }

        Memory::MemsetZero( m_meshInstanceRootOutlineData.data(), m_meshInstanceRootOutlineData.size() * sizeof( uint64_t ) );

        auto SetOutlineBit = [this] ( uint32_t rootIndex )
        {
            EE_ASSERT( rootIndex < m_meshInstanceRootOutlineData.size() * 64 );
            m_meshInstanceRootOutlineData[rootIndex >> 6] |= ( 1ULL << ( rootIndex & 63U ) );
        };

        for ( ComponentID const& componentID : m_outlinedComponents )
        {
            if ( StaticMeshComponent const* const* ppComponent = m_staticMeshComponents.FindItem( componentID ) )
            {
                HandleAllocator<uint32_t>::Handle const& instanceHandle = ( *ppComponent )->m_meshInstanceRootProxy.m_instanceHandle;
                if ( instanceHandle.IsValid() )
                {
                    SetOutlineBit( instanceHandle.m_offset );
                }
                continue;
            }

            if ( SkeletalMeshComponent const* const* ppComponent = m_skeletalMeshComponents.FindItem( componentID ) )
            {
                HandleAllocator<uint32_t>::Handle const& instanceHandle = ( *ppComponent )->m_meshInstanceRootProxy.m_instanceHandle;
                if ( instanceHandle.IsValid() )
                {
                    SetOutlineBit( instanceHandle.m_offset );
                }
            }
        }

        m_meshInstanceRootOutlineNeedUpdate = true;
    }

    void RenderWorldSystem::ClearOutlinedComponents()
    {
        if ( m_outlinedComponents.empty() )
        {
            return;
        }

        if ( !m_meshInstanceRootOutlineData.empty() )
        {
            Memory::MemsetZero( m_meshInstanceRootOutlineData.data(), m_meshInstanceRootOutlineData.size() * sizeof( uint64_t ) );
        }

        m_meshInstanceRootOutlineNeedUpdate = true;
        m_outlinedComponents.clear();
    }
    #endif

    void RenderWorldSystem::InitializeSystem( SystemRegistry const& systemRegistry )
    {
        m_pTaskSystem = systemRegistry.GetSystem<TaskSystem>();
        m_pRenderSystem = systemRegistry.GetSystem<RenderSystem>();

        m_deviceRenderWorld.Initialize( m_pTaskSystem, m_pRenderSystem );

        #if EE_DEVELOPMENT_TOOLS
        m_meshInstanceRootOutlineBuffer.Initialize( m_pRenderSystem->GetContextRHI(), true );
        #endif

        // TODO: Need to make it a resource instead of allocating it here
        static constexpr uint32_t g_RadianceResolution = 128;
        static constexpr uint32_t g_IrradianceResolution = 32;

        RHI::TextureParameters renderTargetParameters = {};
        renderTargetParameters.m_width = g_RadianceResolution;
        renderTargetParameters.m_height = g_RadianceResolution;
        renderTargetParameters.m_arrayLayers = 6;
        renderTargetParameters.m_mipLevels = RHI::ComputeTextureMipLevels( g_RadianceResolution, g_RadianceResolution, 1 );
        renderTargetParameters.m_format = RHI::DataFormat::RGBA16_SFloat;
        renderTargetParameters.m_descriptorTypes = TBitFlags<RHI::DescriptorTypeFlags>( RHI::DescriptorTypeFlags::TextureCube,
                                                                                        RHI::DescriptorTypeFlags::RenderTarget );
        renderTargetParameters.m_clearValue = { { 0.0F, 0.0F, 0.0F, 1.0F } };
        renderTargetParameters.m_debugName = "GlobalEnvironmentMap Radiance Target";

        m_pRadianceTexture = RHI::CreateTexture( m_pRenderSystem->GetContextRHI(), renderTargetParameters );

        renderTargetParameters.m_width = g_IrradianceResolution;
        renderTargetParameters.m_height = g_IrradianceResolution;
        renderTargetParameters.m_format = RHI::DataFormat::RGBA32_SFloat;
        renderTargetParameters.m_mipLevels = 1;
        renderTargetParameters.m_debugName = "GlobalEnvironmentMap Irradiance Target";

        m_pIrradianceTexture = RHI::CreateTexture( m_pRenderSystem->GetContextRHI(), renderTargetParameters );
    }

    void RenderWorldSystem::ShutdownSystem()
    {
        m_pRenderSystem->WaitAllQueuesIdle();

        EE_ASSERT( m_numShadowCastingDirectionalLights == 0 );

        m_deviceRenderWorld.Shutdown( m_pRenderSystem );

        #if EE_DEVELOPMENT_TOOLS
        m_meshInstanceRootOutlineBuffer.Shutdown( m_pRenderSystem->GetContextRHI() );
        #endif

        RHI::DestroyTexture( m_pRenderSystem->GetContextRHI(), eastl::move( m_pRadianceTexture ) );
        RHI::DestroyTexture( m_pRenderSystem->GetContextRHI(), eastl::move( m_pIrradianceTexture ) );

        m_pRenderSystem = nullptr;
        m_pTaskSystem = nullptr;
    }

    void RenderWorldSystem::RegisterComponent( Entity* pEntity, EntityComponent* pComponent )
    {
        // Meshes
        //-------------------------------------------------------------------------

        if ( StaticMeshComponent* pStaticMeshComponent = TryCast<StaticMeshComponent>( pComponent ) )
        {
            if ( pStaticMeshComponent->HasMeshResourceSet() )
            {
                EE_ASSERT( pStaticMeshComponent->m_meshInstanceProxies.empty() );

                pStaticMeshComponent->QueueMeshInstanceInitialize( &m_deviceRenderWorld, m_pRenderSystem->GetPlaceholderMaterial() );

                if ( pStaticMeshComponent->m_viewLayers.IsFlagSet( ViewLayer::GlobalEnvironmentMap ) )
                {
                    m_needUpdateGlobalEnvironmentMap = true;
                }

                m_staticMeshComponents.Add( pStaticMeshComponent );
                m_staticMeshComponentInstanceUpdateQueue.Bind( pStaticMeshComponent, pStaticMeshComponent->GetInstanceDataUpdateSignal() );

                pStaticMeshComponent->GetInstanceDataUpdateSignal()->Send( pStaticMeshComponent );
            }
        }
        else if ( SkeletalMeshComponent* pSkeletalMeshComponent = TryCast<SkeletalMeshComponent>( pComponent ) )
        {
            if ( pSkeletalMeshComponent->HasMeshResourceSet() )
            {
                EE_ASSERT( pSkeletalMeshComponent->m_meshInstanceProxies.empty() );
                EE_ASSERT( !pSkeletalMeshComponent->m_skinningProxy.IsValid() );

                pSkeletalMeshComponent->m_skinningProxy = m_deviceRenderWorld.AllocateSkinningInstance( pSkeletalMeshComponent->GetMesh()->GetNumBones() );

                pSkeletalMeshComponent->QueueMeshInstanceInitialize( &m_deviceRenderWorld, m_pRenderSystem->GetPlaceholderMaterial() );
                pSkeletalMeshComponent->UpdateSkinningProxy();

                if ( pSkeletalMeshComponent->m_viewLayers.IsFlagSet( ViewLayer::GlobalEnvironmentMap ) )
                {
                    m_needUpdateGlobalEnvironmentMap = true;
                }

                m_skeletalMeshComponents.Add( pSkeletalMeshComponent );
                m_skeletalMeshComponentInstanceUpdateQueue.Bind( pSkeletalMeshComponent, pSkeletalMeshComponent->GetInstanceDataUpdateSignal() );

                pSkeletalMeshComponent->GetInstanceDataUpdateSignal()->Send( pSkeletalMeshComponent );
            }
        }

        // Lights
        //-------------------------------------------------------------------------

        else if ( auto pLightComponent = TryCast<LightComponent>( pComponent ) )
        {
            if ( auto pDirectionalLightComponent = TryCast<DirectionalLightComponent>( pComponent ) )
            {
                if ( pDirectionalLightComponent->GetShadowed() )
                {
                    pDirectionalLightComponent->m_cascadedShadowIndex = uint16_t( m_numShadowCastingDirectionalLights );
                    m_numShadowCastingDirectionalLights++;
                }

                pDirectionalLightComponent->m_lightInstanceProxy = m_deviceRenderWorld.AllocateDirectionalLight();
                pDirectionalLightComponent->OnWorldTransformUpdated();

                m_directionalLightComponents.Add( pDirectionalLightComponent );
            }
            else if ( auto pPointLightComponent = TryCast<PointLightComponent>( pComponent ) )
            {
                pPointLightComponent->m_lightInstanceProxy = m_deviceRenderWorld.AllocatePointLight();
                pPointLightComponent->OnWorldTransformUpdated();

                m_pointLightComponents.Add( pPointLightComponent );
            }
            else if ( auto pSpotLightComponent = TryCast<SpotLightComponent>( pComponent ) )
            {
                pSpotLightComponent->m_lightInstanceProxy = m_deviceRenderWorld.AllocateSpotLight();
                pSpotLightComponent->OnWorldTransformUpdated();

                m_spotLightComponents.Add( pSpotLightComponent );
            }
        }

        // Environment Maps
        //-------------------------------------------------------------------------

        else if ( auto pLocalEnvMapComponent = TryCast<LocalEnvironmentMapComponent>( pComponent ) )
        {
        }
    }

    void RenderWorldSystem::UnregisterComponent( Entity* pEntity, EntityComponent* pComponent )
    {
        // Meshes
        //-------------------------------------------------------------------------

        if ( StaticMeshComponent* pStaticMeshComponent = TryCast<StaticMeshComponent>( pComponent ) )
        {
            if ( pStaticMeshComponent->HasMeshResourceSet() )
            {
                EE_ASSERT( !pStaticMeshComponent->m_meshInstanceProxies.empty() );

                m_staticMeshComponentInstanceUpdateQueue.Unbind( pStaticMeshComponent, pStaticMeshComponent->GetInstanceDataUpdateSignal() );

                m_staticMeshComponents.Remove( pStaticMeshComponent->GetID() );

                for ( auto& meshInstanceProxyPair : pStaticMeshComponent->m_meshInstanceProxies )
                {
                    m_deviceRenderWorld.DeallocateMeshInstance( eastl::move( meshInstanceProxyPair.second ) );
                }
                pStaticMeshComponent->m_meshInstanceProxies.clear();

                m_deviceRenderWorld.DeallocateMeshInstanceRoot( eastl::move( pStaticMeshComponent->m_meshInstanceRootProxy ) );

                if ( pStaticMeshComponent->m_viewLayers.IsFlagSet( ViewLayer::GlobalEnvironmentMap ) )
                {
                    m_needUpdateGlobalEnvironmentMap = true;
                }
            }
        }
        else if ( SkeletalMeshComponent* pSkeletalMeshComponent = TryCast<SkeletalMeshComponent>( pComponent ) )
        {
            if ( pSkeletalMeshComponent->HasMeshResourceSet() )
            {
                EE_ASSERT( !pSkeletalMeshComponent->m_meshInstanceProxies.empty() );
                EE_ASSERT( pSkeletalMeshComponent->m_skinningProxy.IsValid() );

                m_skeletalMeshComponentInstanceUpdateQueue.Unbind( pSkeletalMeshComponent, pSkeletalMeshComponent->GetInstanceDataUpdateSignal() );

                m_skeletalMeshComponents.Remove( pSkeletalMeshComponent->GetID() );
                m_deviceRenderWorld.DeallocateSkinningInstance( eastl::move( pSkeletalMeshComponent->m_skinningProxy ) );

                for ( auto& meshInstanceProxyPair : pSkeletalMeshComponent->m_meshInstanceProxies )
                {
                    m_deviceRenderWorld.DeallocateMeshInstance( eastl::move( meshInstanceProxyPair.second ) );
                }
                pSkeletalMeshComponent->m_meshInstanceProxies.clear();

                m_deviceRenderWorld.DeallocateMeshInstanceRoot( eastl::move( pSkeletalMeshComponent->m_meshInstanceRootProxy ) );

                if ( pSkeletalMeshComponent->m_viewLayers.IsFlagSet( ViewLayer::GlobalEnvironmentMap ) )
                {
                    m_needUpdateGlobalEnvironmentMap = true;
                }
            }
        }

        // Lights
        //-------------------------------------------------------------------------

        else if ( auto pLightComponent = TryCast<LightComponent>( pComponent ) )
        {
            if ( auto pDirectionalLightComponent = TryCast<DirectionalLightComponent>( pComponent ) )
            {
                if ( pDirectionalLightComponent->GetShadowed() )
                {
                    m_numShadowCastingDirectionalLights--;
                }

                m_deviceRenderWorld.DeallocateDirectionalLight( eastl::move( pDirectionalLightComponent->m_lightInstanceProxy ) );

                m_directionalLightComponents.Remove( pDirectionalLightComponent->GetID() );
            }
            else if ( auto pPointLightComponent = TryCast<PointLightComponent>( pComponent ) )
            {
                m_deviceRenderWorld.DeallocatePointLight( eastl::move( pPointLightComponent->m_lightInstanceProxy ) );

                m_pointLightComponents.Remove( pPointLightComponent->GetID() );
            }
            else if ( auto pSpotLightComponent = TryCast<SpotLightComponent>( pComponent ) )
            {
                m_deviceRenderWorld.DeallocateSpotLight( eastl::move( pSpotLightComponent->m_lightInstanceProxy ) );

                m_spotLightComponents.Remove( pSpotLightComponent->GetID() );
            }
        }

        // Environment Maps
        //-------------------------------------------------------------------------

        else if ( auto pLocalEnvMapComponent = TryCast<LocalEnvironmentMapComponent>( pComponent ) )
        {
            // Do nothing
        }
    }

    void RenderWorldSystem::UpdateDeviceResources()
    {
        EE_PROFILE_FUNCTION_RENDER();

        m_deviceRenderWorld.UpdateDeviceResources_BeforeInstanceInitialize( m_pRenderSystem );

        // InstanceUpdate StaticMesh
        //-------------------------------------------------------------------------
        {
            TEntityMessageQueue<StaticMeshComponent>::Message staticMeshComponentMessage = {};
            while ( m_staticMeshComponentInstanceUpdateQueue.Dequeue( staticMeshComponentMessage ) )
            {
                StaticMeshComponent* pStaticMeshComponent = staticMeshComponentMessage.m_pComponent;
                EE_ASSERT( pStaticMeshComponent );

                auto CopyBufferMemory = [pStaticMeshComponent] ( uint8_t* pDstMemory_WriteCombined, size_t dstSize )
                {
                    pStaticMeshComponent->WriteInstanceData( { reinterpret_cast<uint32_t*>( pDstMemory_WriteCombined ), sizeof( ShaderTypes::MeshInstanceRoot ) / sizeof( uint32_t ) } );
                };

                m_pRenderSystem->QueueBufferUpdate
                (
                    CopyBufferMemory,
                    m_deviceRenderWorld.GetMeshInstanceRootBuffer(),
                    pStaticMeshComponent->m_meshInstanceRootProxy.m_instanceHandle.m_offset * sizeof( ShaderTypes::MeshInstanceRoot ),
                    pStaticMeshComponent->m_meshInstanceRootProxy.m_instanceHandle.m_size * sizeof( ShaderTypes::MeshInstanceRoot )
                );
                pStaticMeshComponent->QueueMeshInstanceInitialize( &m_deviceRenderWorld, m_pRenderSystem->GetPlaceholderMaterial() );
            }

            m_staticMeshComponentInstanceUpdateQueue.ClearIgnoredComponents();
        }

        // InstanceUpdate SkeletalMesh
        //-------------------------------------------------------------------------
        {
            TEntityMessageQueue<SkeletalMeshComponent>::Message skeletalMeshComponentMessage = {};
            while ( m_skeletalMeshComponentInstanceUpdateQueue.Dequeue( skeletalMeshComponentMessage ) )
            {
                SkeletalMeshComponent* pSkeletalMeshComponent = skeletalMeshComponentMessage.m_pComponent;
                EE_ASSERT( pSkeletalMeshComponent );

                auto CopyBufferMemory = [pSkeletalMeshComponent] ( uint8_t* pDstMemory_WriteCombined, size_t dstSize )
                {
                    pSkeletalMeshComponent->WriteInstanceData( { reinterpret_cast<uint32_t*>( pDstMemory_WriteCombined ), sizeof( ShaderTypes::MeshInstanceRoot ) / sizeof( uint32_t ) } );
                };

                m_pRenderSystem->QueueBufferUpdate
                (
                    CopyBufferMemory,
                    m_deviceRenderWorld.GetMeshInstanceRootBuffer(),
                    pSkeletalMeshComponent->m_meshInstanceRootProxy.m_instanceHandle.m_offset * sizeof( ShaderTypes::MeshInstanceRoot ),
                    pSkeletalMeshComponent->m_meshInstanceRootProxy.m_instanceHandle.m_size * sizeof( ShaderTypes::MeshInstanceRoot )
                );
                pSkeletalMeshComponent->QueueMeshInstanceInitialize( &m_deviceRenderWorld, m_pRenderSystem->GetPlaceholderMaterial() );
            }

            m_skeletalMeshComponentInstanceUpdateQueue.ClearIgnoredComponents();
        }

        m_deviceRenderWorld.UpdateDeviceResources_AfterInstanceInitialize( m_pRenderSystem );

        //-------------------------------------------------------------------------

        #if EE_DEVELOPMENT_TOOLS
        if ( m_meshInstanceRootOutlineData.size() != m_deviceRenderWorld.GetNumMeshInstanceRootPages() )
        {
            m_meshInstanceRootOutlineData.resize( m_deviceRenderWorld.GetNumMeshInstanceRootPages(), 0 );
            m_meshInstanceRootOutlineNeedUpdate = true;
        }

        auto UpdateBuffer_MeshInstanceRootOutline = [this] ( RHI::Buffer* && pOldBuffer, size_t newBufferSize )
        {
            m_meshInstanceRootOutlineNeedUpdate = true;

            RHI::BufferParameters outlineBufferParameters = {};
            outlineBufferParameters.m_bufferSize = newBufferSize;
            outlineBufferParameters.m_bufferStride = sizeof( uint64_t );
            outlineBufferParameters.m_format = RHI::DataFormat::RG32_UInt;
            outlineBufferParameters.m_debugName = "RenderWorldSystem MeshInstanceRoot Outline Buffer";

            RHI::Buffer* pOutlineBuffer = RHI::CreateBuffer( m_pRenderSystem->GetContextRHI(), outlineBufferParameters );

            if ( pOldBuffer )
            {
                m_pRenderSystem->QueueResourceDelete( eastl::move( pOldBuffer ) );
            }

            return pOutlineBuffer;
        };

        m_meshInstanceRootOutlineBuffer.UpdateDeviceResources
        (
            m_meshInstanceRootOutlineData.size() * sizeof( uint64_t ),
            UpdateBuffer_MeshInstanceRootOutline
        );

        if ( m_meshInstanceRootOutlineNeedUpdate )
        {
            EE_ASSERT( m_meshInstanceRootOutlineBuffer.m_pBuffer != nullptr );
            EE_ASSERT( m_meshInstanceRootOutlineBuffer.m_pBuffer->m_size >= m_meshInstanceRootOutlineData.size() * sizeof( uint64_t ) );

            size_t const outlineBufferSize = m_meshInstanceRootOutlineData.size() * sizeof( uint64_t );
            uint64_t const* pOutlineBits = m_meshInstanceRootOutlineData.data();

            auto CopyBufferMemory = [pOutlineBits, outlineBufferSize] ( uint8_t* pDstMemory_WriteCombined, size_t dstSize )
            {
                EE_ASSERT( dstSize == outlineBufferSize );
                Memory::CopyToWriteCombined( pDstMemory_WriteCombined, pOutlineBits, outlineBufferSize );
            };

            m_pRenderSystem->QueueBufferUpdate
            (
                CopyBufferMemory,
                m_meshInstanceRootOutlineBuffer.m_pBuffer,
                0,
                outlineBufferSize
            );

            m_meshInstanceRootOutlineNeedUpdate = false;
        }
        #endif
    }

    #if EE_DEVELOPMENT_TOOLS
    RHI::BufferHandle RenderWorldSystem::GetMeshInstanceRootOutlineBufferHandle() const
    {
        return RHI::GetBufferHandle( m_meshInstanceRootOutlineBuffer.m_pBuffer, RHI::DescriptorTypeFlags::Buffer );
    }
    #endif

    RHI::TextureHandle RenderWorldSystem::GetRadianceTextureHandle() const
    {
        return RHI::GetTextureHandle( m_pRadianceTexture, RHI::DescriptorTypeFlags::TextureCube, 0 );
    }

    float RenderWorldSystem::GetRadianceTextureMipLevels() const
    {
        return float( m_pRadianceTexture->m_mipLevels );
    }

    RHI::TextureHandle RenderWorldSystem::GetIrradianceTextureHandle() const
    {
        return RHI::GetTextureHandle( m_pIrradianceTexture, RHI::DescriptorTypeFlags::TextureCube, 0 );
    }
}
