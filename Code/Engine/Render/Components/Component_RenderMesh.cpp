#include "Component_RenderMesh.h"
#include "Engine/Render/Device/DeviceRenderWorld.h"
#include "Engine/Render/RenderMesh.h"
#include "Engine/Render/Shaders/MeshInstance.esh"

//-------------------------------------------------------------------------

namespace EE::Render
{
    bool MeshComponent::SubmeshSettings::operator==( SubmeshSettings const& rhs ) const
    {
        if ( m_hiddenSubmeshes.size() != rhs.m_hiddenSubmeshes.size() )
        {
            return false;
        }

        if ( m_materialOverrides.size() != rhs.m_materialOverrides.size() )
        {
            return false;
        }

        //-------------------------------------------------------------------------

        int32_t const numHidden = (int32_t) m_hiddenSubmeshes.size();
        for ( int32_t i = 0; i < numHidden; i++ )
        {
            if ( m_hiddenSubmeshes[i] != rhs.m_hiddenSubmeshes[i] )
            {
                return false;
            }
        }

        //-------------------------------------------------------------------------

        int32_t const numOverrides = (int32_t) m_materialOverrides.size();
        for ( int32_t i = 0; i < numOverrides; i++ )
        {
            if ( m_materialOverrides[i] != rhs.m_materialOverrides[i] )
            {
                return false;
            }
        }

        return true;
    }

    MeshComponent::MaterialOverride* MeshComponent::SubmeshSettings::GetMaterialOverride( int16_t submeshIdx )
    {
        for ( auto& materialOverride : m_materialOverrides )
        {
            if ( materialOverride.m_submeshIdx == submeshIdx )
            {
                return &materialOverride;
            }
        }

        return nullptr;
    }

    //-------------------------------------------------------------------------

    void MeshComponent::Initialize()
    {
        SpatialEntityComponent::Initialize();
        ValidateAndFixSubmeshSettings();
    }

    void MeshComponent::SetViewLayers( TBitFlags<ViewLayer> viewLayers )
    {
        EE_ASSERT( !IsInitialized() );
        m_viewLayers = viewLayers;
    }

    int32_t MeshComponent::GetNumSubmeshes() const
    {
        auto pMesh = GetMeshResource();
        if ( pMesh != nullptr )
        {
            return pMesh->GetNumSubmeshes();
        }

        return 0;
    }

    StringID MeshComponent::GetSubmeshID( int32_t submeshIdx ) const
    {
        auto pMesh = GetMeshResource();
        if ( pMesh != nullptr )
        {
            EE_ASSERT( submeshIdx >= 0 && submeshIdx < pMesh->GetNumSubmeshes() );
            return pMesh->GetSubmeshID( submeshIdx );
        }

        return StringID();
    }

    //-------------------------------------------------------------------------

    void MeshComponent::SetVisible( bool visible )
    {
        m_componentHidden = !visible;
        OnRenderInstanceDataUpdated();
    }

    void MeshComponent::SetSubmeshVisibility( int16_t submeshIdx, bool isVisible )
    {
        EE_ASSERT( submeshIdx >= 0 );

        if ( IsMeshLoaded() )
        {
            EE_ASSERT( submeshIdx < int32_t( GetMeshResource()->GetNumSubmeshes() ) );
        }

        //-------------------------------------------------------------------------

        if ( isVisible )
        {
            m_submeshSettings.m_hiddenSubmeshes.erase_first( submeshIdx );
        }
        else
        {
            VectorEmplaceBackUnique( m_submeshSettings.m_hiddenSubmeshes, submeshIdx );
        }

        OnRenderInstanceDataUpdated();
    }

    void MeshComponent::SetSubmeshVisibility( TVector<int16_t> const& hiddenSubmeshes )
    {
        m_submeshSettings.m_hiddenSubmeshes = hiddenSubmeshes;
        OnRenderInstanceDataUpdated();
    }

    //-------------------------------------------------------------------------

    bool MeshComponent::IsMaterialOverridden( int16_t submeshIdx ) const
    {
        EE_ASSERT( submeshIdx >= 0 );
        return m_submeshSettings.GetMaterialOverride( submeshIdx ) != nullptr;
    }

    Material const* MeshComponent::GetMaterialOverride( int16_t submeshIdx ) const
    {
        EE_ASSERT( submeshIdx >= 0 );
        auto pOverride = m_submeshSettings.GetMaterialOverride( submeshIdx );
        if ( pOverride == nullptr )
        {
            return nullptr;
        }

        return pOverride->m_material.IsLoaded() ? pOverride->m_material.GetPtr() : nullptr;
    }

    ResourceID MeshComponent::GetMaterialOverrideResourceID( int16_t submeshIdx ) const
    {
        EE_ASSERT( submeshIdx >= 0 );
        auto pOverride = m_submeshSettings.GetMaterialOverride( submeshIdx );
        if ( pOverride == nullptr )
        {
            return ResourceID();
        }

        return pOverride->m_material.GetResourceID();
    }

    void MeshComponent::SetMaterialOverride( int16_t submeshIdx, ResourceID const& materialResourceID )
    {
        EE_ASSERT( submeshIdx >= 0 );

        auto SetOverride = [this, submeshIdx, materialResourceID] ()
        {
            auto pOverride = m_submeshSettings.GetMaterialOverride( submeshIdx );
            if ( pOverride == nullptr )
            {
                m_submeshSettings.m_materialOverrides.emplace_back( submeshIdx, materialResourceID );
            }
            else
            {
                pOverride->m_material = materialResourceID;
            }
        };

        RequestRuntimeResourceChange( SetOverride );
    }

    void MeshComponent::ClearMaterialOverrides()
    {
        RequestRuntimeResourceChange( [this] () { m_submeshSettings.m_materialOverrides.clear(); } );
    }

    TInlineVector<Material const*, 50> MeshComponent::GetResolvedMaterials() const
    {
        TInlineVector<Material const*, 50> materials;

        if ( !IsMeshLoaded() )
        {
            return materials;
        }

        //-------------------------------------------------------------------------

        auto pMesh = GetMeshResource();
        EE_ASSERT( pMesh != nullptr );

        int32_t const numSubmeshes = pMesh->GetNumSubmeshes();
        materials.reserve( numSubmeshes );

        for ( int16_t submeshIdx = 0; submeshIdx < numSubmeshes; submeshIdx++ )
        {
            auto pOverride = m_submeshSettings.GetMaterialOverride( submeshIdx );
            if ( pOverride != nullptr )
            {
                materials.emplace_back( pOverride->m_material.IsLoaded() ? pOverride->m_material.GetPtr() : nullptr );
            }
            else // Use default material
            {
                materials.emplace_back( pMesh->GetMaterial( submeshIdx ) );
            }
        }

        return materials;
    }

    //-------------------------------------------------------------------------

    void MeshComponent::SetForcedMinLOD( int32_t forceMinLOD )
    {
        m_forcedMinLOD = Math::Clamp( forceMinLOD, -1, 7 );
        OnRenderInstanceDataUpdated();
    }

    int32_t MeshComponent::GetForcedMinLOD() const
    {
        return m_forcedMinLOD;
    }

    void MeshComponent::SetForcedLOD( int32_t forceLOD )
    {
        m_forcedLOD = Math::Clamp( forceLOD, -1, 7 );
        OnRenderInstanceDataUpdated();
    }

    int32_t MeshComponent::GetForcedLOD() const
    {
        return m_forcedLOD;
    }

    void MeshComponent::ValidateAndFixSubmeshSettings()
    {
        if ( !HasMeshResourceSet() )
        {
            m_submeshSettings.Clear();
        }
        else
        {
            if ( IsMeshLoaded() )
            {
                int32_t const numSubmeshes = GetMeshResource()->GetNumSubmeshes();

                // Fill all resource IDs (and remove any invalid ones)
                for ( int32_t i = int32_t( m_submeshSettings.m_materialOverrides.size() ) - 1; i >= 0; i-- )
                {
                    int16_t const submeshIdx = m_submeshSettings.m_materialOverrides[i].m_submeshIdx;
                    if ( submeshIdx < 0 || submeshIdx >= numSubmeshes )
                    {
                        m_submeshSettings.m_materialOverrides.erase_unsorted( m_submeshSettings.m_materialOverrides.begin() + i );
                    }
                }

                // Fill all visibility state
                for ( int32_t i = int32_t( m_submeshSettings.m_hiddenSubmeshes.size() ) - 1; i >= 0; i-- )
                {
                    if ( m_submeshSettings.m_hiddenSubmeshes[i] < 0 || m_submeshSettings.m_hiddenSubmeshes[i] >= numSubmeshes )
                    {
                        m_submeshSettings.m_hiddenSubmeshes.erase_unsorted( m_submeshSettings.m_hiddenSubmeshes.begin() + i );
                    }
                }
            }
            else // Mesh not loaded
            {
                // Dont mess with the settings as we cannot validate anything...
            }
        }
    }

    //-------------------------------------------------------------------------

    void MeshComponent::AllocateMeshInstanceProxies( DeviceRenderWorld* pDeviceRenderWorld )
    {
        EE_ASSERT( pDeviceRenderWorld != nullptr );
        EE_ASSERT( m_meshInstanceRootProxy.IsValid() );
        EE_ASSERT( m_meshInstanceProxies.empty() );
        EE_ASSERT( !m_submeshToMeshInstance.empty() );

        uint32_t const numShaderPools = pDeviceRenderWorld->GetNumMeshInstanceShaderPools();

        //-------------------------------------------------------------------------

        TVector<uint32_t> numSubmeshesPerShader( numShaderPools, 0 );
        TVector<uint32_t> uniqueShaderIndices;
        uniqueShaderIndices.reserve( m_submeshToMeshInstance.size() );
        for ( SubmeshToMeshInstance const& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            uint32_t const shaderIndex = uint32_t( submeshToMeshInstance.m_shaderIndex );
            EE_ASSERT( shaderIndex < numShaderPools );

            if ( numSubmeshesPerShader[shaderIndex]++ == 0 )
            {
                uniqueShaderIndices.push_back( shaderIndex );
            }
        }

        m_meshInstanceProxies.reserve( uniqueShaderIndices.size() );

        //-------------------------------------------------------------------------

        TVector<uint32_t> numClustersPerInstance( m_submeshToMeshInstance.size() );
        TVector<uint32_t> numClustersPerInstanceOffsets( numShaderPools, 0 );

        uint32_t submeshNumClustersOffset = 0;
        for ( uint32_t shaderIndex : uniqueShaderIndices )
        {
            numClustersPerInstanceOffsets[shaderIndex] = submeshNumClustersOffset;
            submeshNumClustersOffset += numSubmeshesPerShader[shaderIndex];
        }
        EE_ASSERT( submeshNumClustersOffset == m_submeshToMeshInstance.size() );

        for ( SubmeshToMeshInstance const& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            uint32_t const shaderIndex = uint32_t( submeshToMeshInstance.m_shaderIndex );
            numClustersPerInstance[numClustersPerInstanceOffsets[shaderIndex]++] = submeshToMeshInstance.m_numClusters;
        }

        //-------------------------------------------------------------------------

        uint32_t proxyNumClustersOffset = 0;
        for ( uint32_t shaderIndex : uniqueShaderIndices )
        {
            uint32_t const numSubmeshesInShader = numSubmeshesPerShader[shaderIndex];

            TArrayView<uint32_t const> proxyNumClustersPerInstance = TArrayView<uint32_t const>( numClustersPerInstance.data() + proxyNumClustersOffset, numSubmeshesInShader );

            MeshInstanceProxy meshInstanceProxy = pDeviceRenderWorld->AllocateMeshInstance( shaderIndex, proxyNumClustersPerInstance );
            m_meshInstanceProxies.emplace_back( TPair<uint32_t, MeshInstanceProxy>{ shaderIndex, eastl::move( meshInstanceProxy ) } );

            proxyNumClustersOffset += numSubmeshesInShader;
        }
    }

    void MeshComponent::QueueMeshInstanceInitialize( DeviceRenderWorld* pDeviceRenderWorld, Material const* pPlaceholderMaterial )
    {
        EE_ASSERT( pDeviceRenderWorld != nullptr );

        if ( m_meshInstanceProxies.empty() )
        {
            EE_ASSERT( !m_meshInstanceRootProxy.IsValid() );
            EE_ASSERT( pPlaceholderMaterial != nullptr );

            m_meshInstanceRootProxy = pDeviceRenderWorld->AllocateMeshInstanceRoot();
            ResolveSubmeshMaterials( pPlaceholderMaterial );
            UpdateSubmeshVisibility();
            AllocateMeshInstanceProxies( pDeviceRenderWorld );
            ResolveSubmeshProxyData( pDeviceRenderWorld->GetNumMeshInstanceShaderPools() );
        }
        else
        {
            ValidateSubmeshInstanceData( pPlaceholderMaterial );
            UpdateSubmeshVisibility();
        }

        //-------------------------------------------------------------------------

        EE_ASSERT( m_meshInstanceRootProxy.IsValid() );
        EE_ASSERT( !m_meshInstanceProxies.empty() );
        EE_ASSERT( !m_submeshToMeshInstance.empty() );

        for ( SubmeshToMeshInstance const& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            pDeviceRenderWorld->QueueMeshInstanceInitialize
            (
                m_meshInstanceProxies[submeshToMeshInstance.m_proxyIndex].second,
                m_meshInstanceRootProxy.m_instanceHandle.m_offset,
                submeshToMeshInstance.m_pMeshBuffer,
                submeshToMeshInstance.m_shaderParametersOffsetIn32ByteBlocks,
                submeshToMeshInstance.m_numClusters,
                submeshToMeshInstance.m_lodMask,
                submeshToMeshInstance.m_instanceIndex,
                submeshToMeshInstance.m_clusterToInstanceOffset,
                submeshToMeshInstance.m_instanceHidden
            );
        }

        //-------------------------------------------------------------------------

        WriteMeshInstanceRootTransform();
        WriteMeshInstanceLocalTransforms();
    }

    void MeshComponent::WriteInstanceData( Mesh const* pMeshResource, uint32_t boneOffset, TArrayView<uint32_t> bufferData_WriteCombined ) const
    {
        uint32_t numLODs = uint32_t( pMeshResource->GetLODDistances().size() );
        EE_ASSERT( numLODs <= 8 );

        alignas( 32 ) ShaderTypes::MeshInstanceRoot meshInstanceRoot = {};
        meshInstanceRoot.m_instanceHidden = !IsVisible();

        meshInstanceRoot.m_numLODs = numLODs;

        meshInstanceRoot.m_renderViewLayerFlags = m_viewLayers;

        meshInstanceRoot.m_boneOffset = boneOffset;

        EE_ASSERT( m_forcedMinLOD <= 7 );
        EE_ASSERT( m_forcedLOD <= 7 );

        meshInstanceRoot.m_forceMinLOD = uint32_t( Math::Max( 0, m_forcedMinLOD ) );
        meshInstanceRoot.m_forceLOD = uint32_t( Math::Max( 0, m_forcedLOD ) );
        meshInstanceRoot.m_useForcedLOD = ( m_forcedLOD >= 0 );

        Matrix transformMatrix = GetWorldTransform().ToMatrix();
        float const globalUniformScale = GetWorldTransform().GetScale();

        // Validate that this matrix is a valid 4x3 matrix with implicit (0,0,0,1) column
        EE_ASSERT( Math::IsNearEqual( transformMatrix.GetRow( 0 ).GetW(), 0.0F ) );
        EE_ASSERT( Math::IsNearEqual( transformMatrix.GetRow( 1 ).GetW(), 0.0F ) );
        EE_ASSERT( Math::IsNearEqual( transformMatrix.GetRow( 2 ).GetW(), 0.0F ) );
        EE_ASSERT( Math::IsNearEqual( transformMatrix.GetRow( 3 ).GetW(), 1.0F ) );

        transformMatrix.GetRow( 0 ).StoreFloat3( meshInstanceRoot.m_rootTransform + 0 );
        transformMatrix.GetRow( 1 ).StoreFloat3( meshInstanceRoot.m_rootTransform + 3 );
        transformMatrix.GetRow( 2 ).StoreFloat3( meshInstanceRoot.m_rootTransform + 6 );
        transformMatrix.GetRow( 3 ).StoreFloat3( meshInstanceRoot.m_rootTransform + 9 );

        AABB const worldAABB = GetWorldBounds().GetAABB();
        meshInstanceRoot.SetWorldAABB( worldAABB.m_center, worldAABB.m_halfExtents, GetWorldTransform().GetTranslation().ToFloat3() );

        TArrayView<float const> meshDataLODDistance = pMeshResource->GetLODDistances();
        for ( uint32_t lodIndex = 0; lodIndex < uint32_t( meshDataLODDistance.size() ); ++lodIndex )
        {
            meshInstanceRoot.SetLODDistance( lodIndex, meshDataLODDistance[lodIndex] * globalUniformScale );
        }

        Memory::CopyToWriteCombined( bufferData_WriteCombined.data(), &meshInstanceRoot, sizeof( ShaderTypes::MeshInstanceRoot ) );
    }

    void MeshComponent::WriteMeshInstanceRootTransform()
    {
        if ( !m_meshInstanceRootProxy.IsValid() )
        {
            return;
        }

        EE_PROFILE_FUNCTION_RENDER();

        AABB const worldAABB = GetWorldBounds().GetAABB();
        m_meshInstanceRootProxy.WriteRootTransform( GetWorldTransform(), GetWorldNonUniformScale(), worldAABB.m_center.ToFloat3(), worldAABB.m_halfExtents.ToFloat3() );
    }

    void MeshComponent::WriteMeshInstanceLocalTransforms()
    {
        EE_PROFILE_FUNCTION_RENDER();

        EE_ASSERT( !m_meshInstanceProxies.empty() );

        TVector<Matrix43> const& submeshLocalTransforms = GetMeshResource()->GetSubmeshLocalTransforms();
        EE_ASSERT( submeshLocalTransforms.size() == m_submeshToMeshInstance.size() );

        //-------------------------------------------------------------------------

        for ( auto& meshInstanceProxyPair : m_meshInstanceProxies )
        {
            meshInstanceProxyPair.second.StartLocalTransformWrite();
        }

        for ( SubmeshToMeshInstance const& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            m_meshInstanceProxies[submeshToMeshInstance.m_proxyIndex].second.WriteLocalTransform( submeshLocalTransforms[submeshToMeshInstance.m_submeshIndex] );
        }

        for ( auto& meshInstanceProxyPair : m_meshInstanceProxies )
        {
            meshInstanceProxyPair.second.SubmitLocalTransformWrite();
        }
    }

    void MeshComponent::ResolveSubmeshMaterials( Material const* pPlaceholderMaterial )
    {
        EE_PROFILE_FUNCTION_RENDER();

        EE_ASSERT( pPlaceholderMaterial != nullptr );

        Mesh const* pMesh = GetMeshResource();
        EE_ASSERT( pMesh != nullptr );

        TInlineVector<Material const*, 50> const resolvedMaterials = GetResolvedMaterials();
        uint32_t const numSubmeshes = uint32_t( pMesh->GetNumSubmeshes() );
        EE_ASSERT( resolvedMaterials.size() == numSubmeshes );

        m_submeshToMeshInstance.resize( numSubmeshes );
        for ( uint32_t submeshIndex = 0; submeshIndex < numSubmeshes; ++submeshIndex )
        {
            SubmeshToMeshInstance& submeshToMeshInstance = m_submeshToMeshInstance[submeshIndex];

            submeshToMeshInstance.m_submeshIndex = submeshIndex;

            Material const* pMaterial = resolvedMaterials[submeshIndex];
            if ( pMaterial == nullptr )
            {
                EE_LOG_ERROR( LogCategory::Render, "MeshComponent", "Failed to resolve material for submesh %u, reverting to placeholder", submeshIndex );
                pMaterial = pPlaceholderMaterial;
            }

            EE_ASSERT( pMaterial != nullptr );

            submeshToMeshInstance.m_shaderIndex = pMaterial->GetShaderIndex();
            EE_ASSERT( submeshToMeshInstance.m_shaderIndex != -1 );

            submeshToMeshInstance.m_shaderParametersOffsetIn32ByteBlocks = pMaterial->GetShaderParametersOffsetIn32ByteBlocks();

            Mesh::Submesh const& submesh = pMesh->GetSubmesh( submeshIndex );
            submeshToMeshInstance.m_numClusters = pMesh->GetGeometry()[submesh.m_geometryIdx].GetNumClusters();
            submeshToMeshInstance.m_pMeshBuffer = pMesh->GetMeshBuffer( submesh.m_geometryIdx );
            submeshToMeshInstance.m_lodMask = submesh.m_lodMask;
        }
    }

    void MeshComponent::ResolveSubmeshProxyData( uint32_t numShaderPools )
    {
        EE_PROFILE_FUNCTION_RENDER();

        EE_ASSERT( !m_meshInstanceProxies.empty() );
        EE_ASSERT( !m_submeshToMeshInstance.empty() );

        TVector<uint32_t> shaderToProxyIndex( numShaderPools, ~0U );
        for ( uint32_t proxyIndex = 0; proxyIndex < m_meshInstanceProxies.size(); ++proxyIndex )
        {
            EE_ASSERT( m_meshInstanceProxies[proxyIndex].first < numShaderPools );
            shaderToProxyIndex[m_meshInstanceProxies[proxyIndex].first] = proxyIndex;
        }

        TVector<uint32_t> proxyInstanceIndices( m_meshInstanceProxies.size(), 0 );
        TVector<uint32_t> proxyClusterToInstanceOffsets( m_meshInstanceProxies.size(), 0 );
        for ( uint32_t proxyIndex = 0; proxyIndex < m_meshInstanceProxies.size(); ++proxyIndex )
        {
            proxyClusterToInstanceOffsets[proxyIndex] = m_meshInstanceProxies[proxyIndex].second.m_clusterHandle.m_offset;
        }

        for ( SubmeshToMeshInstance& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            uint32_t const proxyIndex = shaderToProxyIndex[uint32_t( submeshToMeshInstance.m_shaderIndex )];
            EE_ASSERT( proxyIndex != ~0U );

            submeshToMeshInstance.m_proxyIndex = proxyIndex;
            submeshToMeshInstance.m_instanceIndex = proxyInstanceIndices[proxyIndex]++;
            submeshToMeshInstance.m_clusterToInstanceOffset = proxyClusterToInstanceOffsets[proxyIndex];

            proxyClusterToInstanceOffsets[proxyIndex] += submeshToMeshInstance.m_numClusters;
        }

        // Validation
        //-------------------------------------------------------------------------

        for ( uint32_t proxyIndex = 0; proxyIndex < m_meshInstanceProxies.size(); ++proxyIndex )
        {
            EE_ASSERT( proxyInstanceIndices[proxyIndex] == m_meshInstanceProxies[proxyIndex].second.m_instanceHandle.m_size );
            EE_ASSERT( proxyClusterToInstanceOffsets[proxyIndex] == m_meshInstanceProxies[proxyIndex].second.m_clusterHandle.m_offset + m_meshInstanceProxies[proxyIndex].second.m_clusterHandle.m_size );
        }
    }

    void MeshComponent::UpdateSubmeshVisibility()
    {
        EE_PROFILE_FUNCTION_RENDER();

        EE_ASSERT( !m_submeshToMeshInstance.empty() );

        Mesh const* pMesh = GetMeshResource();
        EE_ASSERT( pMesh != nullptr );

        uint32_t const numSubmeshes = uint32_t( pMesh->GetNumSubmeshes() );
        EE_ASSERT( m_submeshToMeshInstance.size() == numSubmeshes );

        TVector<bool> hiddenSubmeshLookup( numSubmeshes, false );
        for ( int16_t hiddenSubmeshIdx : m_submeshSettings.m_hiddenSubmeshes )
        {
            EE_ASSERT( hiddenSubmeshIdx >= 0 && uint32_t( hiddenSubmeshIdx ) < numSubmeshes );
            hiddenSubmeshLookup[hiddenSubmeshIdx] = true;
        }

        for ( SubmeshToMeshInstance& submeshToMeshInstance : m_submeshToMeshInstance )
        {
            submeshToMeshInstance.m_instanceHidden = hiddenSubmeshLookup[submeshToMeshInstance.m_submeshIndex];
        }
    }

    void MeshComponent::ValidateSubmeshInstanceData( Material const* pPlaceholderMaterial ) const
    {
        EE_PROFILE_FUNCTION_RENDER();

        EE_ASSERT( pPlaceholderMaterial != nullptr );

        Mesh const* pMesh = GetMeshResource();
        EE_ASSERT( pMesh != nullptr );

        TInlineVector<Material const*, 50> const resolvedMaterials = GetResolvedMaterials();
        uint32_t const numSubmeshes = uint32_t( pMesh->GetNumSubmeshes() );

        // If any of the asserts below are failing this likely means that you're doing something that needs to go through RequestRuntimeResourceChange()
        //-------------------------------------------------------------------------

        EE_ASSERT( resolvedMaterials.size() == numSubmeshes );
        EE_ASSERT( m_submeshToMeshInstance.size() == numSubmeshes );

        for ( uint32_t submeshIndex = 0; submeshIndex < numSubmeshes; ++submeshIndex )
        {
            SubmeshToMeshInstance const& submeshToMeshInstance = m_submeshToMeshInstance[submeshIndex];

            Material const* pMaterial = resolvedMaterials[submeshIndex];
            if ( pMaterial == nullptr )
            {
                pMaterial = pPlaceholderMaterial;
            }
            EE_ASSERT( pMaterial != nullptr );

            EE_ASSERT( submeshToMeshInstance.m_submeshIndex == submeshIndex );
            EE_ASSERT( submeshToMeshInstance.m_shaderIndex == pMaterial->GetShaderIndex() );
            EE_ASSERT( submeshToMeshInstance.m_shaderParametersOffsetIn32ByteBlocks == pMaterial->GetShaderParametersOffsetIn32ByteBlocks() );

            Mesh::Submesh const& submesh = pMesh->GetSubmesh( submeshIndex );
            EE_ASSERT( submeshToMeshInstance.m_numClusters == pMesh->GetGeometry()[submesh.m_geometryIdx].GetNumClusters() );
            EE_ASSERT( submeshToMeshInstance.m_pMeshBuffer == pMesh->GetMeshBuffer( submesh.m_geometryIdx ) );
            EE_ASSERT( submeshToMeshInstance.m_lodMask == submesh.m_lodMask );
        }
    }
}
