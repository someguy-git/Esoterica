#include "RenderGeometry.h"
#include "Engine/Render/Shaders/MeshData.esh"

//-------------------------------------------------------------------------

namespace EE::Render
{
    static_assert( sizeof( MeshCluster ) == sizeof( ShaderTypes::MeshCluster ) );
    static_assert( sizeof( MeshHeader ) == sizeof( ShaderTypes::MeshHeader ) );
    static_assert( sizeof( MeshCluster::VertexNormalAttribute ) == sizeof( ShaderTypes::VertexNormalAttribute ) );
    static_assert( sizeof( MeshCluster::TextureCoordinateAttribute ) == sizeof( ShaderTypes::TextureCoordinateAttribute ) );
    static_assert( sizeof( MeshCluster::VertexColorAttribute ) == sizeof( ShaderTypes::VertexColorAttribute ) );
    static_assert( sizeof( MeshCluster::SkinningAttribute ) == sizeof( ShaderTypes::SkinningAttribute ) );
}
