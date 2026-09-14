#include "Animation_ToolsGraphNode_ExternalPose.h"
#include "Engine/Animation/Graph/Nodes/Animation_RuntimeGraphNode_ExternalPose.h"
#include "EngineTools/Animation/ToolsGraph/Animation_ToolsGraph_Compilation.h"

//-------------------------------------------------------------------------

namespace EE::Animation
{
    ExternalPoseToolsNode::ExternalPoseToolsNode()
    {
        CreateOutputPin( "Pose", GraphValueType::Pose );
        Rename( "External Pose" );
    }

    int16_t ExternalPoseToolsNode::Compile( GraphCompilationContext &context ) const
    {
        ExternalPoseNode::Definition *pDefinition = nullptr;
        NodeCompilationState const state = context.GetDefinition<ExternalPoseNode>( this, pDefinition );
        if ( state == NodeCompilationState::NeedCompilation )
        {
            context.RegisterExternalPoseSlotNode( pDefinition->m_nodeIdx, StringID( GetName() ) );
        }

        return pDefinition->m_nodeIdx;
    }

    String ExternalPoseToolsNode::CreateUniqueNodeName( String const &desiredName ) const
    {
        String uniqueName = desiredName;

        if ( HasParentGraph() )
        {
            // Ensure that the slot name is unique within the same graph
            int32_t cnt = 0;
            auto const externalSlotNodes = GetRootGraph()->FindAllNodesOfType<ExternalPoseToolsNode>( NodeGraph::SearchMode::Recursive, NodeGraph::SearchTypeMatch::Exact );
            for ( int32_t i = 0; i < externalSlotNodes.size(); i++ )
            {
                if ( externalSlotNodes[i] == this )
                {
                    continue;
                }

                if ( externalSlotNodes[i]->GetName() == uniqueName )
                {
                    uniqueName = String( String::CtorSprintf(), "%s_%d", desiredName.c_str(), cnt );
                    cnt++;
                    i = -1;
                }
            }
        }

        return uniqueName;
    }
}