//
// Copyright 2019 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "UsdUndoRenameCommand.h"

#include <ufe/log.h>

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <mayaUsd/ufe/Utils.h>

#include <mayaUsdUtils/util.h>

#include "private/InPathChange.h"

#ifdef UFE_V2_FEATURES_AVAILABLE
#define UFE_ENABLE_ASSERTS
#include <ufe/ufeAssert.h>
#else
#include <cassert>
#endif

MAYAUSD_NS_DEF {
namespace ufe {

UsdUndoRenameCommand::UsdUndoRenameCommand(const UsdSceneItem::Ptr& srcItem, const Ufe::PathComponent& newName)
    : Ufe::UndoableCommand()
{
    const UsdPrim& prim = srcItem->prim();
    _stage = prim.GetStage();
    _ufeSrcItem = srcItem;
    _usdSrcPath = prim.GetPath();

    // Every call to rename() (through execute(), undo() or redo()) removes
    // a prim, which becomes expired.  Since USD UFE scene items contain a
    // prim, we must recreate them after every call to rename.
    _usdDstPath = prim.GetParent().GetPath().AppendChild(TfToken(newName.string()));

    _layer = MayaUsdUtils::defPrimSpecLayer(prim);
    if (!_layer) {
        std::string err = TfStringPrintf("No prim found at %s", prim.GetPath().GetString().c_str());
        throw std::runtime_error(err.c_str());
    }

    // if the current layer doesn't have any opinions that affects selected prim
    if (!MayaUsdUtils::doesEditTargetLayerHavePrimSpec(prim)) {
        auto possibleTargetLayer = MayaUsdUtils::strongestLayerWithPrimSpec(prim);
        std::string err = TfStringPrintf("Cannot rename [%s] defined on another layer. " 
                                         "Please set [%s] as the target layer to proceed", 
                                         prim.GetName().GetString().c_str(),
                                         possibleTargetLayer->GetDisplayName().c_str());
        throw std::runtime_error(err.c_str());
    }
    else
    {
        auto layers = MayaUsdUtils::layersWithPrimSpec(prim);

        if (layers.size() > 1) {
            std::string layerDisplayNames;
            for (auto layer : layers) {
                layerDisplayNames.append("[" + layer->GetDisplayName() + "]" + ",");
            }
            layerDisplayNames.pop_back();
            std::string err = TfStringPrintf("Cannot rename [%s] with definitions or opinions on other layers. "
                                             "Opinions exist in %s", prim.GetName().GetString().c_str(), layerDisplayNames.c_str());
            throw std::runtime_error(err.c_str());
        }
    }
}

UsdUndoRenameCommand::~UsdUndoRenameCommand()
{
}

UsdUndoRenameCommand::Ptr UsdUndoRenameCommand::create(const UsdSceneItem::Ptr& srcItem, const Ufe::PathComponent& newName)
{
    return std::make_shared<UsdUndoRenameCommand>(srcItem, newName);
}

UsdSceneItem::Ptr UsdUndoRenameCommand::renamedItem() const
{
    return _ufeDstItem;
}

bool UsdUndoRenameCommand::renameRedo()
{
    // Copy the source path using CopySpec, and remove the source.

    // We use the source layer as the destination.  An alternate workflow
    // would be the edit target layer be the destination:
    // _layer = _stage->GetEditTarget().GetLayer()
    bool status = SdfCopySpec(_layer, _usdSrcPath, _layer, _usdDstPath);
    if (status) {
        // remove all scene description for the given path and 
        // its subtree in the current UsdEditTarget 
        {
            UsdEditContext ctx(_stage, _layer);
            status = _stage->RemovePrim(_ufeSrcItem->prim().GetPath());
        }

        if (status) {
            // The renamed scene item is a "sibling" of its original name.
            _ufeDstItem = createSiblingSceneItem(_ufeSrcItem->path(), _usdDstPath.GetElementString());

            sendRenameNotification(_ufeDstItem, _ufeSrcItem->path());
        }
    }
    else {
        UFE_LOG(std::string("Warning: SdfCopySpec(") +
                _usdSrcPath.GetString() + std::string(") failed."));
    }

    return status;
}

bool UsdUndoRenameCommand::renameUndo()
{
    // Copy the source path using CopySpec, and remove the source.
    bool status = SdfCopySpec(_layer, _usdDstPath, _layer, _usdSrcPath);

    if (status) {
        // remove all scene description for the given path and 
        // its subtree in the current UsdEditTarget 
        {
            UsdEditContext ctx(_stage, _layer);
            status = _stage->RemovePrim(_usdDstPath);
        }

        if (status) {
            // create a new prim at _usdSrcPath
            auto newPrim = _stage->DefinePrim(_usdSrcPath);
            
            #ifdef UFE_V2_FEATURES_AVAILABLE
                UFE_ASSERT_MSG(newPrim, "Invalid prim cannot be inactivated.");
            #else
                assert(newPrim);
            #endif

            // I shouldn't have to again create a sibling sceneItem here since we already have a valid _ufeSrcItem
            // however, I get random crashes if I don't which needs furthur investigation.  HS, 6-May-2020.
            _ufeSrcItem = createSiblingSceneItem(_ufeDstItem->path(), _usdSrcPath.GetElementString());

            sendRenameNotification(_ufeSrcItem, _ufeDstItem->path());

            _ufeDstItem = nullptr;
        }
    }
    else {
        UFE_LOG(std::string("Warning: SdfCopySpec(") +
                _usdDstPath.GetString() + std::string(") failed."));
    }

    return status;
}

//------------------------------------------------------------------------------
// UsdUndoRenameCommand overrides
//------------------------------------------------------------------------------

void UsdUndoRenameCommand::undo()
{
    // MAYA-92264: Pixar bug prevents undo from working.  Try again with USD
    // version 0.8.5 or later.  PPT, 7-Jul-2018.
    try {
        InPathChange pc;
        if (!renameUndo()) {
            UFE_LOG("rename undo failed");
        }
    }
    catch (const std::exception& e) {
        UFE_LOG(e.what());
        throw;  // re-throw the same exception
    }
}

void UsdUndoRenameCommand::redo()
{
    InPathChange pc;
    if (!renameRedo()) {
        UFE_LOG("rename redo failed");
    }
}

} // namespace ufe
} // namespace MayaUsd
