#pragma once
#include "Base/Render/PageAllocator.h"
#include "Base/Types/Arrays.h"
#include "Base/Types/Color.h"
#include "EASTL/atomic.h"

//-------------------------------------------------------------------------

namespace EE
{
    class Transform;
    struct Matrix43;
}

namespace EE::Render
{
    class Material;

    namespace ShaderTypes
    {
        struct Transform;
        struct SkinningTransform;
        struct DirectionalLightUpdateCommand;
        struct PointLightUpdateCommand;
        struct SpotLightUpdateCommand;
        struct SkinningTransformUpdateCommand;
        struct MeshInstanceTransformUpdateCommand;
        struct MeshInstanceRootUpdateCommand;
    }

    //-------------------------------------------------------------------------

    using Buffer32ByteBlock = uint32_t[8];

    using ShaderDataHandle = PageAllocator<Buffer32ByteBlock, uint32_t>::Handle;

    //-------------------------------------------------------------------------

    struct MeshInstanceRootProxy final
    {
        void WriteRootTransform( Transform const& worldTransform, Float3 worldNonUniformScale, Float3 worldAABBCenter, Float3 worldAABBHalfExtents );

        inline bool IsValid() const { return m_instanceHandle.IsValid(); }

        //-------------------------------------------------------------------------

        eastl::atomic<uint32_t>*                                                m_pTransformUpdateCounter = nullptr;
        uint64_t const*                                                         m_pTransformUpdateSequence = nullptr;
        ShaderTypes::MeshInstanceRootUpdateCommand*                             m_pDstUpdateCommands = nullptr; // TODO: Need a workaround for platforms that don't support virtual memory. Can use PageAllocator<T> handle for that.

        uint64_t                                                                m_dstTransformUpdateSequence = ~0ULL;
        uint32_t                                                                m_dstTransformUpdateIndex = ~0U;
        HandleAllocator<uint32_t>::Handle                                       m_instanceHandle = {};
    };

    //-------------------------------------------------------------------------

    struct MeshInstanceProxy final
    {
        void StartLocalTransformWrite();
        void WriteLocalTransform( Matrix43 const& localTransform );
        void SubmitLocalTransformWrite() const;

        inline bool IsValid() const { return m_instanceHandle.IsValid(); }

        //-------------------------------------------------------------------------

        eastl::atomic<uint32_t>*                                                m_pTransformUpdateCounter = nullptr;
        uint64_t const*                                                         m_pTransformUpdateSequence = nullptr;
        ShaderTypes::MeshInstanceTransformUpdateCommand*                        m_pDstTransformUpdateCommands = nullptr; // TODO: Need a workaround for platforms that don't support virtual memory. Can use PageAllocator<T> handle for that.

        uint64_t                                                                m_dstTransformUpdateSequence = ~0ULL;
        uint32_t                                                                m_dstTransformUpdateIndex = ~0U;
        uint32_t                                                                m_numWrittenLocalTransforms = 0; // Write cursor for the reserved local transform range
        HandleAllocator<uint32_t>::Handle                                       m_instanceHandle = {};

        uint32_t                                                                m_shaderIndex = ~0U;
        HandleAllocator<uint32_t>::Handle                                       m_clusterHandle = {};
    };

    //-------------------------------------------------------------------------

    struct LightInstanceProxy final
    {
        void WriteDirectionalLight( Float3 lightDirection, float maxIntensity, Color tintedColor, uint16_t cascadedShadowIndex );
        void WritePointLight( Float3 lightPosition, float maxIntensity, float maxRadius, float falloff, Color tintedColor, uint16_t shadowMapHandle );
        void WriteSpotLight( Float3 lightPosition, Float3 lightDirection, float maxIntensity, float maxRadius, float falloff, Color tintedColor, float innerConeAngle, float outerConeAngle, uint16_t shadowMapHandle );

        inline bool IsValid() const { return m_instanceHandle.IsValid(); }

        //-------------------------------------------------------------------------

        eastl::atomic<uint32_t>*                                                m_pTransformUpdateCounter = nullptr;
        uint64_t const*                                                         m_pTransformUpdateSequence = nullptr;
        void*                                                                   m_pDstUpdateCommands = nullptr;

        uint64_t                                                                m_dstTransformUpdateSequence = ~0ULL;
        uint32_t                                                                m_dstTransformUpdateIndex = ~0U;
        HandleAllocator<uint32_t>::Handle                                       m_instanceHandle = {};
    };

    //-------------------------------------------------------------------------

    struct SkinningProxy final
    {
        void WriteTransforms( TArrayView<Transform const> boneTransforms, TArrayView<Transform const> inverseBindPose );

        inline bool IsValid() const { return m_bonesHandle.IsValid(); }

        //-------------------------------------------------------------------------

        eastl::atomic<uint32_t>*                                                m_pTransformUpdateCounter = nullptr;
        uint64_t const*                                                         m_pTransformUpdateSequence = nullptr;
        ShaderTypes::SkinningTransformUpdateCommand*                            m_pDstTransformUpdateCommands = nullptr; // TODO: Need a workaround for platforms that don't support virtual memory. Can use PageAllocator<T> handle for that.

        uint64_t                                                                m_dstTransformUpdateSequence = ~0ULL;
        uint32_t                                                                m_dstTransformUpdateIndex = ~0U;
        HandleAllocator<uint32_t>::Handle                                       m_bonesHandle = {};
    };
}
