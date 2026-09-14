#pragma once

#include "Engine/Render/Shaders/EngineShader.h"
#include "Base/Render/HandleAllocator.h"
#include "Base/Render/PageAllocator.h"
#include "Base/Render/RHI.h"
#include "Base/Types/Arrays.h"
#include "EASTL/atomic.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    enum class AsyncResourceUpdateState
    {
        AllocatePending,
        UpdatePending,
        SubmitPending,
        TransferPending,
        CompletePending,
        Completed,
    };

    using Buffer32ByteBlock = uint32_t[8];

    using ShaderDataHandle = PageAllocator<Buffer32ByteBlock, uint32_t>::Handle;

    // Async buffer update, everything in this struct is owned externally
    struct AsyncBufferUpdate
    {
        eastl::atomic<AsyncResourceUpdateState> m_updateState = AsyncResourceUpdateState::AllocatePending;

        uint8_t*                                m_pDstMemory_WriteCombined = nullptr;
        RHI::Buffer*                            m_pDstBuffer = nullptr;
        uint64_t                                m_waitSemaphore = 0;
        RHI::BufferParameters                   m_bufferParameters = {};
        RHI::BufferSubAllocation                m_stagingAllocation = {};
        uint64_t                                m_dstOffset = 0;
        uint64_t                                m_dstSize = 0;
    };

    // Async texture update, everything in this struct is owned externally
    struct AsyncTextureUpdate
    {
        eastl::atomic<AsyncResourceUpdateState> m_updateState = AsyncResourceUpdateState::AllocatePending;

        uint8_t*                                m_pDstMemory_WriteCombined = nullptr;
        RHI::Texture*                           m_pDstTexture = nullptr;
        uint64_t                                m_waitSemaphore = 0;
        RHI::TextureParameters                  m_textureParameters = {};
        RHI::BufferSubAllocation                m_stagingAllocation = {};
        RHI::TextureState                       m_dstTextureState = {};
        RHI::TextureCopyRegion                  m_dstCopyRegion = {};
        uint32_t                                m_numMipLevels = 0;
        uint32_t                                m_numArrayLayers = 0;
    };

    // Async shader data update, everything in this struct is owned externally
    struct AsyncShaderDataUpdate
    {
        eastl::atomic<AsyncResourceUpdateState> m_updateState = AsyncResourceUpdateState::AllocatePending;

        uint32_t                                m_shaderDataSizeInBytes = 0;

        ShaderDataHandle                        m_shaderDataHandle = {};
    };

    // Async shader data update, everything in this struct is owned externally
    struct AsyncMaterialParametersUpdate
    {
        eastl::atomic<AsyncResourceUpdateState> m_updateState = AsyncResourceUpdateState::AllocatePending;

        size_t                                  m_shaderIndex = 0;

        MaterialShaderParametersInstance        m_materialShaderParameters = {};
    };
}
