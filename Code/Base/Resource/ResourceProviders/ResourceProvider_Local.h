#pragma once

#include "Base/Resource/IResourceProvider.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    class ResourceSettings;

    //-------------------------------------------------------------------------

    class EE_BASE_API ResourceProvider final : public IResourceProvider
    {

    public:

        ResourceProvider( ResourceSettings const& settings ) : IResourceProvider( settings ) {}
        virtual bool IsReady() const override final;

    private:

        virtual bool Initialize() override;
        virtual void RequestRawResource( ResourceRequest* pRequest ) override;
        virtual void CancelRequest( ResourceRequest* pRequest ) override;
    };
}