#include "ResourceProvider_Local.h"
#include "Base/Resource/ResourceRequest.h"
#include "Base/Resource/Settings/Settings_Resource.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    bool ResourceProvider::IsReady() const
    {
        return true;
    }

    bool ResourceProvider::Initialize()
    {
        return true;
    }

    void ResourceProvider::RequestRawResource( ResourceRequest* pRequest )
    {
        ResourceID const& resourceID = pRequest->GetResourceID();
        FileSystem::Path const resourceFilePath = resourceID.GetCompiledFileSystemPath( m_settings.m_compiledResourceDirectoryPath );
        pRequest->OnRawResourceRequestComplete( resourceFilePath.c_str(), String() );
    }

    void ResourceProvider::CancelRequest( ResourceRequest* pRequest )
    {
         // Do Nothing
    }
}