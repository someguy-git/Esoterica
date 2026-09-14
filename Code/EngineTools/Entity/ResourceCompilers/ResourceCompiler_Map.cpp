#include "ResourceCompiler_Map.h"
#include "EngineTools/Resource/ResourceCompilerContext.h"
#include "EngineTools/Entity/ResourceDescriptors/ResourceDescriptor_EntityMap.h"
#include "Engine/Navmesh/Components/Component_Navmesh.h"

//-------------------------------------------------------------------------

namespace EE::EntityModel
{
    EntityMapCompiler::EntityMapCompiler()
        : Resource::Compiler( "EntityMapCompiler" )
    {
        RegisterOutput<EntityMapDescriptor>();
    }

    Resource::CompilationResult EntityMapCompiler::Compile( Resource::CompileContext const& ctx ) const
    {
        auto pResourceDescriptor = ctx.GetDescriptor<EntityMapResourceDescriptor>();
        EntityMapDescriptor map = pResourceDescriptor->m_mapDescriptor;

        //-------------------------------------------------------------------------
        // Sanitize collection
        //-------------------------------------------------------------------------

        map.RemoveInvalidComponentsAndSystems( ctx.m_typeRegistry, ctx.GetLog(), ctx.IsCompilingForShippingBuild() );

        //-------------------------------------------------------------------------
        // Component Modifications
        //-------------------------------------------------------------------------

        // We need to set the navmesh component resource ptr to the appropriate resource ID
        auto const navmeshComponents = map.GetComponentsOfType<Navmesh::NavmeshComponent>( ctx.m_typeRegistry, false );
        if ( !navmeshComponents.empty() )
        {
            if ( navmeshComponents.size() > 1 )
            {
                ctx.LogWarning( "More than one navmesh component found in this map, this is not supported... Ignoring all components apart from the first found!" );
            }

            // TODO: see if there is a smart way to avoid using strings for property access
            TypeSystem::PropertyPath const navmeshResourcePropertyPath( "m_navmeshData" );

            // Remove any values for the navmesh resource property
            ComponentDescriptor* pNavmeshComponentDesc = navmeshComponents[0].m_pComponent;
            pNavmeshComponentDesc->RemovePropertyValue( navmeshResourcePropertyPath );

            // Set navmesh resource ptr
            ResourceID const navmeshResourceID = Navmesh::NavmeshData::GetNavmeshResourceIDForMap( ctx.m_resourceID.GetDataPath() );

            TypeSystem::PropertyDescriptor navmeshPtrPropertyDesc;
            navmeshPtrPropertyDesc.m_path = navmeshResourcePropertyPath;
            navmeshPtrPropertyDesc.m_stringValue = navmeshResourceID.GetString();
            if ( TypeSystem::Conversion::ConvertStringToBinary( ctx.m_typeRegistry, GetCoreTypeID( TypeSystem::CoreTypeID::TResourcePtr ), TypeSystem::TypeID(), navmeshResourceID.GetString(), navmeshPtrPropertyDesc.m_byteValue ) )
            {
                pNavmeshComponentDesc->m_properties.emplace_back( navmeshPtrPropertyDesc );
            }
        }

        //-------------------------------------------------------------------------
        // Serialize
        //-------------------------------------------------------------------------

        Resource::ResourceHeader hdr( EntityMapDescriptor::s_version, EntityMapDescriptor::GetStaticResourceTypeID(), ctx.m_sourceResourceHash );
        Serialization::BinaryOutputArchive archive;
        archive << hdr << map;

        if ( archive.WriteToFile( ctx.GetOutputPath() ) )
        {
            return Resource::CompilationResult::Success;
        }
        else
        {
            return Resource::CompilationResult::Failure;
        }
    }
}