//
// Copyright 2015 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
//  USDExporter.cpp
//
//  Created by Michael B. Johnson on 11/22/17, based on previous work from 2015.
//
#include <regex>
#include <iostream>
#include <fstream>

#include "USDExporter.h"
#include "USDTextureHelper.h"
#include "USDSketchUpUtilities.h"

#include "pxr/base/arch/systemInfo.h"
#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/tf/setenv.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/plug/registry.h"
#include "pxr/usd/ar/defaultResolver.h"
#include "pxr/usd/kind/registry.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/zipFile.h"
#include "pxr/usd/usdUtils/dependencies.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvar.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/xformCommonAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdShade/material.h"

#include <sys/time.h>

double _getCurrentTime_() {
    struct timeval v;
    double seconds;
    double useconds;
    
    gettimeofday(&v, NULL);
    seconds = v.tv_sec;
    useconds = (1.0e-6) * v.tv_usec;
    seconds += useconds;
    return seconds;
}

// SketchUp thinks in inches, we want centimeters
static double inchesToCM = 2.54;
// SketchUp's default frontface color
static pxr::GfVec4d defaultFrontFaceRGBA(1.0, 1.0, 1.0, 1.0);
// SketchUp's default backface color
static pxr::GfVec4d defaultBackFaceRGBA(198.0/255.0, 214.0/255.0, 224.0/255.0,
                                        1.0);
static std::string componentDefinitionSuffix = "__SUComponentDefinition";
static std::string instanceSuffix = "__USDInstance_";

static std::string frontSide = "FrontSide";
static std::string backSide = "BackSide";
static std::string bothSides = "BothSides";

class InitUSDPluginPath {
public:
    InitUSDPluginPath() {
        std::string dir = pxr::TfGetPathName(pxr::ArchGetExecutablePath());
        std::string pluginDir = pxr::TfStringCatPaths(dir,
                                                      "../PlugIns/USDExporter.plugin/Contents/Resources/usd/");
        pxr::PlugRegistry::GetInstance().RegisterPlugins(pluginDir);
    }
};

// constructor runs once to set USD plugin path
static InitUSDPluginPath getUSDPlugInPathSet;

MeshSubset::MeshSubset(std::string materialTextureName, pxr::VtArray<int> faceIndices) :
                        _materialTextureName(materialTextureName),
                        _faceIndices(faceIndices) {
}

MeshSubset::~MeshSubset() {
}
    
const std::string
MeshSubset::GetMaterialTextureName() {
    return _materialTextureName;
}

const pxr::VtArray<int> MeshSubset::GetFaceIndices() {
    return _faceIndices;
}

pxr::SdfPath
MeshSubset::GetMaterialPath() {
    return _materialPath;
}

void
MeshSubset::SetMaterialPath(pxr::SdfPath path) {
    _materialPath = pxr::SdfPath(path);
}

USDExporter::USDExporter(): _model(SU_INVALID), _textureWriter(SU_INVALID) {
    SUInitialize();
    SU_CALL(SUTextureWriterCreate(&_textureWriter));
    SetExportMeshes(true);
    SetExportCameras(true);
    SetExportMaterials(true);
    SetExportNormals(false);
    SetExportEdges(true);
    SetExportLines(true);
    SetExportCurves(true);
    SetExportToSingleFile(false);
    SetExportARKitCompatibleUSDZ(true);
    SetAspectRatio(16.0/9.0);
    SetSensorHeight(24.0);
    SetStartFrame(101.0);
    SetFrameIncrement(48.0);
    _progressCallback = NULL;
}

USDExporter::~USDExporter() {
    if (!SUIsInvalid(_textureWriter)) {
        SUTextureWriterRelease(&_textureWriter);
        SUSetInvalid(_textureWriter);
    }
    SUTerminate();
}

#pragma mark Public converter:

bool
USDExporter::Convert(const std::string& skpSrc, const std::string& usdDst,
                      SketchUpPluginProgressCallback* progress_callback) {
    _progressCallback = progress_callback;
    return _performExport(skpSrc, usdDst);
}

#pragma mark Do the real work:
bool
USDExporter::_performExport(const std::string& skpSrc,
                            const std::string& usdDst) {
    double startTime = _getCurrentTime_();
    double geomTime = 0.0;
    double texturesTime = 0.0;
    double componentsTime = 0.0;
    double camerasTime = 0.0;
    double usdzTime = 0.0;
    double exportTime = 0.0;
    
    // these values will get updated while we do the export, and will be
    // used by the summary text presented to the user at the end of the export.
    _componentDefinitionCount = 0;
    _componentInstanceCount = 0;
    _meshCount = 0;
    _edgesCount = 0;
    _linesCount = 0;
    _curvesCount = 0;
    _camerasCount = 0;
    _materialsCount = 0;
    _geomSubsetsCount = 0;
    _filePathsForZip.clear();
    _exportTimeSummary.clear();
    _useSharedFallbackMaterial = false;
    
    _exportingUSDZ = false;
    SetSkpFileName(skpSrc);
    SU_CALL(SUModelCreateFromFile(&_model, _skpFileName.c_str()));
    SetUSDFileName(usdDst);

    _stage = pxr::UsdStage::CreateNew(_baseFileName);
    if (!_stage) {
        if (!SUIsInvalid(_model)) {
            SUModelRelease(&_model);
            SUSetInvalid(_model);
        }
        std::cerr << "Failed to create USD file " << _usdFileName << std::endl;
        throw std::exception();
    }
    if (_exportingUSDZ) {
        // we want to add the USD files without their full path
        std::string fileNameOnly = pxr::TfGetBaseName(_baseFileName);
        _filePathsForZip.insert(fileNameOnly);
        // if we're exporting USDZ, we're first going to create a .usdc file
        // and then a texture directory. We will create those in a tmp
        // directory. All that machinery is hidden when the name is set.
    }

    UsdGeomSetStageUpAxis(_stage, pxr::UsdGeomTokens->z); // SketchUp is Z-up
    std::string parentPath("/");
    // now make a top level scope that is the name of the file
    // to hold the scene & cameras.
    auto baseName = pxr::TfGetBaseName(_usdFileName);
    auto baseNameNoExt = pxr::TfStringGetBeforeSuffix(baseName);
    // Because we want to use this as a USD scope name, we need to make
    // it "safe".
    auto safeBaseNameNoExt = pxr::TfMakeValidIdentifier(baseNameNoExt);
    if (safeBaseNameNoExt != baseNameNoExt) {
        std::cerr << "WARNING: had to change top level scope from "
                << baseNameNoExt << " to "
                << safeBaseNameNoExt << " to be a valid USD scope name"
        << std::endl;
    }
    pxr::SdfPath path(parentPath + safeBaseNameNoExt);
    if (GetExportMaterials()) {
        // only do this if we're exporting materials
        double startTimeTextures = _getCurrentTime_();
        _ExportTextures(path); // do this first so we know our _textureDirectory
        texturesTime = _getCurrentTime_() - startTimeTextures;
        _ExportFallbackDisplayMaterial(path);
    }
    pxr::SdfPath parentPathS(parentPath);
    double startTimeComponents = _getCurrentTime_();
    _ExportComponentDefinitions(parentPathS);
    componentsTime = _getCurrentTime_() - startTimeComponents;

    auto primSchema = pxr::UsdGeomXform::Define(_stage,
                                                pxr::SdfPath(path));
    _stage->SetDefaultPrim(primSchema.GetPrim());
    pxr::UsdPrim prim = primSchema.GetPrim();
    prim.SetMetadata(pxr::SdfFieldKeys->Kind, pxr::KindTokens->assembly);

    double startTimeGeom = _getCurrentTime_();
    _ExportGeom(path);
    geomTime =  _getCurrentTime_() - startTimeGeom;

    if (GetExportCameras()) {
        double startTimeCameras = _getCurrentTime_();
        _ExportCameras(path);
        camerasTime = _getCurrentTime_() - startTimeCameras;
    }
    _stage->Save();
    
    if (_exportingUSDZ) {
        double startTimeUSDZ = _getCurrentTime_();
        if (GetExportARKitCompatibleUSDZ()) {
            pxr::SdfAssetPath p = pxr::SdfAssetPath(_stage->GetRootLayer()->GetRealPath());
            pxr::ArGetResolver().CreateDefaultContextForAsset(p.GetAssetPath());
            bool wroteIt = pxr::UsdUtilsCreateNewARKitUsdzPackage(p,
                                                                  _zipFileName);
            if (!wroteIt) {
                std::cerr << "ERROR: unable to write ARKit compatible USDZ "
                << path << " to filename " << _zipFileName << std::endl;
                throw std::exception();
            }
        } else {
            auto zipWriter = pxr::UsdZipFileWriter().CreateNew(_zipFileName);
            for (auto filePath : _filePathsForZip) {
                zipWriter.AddFile(filePath);
            }
            zipWriter.Save();
        }
        usdzTime = _getCurrentTime_() - startTimeUSDZ;
    }
    exportTime = _getCurrentTime_() - startTime;
    char buffer[256]; // this is asking for trouble, but not sure a clearer way
    sprintf(buffer, "USD Export took %3.2lf secs\n", exportTime);
    _exportTimeSummary += std::string(buffer);
    if (texturesTime > 1.0) {
        sprintf(buffer, "\tTextures Export took %3.2lf secs\n", texturesTime);
        _exportTimeSummary += std::string(buffer);
    }
    if (componentsTime > 1.0) {
        sprintf(buffer, "\tComponents Export took %3.2lf secs\n", componentsTime);
        _exportTimeSummary += std::string(buffer);
    }
    if (geomTime > 1.0) {
        sprintf(buffer, "\tScene Geometry Export took %3.2lf secs\n", geomTime);
        _exportTimeSummary += std::string(buffer);
    }
    if (camerasTime > 1.0) {
        sprintf(buffer, "\tCameras Export took %3.2lf secs\n", camerasTime);
        _exportTimeSummary += std::string(buffer);
    }
    if (usdzTime > 1.0) {
        sprintf(buffer, "\tUSDZ Export took %3.2lf secs\n", usdzTime);
        _exportTimeSummary += std::string(buffer);
    }

    if (!SUIsInvalid(_model)) {
        SUModelRelease(&_model);
        SUSetInvalid(_model);
    }
    return true;
}

#pragma mark Setters/Getters:

const std::string
USDExporter::GetSkpFileName() const {
    return _skpFileName;
}

const std::string
USDExporter::GetUSDFileName() const {
    return _usdFileName;
}

bool
USDExporter::GetExportNormals() const {
    return _exportNormals;
}

bool
USDExporter::GetExportEdges() const {
    return _exportEdges;
}

bool
USDExporter::GetExportLines() const {
    return _exportLines;
}

bool
USDExporter::GetExportCurves() const {
    return _exportCurves;
}

bool
USDExporter::GetExportToSingleFile() const {
    return _exportToSingleFile;
}

bool
USDExporter::GetExportARKitCompatibleUSDZ() const {
    return _exportARKitCompatibleUSDZ;
}

bool
USDExporter::GetExportMaterials()const {
    return _exportMaterials;
}

bool
USDExporter::GetExportMeshes()const {
    return _exportMeshes;
}

bool
USDExporter::GetExportCameras()const {
    return _exportCameras;
}

void
USDExporter::_updateFileNames() {
    _baseFileName = _usdFileName;
    std::string ext = pxr::TfStringGetSuffix(_usdFileName);
    std::string path = pxr::TfGetPathName(_usdFileName);
    std::string base = pxr::TfGetBaseName(_usdFileName);
    std::string baseNoExt = pxr::TfStringGetBeforeSuffix(base);
    _textureDirectory = std::string(path + baseNoExt + "_textures");
    
    _exportingUSDZ = false;
    if (ext == "usdz") {
        _exportingUSDZ = true;
        _zipFileName = path + baseNoExt + ".usdz";
        std::string tmpPath(pxr::ArchGetTmpDir());
        _baseFileName = tmpPath + baseNoExt + ".usdc";
        _textureDirectory = std::string(tmpPath + baseNoExt + "_textures");
        if (GetExportARKitCompatibleUSDZ()) {
            // note: if we're exporting USDZ, for now, to be ARKit compatible,
            // they want a single binary USD file, so we will flip that switch
            // Also, ARKit expects the one and only USD file in there to end w/c
            _exportToSingleFile = true;
        }
        return ;
    }
    if (!_exportToSingleFile) {
        // could be usda or crate
        _geomFileName = path + baseNoExt + ".geom." + ext;
        _componentDefinitionsFileName = path + baseNoExt + ".components." + ext;
    }
}

void
USDExporter::SetSkpFileName(const std::string name) {
    _skpFileName = name;
}

void
USDExporter::SetUSDFileName(const std::string name) {
    _usdFileName = name;
    _updateFileNames();
}

void
USDExporter::SetExportNormals(bool flag) {
    _exportNormals = flag;
}

void
USDExporter::SetExportEdges(bool flag) {
    _exportEdges = flag;
}

void
USDExporter::SetExportLines(bool flag) {
    _exportLines = flag;
}

void
USDExporter::SetExportCurves(bool flag) {
    _exportCurves = flag;
}

void
USDExporter::SetExportToSingleFile(bool flag) {
    _exportToSingleFile = flag;
    _updateFileNames();
}

void
USDExporter::SetExportARKitCompatibleUSDZ(bool flag) {
    _exportARKitCompatibleUSDZ = flag;
    _updateFileNames();
}

void
USDExporter::SetExportMaterials(bool flag) {
    _exportMaterials = flag;
}

void
USDExporter::SetExportMeshes(bool flag) {
    _exportMeshes = flag;
}

void
USDExporter::SetExportCameras(bool flag) {
    _exportCameras = flag;
}

double
USDExporter::GetSensorHeight() const {
    return _sensorHeight;
}

double
USDExporter::GetAspectRatio() const {
    return _aspectRatio;
}

double
USDExporter::GetStartFrame() const {
    return _startFrame;
}

double
USDExporter::GetFrameIncrement() const {
    return _frameIncrement;
}

void
USDExporter::SetAspectRatio(double ratio) {
    _aspectRatio = ratio;
}

void
USDExporter::SetSensorHeight(double height) {
    _sensorHeight = height;
}

void
USDExporter::SetStartFrame(double frame) {
    _startFrame = frame;
}

void
USDExporter::SetFrameIncrement(double frame) {
    _frameIncrement = frame;
}

unsigned long long
USDExporter::GetComponentDefinitionCount() {
    return _componentDefinitionCount;
}

unsigned long long
USDExporter::GetComponentInstanceCount() {
    return _componentInstanceCount;
}

unsigned long long
USDExporter::GetMeshCount() {
    return _meshCount;
}

unsigned long long
USDExporter::GetEdgesCount() {
    return _edgesCount;
}

unsigned long long
USDExporter::GetLinesCount() {
    return _linesCount;
}

unsigned long long
USDExporter::GetCurvesCount() {
    return _curvesCount;
}

unsigned long long
USDExporter::GetCamerasCount() {
    return _camerasCount;
}

unsigned long long
USDExporter::GetMaterialsCount() {
    return _materialsCount;
}

unsigned long long
USDExporter::GetGeomSubsetsCount() {
    return _geomSubsetsCount;
}

std::string
USDExporter::GetExportTimeSummary() {
    return _exportTimeSummary;
}

#pragma mark Components:

void
USDExporter::_ExportComponentDefinitions(const pxr::SdfPath parentPath) {
    size_t num_comp_defs = 0;
    SU_CALL(SUModelGetNumComponentDefinitions(_model, &num_comp_defs));
    if (!num_comp_defs) {
        return ;
    }
    if (!_countComponentDefinitionsActuallyUsed()) {
        return ;
    }
    pxr::UsdStageRefPtr topLevelStage = _stage;
    if (!GetExportToSingleFile()) {
        // open a new file and write the SketchUp component definitions there:
        _stage = pxr::UsdStage::CreateNew(_componentDefinitionsFileName);
        if (!_stage) {
            std::cerr << "Failed to create USD file "
                      << _componentDefinitionsFileName << std::endl;
            throw std::exception();
        }
        std::string fileNameOnly = pxr::TfGetBaseName(_componentDefinitionsFileName);
        _filePathsForZip.insert(fileNameOnly);
        UsdGeomSetStageUpAxis(_stage, pxr::UsdGeomTokens->z); // SketchUp is Z-up
    }

    auto primSchema = pxr::UsdGeomXform::Define(_stage, parentPath);

    _usedComponentNames.clear();
    std::vector<SUComponentDefinitionRef> comp_defs(num_comp_defs);
    SU_CALL(SUModelGetComponentDefinitions(_model, num_comp_defs,
                                           &comp_defs[0], &num_comp_defs));
    _componentDefinitionCount = num_comp_defs;
    std::string msg = std::string("Writing ") + std::to_string(num_comp_defs)
        + " Component Definitions";
    SU_HandleProgress(_progressCallback, 10.0, msg);
    for (size_t def = 0; def < num_comp_defs; ++def) {
        _ExportComponentDefinition(parentPath, comp_defs[def]);
    }
    if (!GetExportToSingleFile()) {
        _stage->Save();
        _stage = topLevelStage;
    }
}

void
USDExporter::_ExportComponentDefinition(const pxr::SdfPath parentPath,
                                        SUComponentDefinitionRef comp_def) {
    _currentComponentDefinition = comp_def;
    std::string name = GetComponentDefinitionName(comp_def);
    if (_instancedComponentNames.find(name) == _instancedComponentNames.end()) {
        // this component was not actually instanced, so move on to the next one
        return;
    }
    // this name might not be a valid USD scope name so we have to make it safe
    std::string cName = pxr::TfMakeValidIdentifier(name) + componentDefinitionSuffix;
    cName = SafeNameFromExclusionList(cName, _usedComponentNames);
    // okay, now that we have a version of this name that we can use as a USD
    // scope, we need to store a few relationships that we'll need later to
    // track back to this specific component definition:
    _originalComponentNameSafeNameDictionary[name] = cName; // for metadata
    _usedComponentNames.insert(cName); // so we know not to reuse it
    uintptr_t index = reinterpret_cast<uintptr_t>(comp_def.ptr);
    // so we can find this name given an instance
    _componentPtrSafeNameMap[index] = cName;
    
    SUEntitiesRef entities = SU_INVALID;
    SUComponentDefinitionGetEntities(comp_def, &entities);
    
    const pxr::TfToken child(cName);
    //std::cerr << "appending " << child << " to parentPath" << parentPath << std::endl;
    const pxr::SdfPath path = parentPath.AppendChild(child);
    auto primSchema = pxr::UsdGeomXform::Define(_stage, path);
    primSchema.GetPrim().SetMetadata(pxr::SdfFieldKeys->Kind,
                                     pxr::KindTokens->component);
    auto keyPath = pxr::TfToken("SketchUp:name");
    pxr::VtValue nameV(name);
    primSchema.GetPrim().SetCustomDataByKey(keyPath, nameV);
    
    SUComponentBehavior behavior;
    SU_CALL(SUComponentDefinitionGetBehavior(comp_def, &behavior));
    _isBillboard = behavior.component_always_face_camera;
    
    // Before we do anything else, we should export our fallback material here
    _ExportFallbackDisplayMaterial(path);
    
    _ExportEntities(path, entities);

    if (GetExportToSingleFile()) {
        // if we're exporting to a single file, we don't want these showing
        // up on the stage, so we use "over" (or "class" would work too).
        // we're using a pattern from:
// https://graphics.pixar.com/usd/docs/api/class_usd_geom_point_instancer.html
        // note that it is vital that we set the specifier *after* we
        // have specified all our children, as we expect them to be using
        // "def" with abandon.  Also note that if we use "class" instead of
        // "over" we could have set it before calling the children, since both
        // "def" and "class" would qualify as actually there.
        primSchema.GetPrim().SetSpecifier(pxr::SdfSpecifierOver);
    }
}

int
USDExporter::_countComponentDefinitionsActuallyUsed() {
    SUEntitiesRef model_entities;
    SU_CALL(SUModelGetEntities(_model, &model_entities));
    // We first need to confirm that a given definition is actually instanced
    // in this file. If not, we shouldn't bother to write it out.
    _instancedComponentNames.clear();
    return _countEntities(model_entities);
}

int
USDExporter::_countEntities(SUEntitiesRef entities) {
    int instancedComponents = 0;
    size_t num_instances = 0;
    SU_CALL(SUEntitiesGetNumInstances(entities, &num_instances));
    if (num_instances > 0) {
        std::vector<SUComponentInstanceRef> instances(num_instances);
        SU_CALL(SUEntitiesGetInstances(entities, num_instances,
                                       &instances[0], &num_instances));
        for (size_t c = 0; c < num_instances; c++) {
            SUComponentInstanceRef instance = instances[c];
            SUComponentDefinitionRef definition = SU_INVALID;
            
            SUDrawingElementRef de = SUComponentInstanceToDrawingElement(instance);
            if (SUIsValid(de)) {
                bool isHidden = false;
                SUDrawingElementGetHidden(de, &isHidden);
                if (isHidden) {
                    continue;
                }
                // need to find out what layer it's on, and make sure that layer
                // is visible
                SULayerRef layer;
                SU_CALL(SUDrawingElementGetLayer(de, &layer));
                bool visible = true;
                SU_CALL(SULayerGetVisibility(layer, &visible));
                if (!visible) {
                    //std::cerr << cName << " is on a hidden layer - skipping" << std::endl;
                    continue;
                }
            }
            SU_CALL(SUComponentInstanceGetDefinition(instance, &definition));
            std::string definitionName = GetComponentDefinitionName(definition);
            _instancedComponentNames.insert(definitionName);
            SUEntitiesRef subEntities = SU_INVALID;
            SUComponentDefinitionGetEntities(definition, &subEntities);
            instancedComponents += _countEntities(subEntities);
        }
        instancedComponents += num_instances;
    }
    size_t num_groups = 0;
    SU_CALL(SUEntitiesGetNumGroups(entities, &num_groups));
    if (!num_groups) {
        return  instancedComponents;
    }
    std::vector<SUGroupRef> groups(num_groups);
    SU_CALL(SUEntitiesGetGroups(entities, num_groups, &groups[0], &num_groups));
    for (size_t g = 0; g < num_groups; g++) {
        SUGroupRef group = groups[g];
        SUEntitiesRef group_entities = SU_INVALID;
        SU_CALL(SUGroupGetEntities(group, &group_entities));
        instancedComponents += _countEntities(group_entities);
    }
    return instancedComponents;
}

#pragma mark SceneGraph:

void
USDExporter::_ExportTextures(const pxr::SdfPath parentPath) {
    // note: if this is a usdz file, we put the textureDirectory
    // into the tmp dir we're writing the usdc to
    USDTextureHelper textureHelper;
    if (!textureHelper.LoadAllTextures(_model, _textureWriter, false)) {
        return ;
    }
    if (textureHelper.MakeTextureDirectory(_textureDirectory)) {
        SU_CALL(SUTextureWriterWriteAllTextures(_textureWriter,
                                                _textureDirectory.c_str()));
    } else {
        std::cerr << "unable to make directory to store textures in: "
        << _textureDirectory << std::endl;
        return ;
    }
    // at the end, we cut down the texture directory name here for referencing
    // we just want the directory, not the whole path
    _textureDirectory = pxr::TfGetBaseName(_textureDirectory);
}

void
USDExporter::_ExportFallbackDisplayMaterial(const pxr::SdfPath path) {
    _useSharedFallbackMaterial = true;
    std::string materialName = "FallbackDisplayMaterial";
    _fallbackDisplayMaterialPath = path.AppendChild(pxr::TfToken(materialName));
    // now we need to define it:
    _ExportDisplayMaterial(_fallbackDisplayMaterialPath);
}

void
USDExporter::_ExportGeom(const pxr::SdfPath parentPath) {
    // If not saving to a single file, create a new sublayer for geometry on
    // the stage and grab an edit target that points to that sublayer,
    // otherwise leave the editTarget to the current one on the stage.
    // Note that this is a different approach than we took for writing out
    // the components where we opened a new stage and wrote them there.
    pxr::UsdEditTarget editTarget = _stage->GetEditTarget();
    if (!GetExportToSingleFile()) {
        pxr::SdfLayerRefPtr geomSublayer = pxr::SdfLayer::CreateNew(_geomFileName);
        std::string fileNameOnly = pxr::TfGetBaseName(_geomFileName);
        _filePathsForZip.insert(fileNameOnly);
        // we want to make this path relative:
        std::string layerPath("./" + pxr::TfGetBaseName(_geomFileName));
        _stage->GetRootLayer()->InsertSubLayerPath(layerPath);
        editTarget = _stage->GetEditTargetForLocalLayer(geomSublayer);
    }
    // Use a UsdEditContext to direct subsequent edits to the desired edit
    // target until it goes out of scope.
    pxr::UsdEditContext editContext(_stage, editTarget);

    SUEntitiesRef model_entities;
    SU_CALL(SUModelGetEntities(_model, &model_entities));
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("Geom"));
    auto primSchema = pxr::UsdGeomXform::Define(_stage, path);
    // this will be eventually be used to figure out which shader to emit.
    _isBillboard = false;
    std::string msg = std::string("Writing Geometry");
    SU_HandleProgress(_progressCallback, 40.0, msg);
    _ExportEntities(path, model_entities);
}

void
USDExporter::_ExportEntities(const pxr::SdfPath parentPath,
                             SUEntitiesRef entities) {
    _ExportInstances(parentPath, entities);
    _ExportGroups(parentPath, entities);
    if (GetExportMeshes()) {
        _ExportFaces(parentPath, entities);
    }
    if (GetExportEdges()) {
        _ExportEdges(parentPath, entities);
    }
    if (GetExportCurves()) {
        _ExportCurves(parentPath, entities);
    }
    if (GetExportLines()) {
        _ExportPolylines(parentPath, entities);
    }
}

#pragma mark Instances:
void
USDExporter::_ExportInstances(const pxr::SdfPath parentPath,
                              SUEntitiesRef entities) {
    size_t num = 0;
    SU_CALL(SUEntitiesGetNumInstances(entities, &num));
    if (!num) {
        return ;
    }
    std::vector<SUComponentInstanceRef> instances(num);
    SU_CALL(SUEntitiesGetInstances(entities, num, &instances[0], &num));
    _componentInstanceCount += num;
    for (size_t i = 0; i < num; i++) {
        _ExportInstance(parentPath, instances[i]);
    }
}

bool
USDExporter::_ExportInstance(const pxr::SdfPath parentPath,
                             SUComponentInstanceRef instance) {
    SUComponentDefinitionRef definition = SU_INVALID;
    SU_CALL(SUComponentInstanceGetDefinition(instance, &definition));
    // unfortunately, we can't depend that the name of the definition will
    // be unique across the file so we need to turn this specific component's
    // pointer into something we can use as an index to find this later
    // we'll want to go from this void* to our transformed name
    uintptr_t index = reinterpret_cast<uintptr_t>(definition.ptr);
    auto cName = _componentPtrSafeNameMap[index];
    // Convert this to a drawing entity and see if it is hidden.
    // if it is, we skip to the next one:
    SUDrawingElementRef de = SUComponentInstanceToDrawingElement(instance);
    if (SUIsValid(de)) {
        bool isHidden = false;
        SUDrawingElementGetHidden(de, &isHidden);
        if (isHidden) {
            //std::cerr << cName << " is hidden - skipping" << std::endl;
            return false;
        }
        // need to find out what layer it's on, and make sure that layer
        // is visible
        SULayerRef layer;
        SU_CALL(SUDrawingElementGetLayer(de, &layer));
        bool visible = true;
        SU_CALL(SULayerGetVisibility(layer, &visible));
        if (!visible) {
            //std::cerr << cName << " is on a hidden layer - skipping" << std::endl;
            return false;
        }
    }
    // we want to keep track of how many instances for a given master/class
    // we've declared, so that we can name them with a running value.
    auto status = _instanceCountPerClass.emplace(cName, 0);
    const int instanceCount = ++(status.first->second);
    _instanceCountPerClass[cName] = instanceCount;

    // Swap "__SUComponentDefinition" with "__USDInstance_"
    // and then suffix it with the current instanceCount
    std::regex replaceExpr(componentDefinitionSuffix);
    std::string baseName = std::regex_replace(cName,
                                              replaceExpr, instanceSuffix);
    std::string instanceName = baseName + std::to_string(instanceCount);
    std::string realInstanceName = GetComponentInstanceName(instance);
    
    SUComponentBehavior behavior;
    SU_CALL(SUComponentDefinitionGetBehavior(definition, &behavior));
    _isBillboard = behavior.component_always_face_camera;

    //std::cerr << "appending instanceName " << instanceName << " to parentPath " << parentPath << std::endl;
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken(instanceName));
    auto primSchema = pxr::UsdGeomXform::Define(_stage, path);
    primSchema.GetPrim().SetInstanceable(true);
    if (_isBillboard) {
        auto keyPath = pxr::TfToken("SketchUp:billboard");
        pxr::VtValue billboard(_isBillboard);
        primSchema.GetPrim().SetCustomDataByKey(keyPath, billboard);
    }
    if (GetExportToSingleFile()) {
        // masters are always at the root
        std::string referencePath("/" + cName);
        auto prim = primSchema.GetPrim();
        prim.GetReferences().AddInternalReference(pxr::SdfPath(referencePath));
    } else {
        std::string baseName = pxr::TfGetBaseName(_componentDefinitionsFileName);
        std::string assetPath("./" + baseName);
        pxr::SdfPath primPath("/" + cName);
        primSchema.GetPrim().GetReferences().AddReference(assetPath, primPath);
    }
    SUTransformation t;
    SU_CALL(SUComponentInstanceGetTransform(instance, &t));
    pxr::GfMatrix4d usdMatrix = usdTransformFromSUTransform(t);
    primSchema.MakeMatrixXform().Set(usdMatrix, pxr::UsdTimeCode::Default());
    return true;
}

#pragma mark Groups:
void
USDExporter::_ExportGroups(const pxr::SdfPath parentPath,
                           SUEntitiesRef entities) {
    size_t num = 0;
    SU_CALL(SUEntitiesGetNumGroups(entities, &num));
    if (!num) {
        return ;
    }
    std::vector<SUGroupRef> groups(num);
    SU_CALL(SUEntitiesGetGroups(entities, num, &groups[0], &num));
    std::set<std::string> groupNamesUsed;
    for (size_t i = 0; i < num; i++) {
        std::string groupName = _ExportGroup(parentPath,
                                             groups[i], groupNamesUsed);
        if (groupName != "") {
            groupNamesUsed.insert(groupName);
        }
    }
}

std::string
USDExporter::_ExportGroup(const pxr::SdfPath parentPath, SUGroupRef group,
                          std::set<std::string>& usedGroupNames) {
    SUDrawingElementRef drawingElement = SUGroupToDrawingElement(group);
    if (SUIsValid(drawingElement)) {
        bool isHidden = false;
        SUDrawingElementGetHidden(drawingElement, &isHidden);
        if (isHidden) {
            return "";
        }
        // need to find out what layer it's on, and make sure that layer
        // is visible
        SULayerRef layer;
        SU_CALL(SUDrawingElementGetLayer(drawingElement, &layer));
        bool visible = true;
        SU_CALL(SULayerGetVisibility(layer, &visible));
        if (!visible) {
            //std::cerr << cName << " is on a hidden layer - skipping" << std::endl;
            return "";
        }

    }
    std::string groupName;
    std::string gName = GetGroupName(group);
    bool namedGroup = false;
    // If a group is left with the default name, it is a string that is
    // not empty - it has length 1, and the character is 0, so check for that.
    if (gName.empty() || (gName.length() && (gName[0] == '\0'))) {
        // unnamed group - give it a unique name
        std::string seed = "GRP_" + std::to_string(usedGroupNames.size());
        groupName = pxr::TfMakeValidIdentifier(seed);
    } else {
        groupName = pxr::TfMakeValidIdentifier(gName);
        namedGroup = true;
    }
    groupName = SafeNameFromExclusionList(groupName, usedGroupNames);
    
    SUEntitiesRef group_entities = SU_INVALID;
    SU_CALL(SUGroupGetEntities(group, &group_entities));

    //std::cerr << "appending group " << groupName << " to parentPath" << parentPath << std::endl;
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken(groupName));
    auto primSchema = pxr::UsdGeomXform::Define(_stage, path);
    SUTransformation t;
    SU_CALL(SUGroupGetTransform(group, &t));
    pxr::GfMatrix4d usdMatrix = usdTransformFromSUTransform(t);
    primSchema.MakeMatrixXform().Set(usdMatrix);
    if (namedGroup) {
        auto prim = primSchema.GetPrim();
        prim.SetMetadata(pxr::SdfFieldKeys->Kind,
                         pxr::KindTokens->group);
        auto keyPath = pxr::TfToken("SketchUp:name");
        pxr::VtValue gNameV(gName);
        prim.SetCustomDataByKey(keyPath, gNameV);
    }
    // now recursively export all the children, which can contain any
    // valid SketchUp entity.
    _ExportEntities(path, group_entities);
    
    return groupName;
}

#pragma mark Meshes:
void
USDExporter::_clearFacesExport() {
    _points.clear();
    _vertexNormals.clear();
    _vertexFlippedNormals.clear();
    _frontFaceTextureName.clear();
    _frontFaceTextureName.clear();
    _backUVs.clear();
    _frontUVs.clear();
    _frontFaceRGBs.clear();
    _frontFaceAs.clear();
    _backFaceRGBs.clear();
    _backFaceAs.clear();
    _faceVertexCounts.clear();
    _flattenedFaceVertexIndices.clear();
    _currentVertexIndex = 0;
    _meshFrontFaceSubsets.clear();
    _meshBackFaceSubsets.clear();
    _texturePathMaterialPath.clear();
}

std::pair<pxr::UsdShadeInput, pxr::UsdShadeInput>
USDExporter::_exportPreviewShader(const pxr::SdfPath path,
                                  pxr::UsdShadeOutput materialSurface) {
    pxr::SdfPath shaderPath = path.AppendChild(pxr::TfToken("PbrPreview"));
    auto schema = pxr::UsdShadeShader::Define(_stage, shaderPath);
    schema.CreateIdAttr().Set(pxr::TfToken("UsdPreviewSurface"));
    auto surfaceOutput = schema.CreateOutput(pxr::TfToken("surface"),
                                               pxr::SdfValueTypeNames->Token);
    materialSurface.ConnectToSource(surfaceOutput);
    
    schema.CreateInput(pxr::TfToken("useSpecularWorkflow"),
                       pxr::SdfValueTypeNames->Int).Set(0);
    schema.CreateInput(pxr::TfToken("specularColor"),
                       pxr::SdfValueTypeNames->Color3f).Set(pxr::GfVec3f(0, 0, 0));
    schema.CreateInput(pxr::TfToken("clearcoat"),
                       pxr::SdfValueTypeNames->Float).Set(0.0f);
    schema.CreateInput(pxr::TfToken("clearcoatRoughness"),
                       pxr::SdfValueTypeNames->Float).Set(0.01f);
    schema.CreateInput(pxr::TfToken("emissiveColor"),
                       pxr::SdfValueTypeNames->Color3f).Set(pxr::GfVec3f(0, 0, 0));
    schema.CreateInput(pxr::TfToken("displacement"),
                       pxr::SdfValueTypeNames->Float).Set(0.0f);
    schema.CreateInput(pxr::TfToken("occlusion"),
                       pxr::SdfValueTypeNames->Float).Set(1.0f);
    schema.CreateInput(pxr::TfToken("normal"),
                       pxr::SdfValueTypeNames->Float3).Set(pxr::GfVec3f(0, 0, 1));
    schema.CreateInput(pxr::TfToken("ior"),
                       pxr::SdfValueTypeNames->Float).Set(1.5f);
    schema.CreateInput(pxr::TfToken("metallic"),
                       pxr::SdfValueTypeNames->Float).Set(0.0f);
    schema.CreateInput(pxr::TfToken("roughness"),
                       pxr::SdfValueTypeNames->Float).Set(0.8f);
    
    auto opacity = schema.CreateInput(pxr::TfToken("opacity"),
                                      pxr::SdfValueTypeNames->Float);
    auto diffuseColor = schema.CreateInput(pxr::TfToken("diffuseColor"),
                              pxr::SdfValueTypeNames->Color3f);
    return std::make_pair(diffuseColor, opacity);
}

pxr::UsdShadeOutput
USDExporter::_exportSTPrimvarShader(const pxr::SdfPath path) {
    pxr::SdfPath shaderPath = path.AppendChild(pxr::TfToken("PrimvarST"));
    auto schema = pxr::UsdShadeShader::Define(_stage, shaderPath);
    schema.CreateIdAttr().Set(pxr::TfToken("UsdPrimvarReader_float2"));
    schema.CreateInput(pxr::TfToken("varname"),
                       pxr::SdfValueTypeNames->Token).Set(pxr::TfToken("st"));
    return schema.CreateOutput(pxr::TfToken("result"),
                               pxr::SdfValueTypeNames->Float2);
}

pxr::UsdShadeOutput
USDExporter::_exportDisplayColorPrimvarShader(const pxr::SdfPath path) {
    pxr::SdfPath shaderPath = path.AppendChild(pxr::TfToken("PrimvarDisplayColor"));
    auto schema = pxr::UsdShadeShader::Define(_stage, shaderPath);
    schema.CreateIdAttr().Set(pxr::TfToken("UsdPrimvarReader_float3"));
    schema.CreateInput(pxr::TfToken("varname"),
                       pxr::SdfValueTypeNames->Token).Set(pxr::TfToken("displayColor"));
    return schema.CreateOutput(pxr::TfToken("result"),
                               pxr::SdfValueTypeNames->Float3);
}

pxr::UsdShadeOutput
USDExporter::_exportDisplayOpacityPrimvarShader(const pxr::SdfPath path) {
    pxr::SdfPath shaderPath = path.AppendChild(pxr::TfToken("PrimvarDisplayOpacity"));
    auto schema = pxr::UsdShadeShader::Define(_stage, shaderPath);
    schema.CreateIdAttr().Set(pxr::TfToken("UsdPrimvarReader_float"));
    schema.CreateInput(pxr::TfToken("varname"),
                       pxr::SdfValueTypeNames->Token).Set(pxr::TfToken("displayOpacity"));
    return schema.CreateOutput(pxr::TfToken("result"),
                               pxr::SdfValueTypeNames->Float);
}

void
USDExporter::_exportTextureShader(const pxr::SdfPath path,
                                  std::string texturePath,
                                  pxr::UsdShadeOutput primvar,
                                  pxr::UsdShadeInput diffuseColor) {
    pxr::SdfPath s3Path = path.AppendChild(pxr::TfToken("Texture"));
    auto s3Schema = pxr::UsdShadeShader::Define(_stage, s3Path);
    s3Schema.CreateIdAttr().Set(pxr::TfToken("UsdUVTexture"));
    auto rgb = s3Schema.CreateOutput(pxr::TfToken("rgb"),
                                     pxr::SdfValueTypeNames->Float3);
    diffuseColor.ConnectToSource(rgb);
    
    _filePathsForZip.insert(texturePath);
    pxr::SdfAssetPath relativePath(texturePath);
    s3Schema.CreateInput(pxr::TfToken("file"),
                         pxr::SdfValueTypeNames->Asset).Set(relativePath);
    s3Schema.CreateInput(pxr::TfToken("wrapS"),
                         pxr::SdfValueTypeNames->Token).Set(pxr::TfToken("repeat"));
    s3Schema.CreateInput(pxr::TfToken("wrapT"),
                         pxr::SdfValueTypeNames->Token).Set(pxr::TfToken("repeat"));
    auto st = s3Schema.CreateInput(pxr::TfToken("st"),
                                   pxr::SdfValueTypeNames->Float2);
    st.ConnectToSource(primvar);
}

void
USDExporter::_ExportTextureMaterial(const pxr::SdfPath path,
                                    std::string texturePath) {
    _materialsCount++;
    auto mSchema = pxr::UsdShadeMaterial::Define(_stage,
                                                 pxr::SdfPath(path));
    auto materialSurface = mSchema.CreateOutput(pxr::TfToken("surface"),
                                                pxr::SdfValueTypeNames->Token);
    auto diffuseColor_opacity = _exportPreviewShader(path, materialSurface);
    auto diffuseColor = diffuseColor_opacity.first;
    auto opacity = diffuseColor_opacity.second;
    opacity.Set(1.0f); // for a textured material, it's fully opaque
    auto primvar = _exportSTPrimvarShader(path);
    _exportTextureShader(path, texturePath, primvar, diffuseColor);
}


void
USDExporter::_ExportDisplayMaterial(const pxr::SdfPath path) {
    _materialsCount++;
    auto mSchema = pxr::UsdShadeMaterial::Define(_stage,
                                                 pxr::SdfPath(path));
    auto materialSurface = mSchema.CreateOutput(pxr::TfToken("surface"),
                                                pxr::SdfValueTypeNames->Token);
    auto diffuseColor_opacity = _exportPreviewShader(path, materialSurface);

    auto primvarRGB = _exportDisplayColorPrimvarShader(path);
    auto diffuseColor = diffuseColor_opacity.first;
    diffuseColor.ConnectToSource(primvarRGB);

    auto primvarOpacity = _exportDisplayOpacityPrimvarShader(path);
    auto opacity = diffuseColor_opacity.second;
    opacity.ConnectToSource(primvarOpacity);
}

int
USDExporter::_cacheTextureMaterial(pxr::SdfPath path, MeshSubset& subset, int index) {
    std::string textureName = subset.GetMaterialTextureName();
    std::string texturePath = _textureDirectory + "/" + textureName;
    if (_texturePathMaterialPath.find(texturePath) == _texturePathMaterialPath.end()) {
        // okay, we have not yet made a material with this texture, so
        // let's make a new one and cache it for later.
        std::string materialName = "TextureMaterial";
        if (index != 0) {
            materialName += "_" + std::to_string(index);
        }
        index++;
        //std::cerr << "appending material " << materialName << " to parentPath" << path << std::endl;
        pxr::SdfPath materialPath = path.AppendChild(pxr::TfToken(materialName));
        subset.SetMaterialPath(materialPath);
        _ExportTextureMaterial(materialPath, texturePath);
        _texturePathMaterialPath[texturePath] = materialPath;
    } else {
        pxr::SdfPath materialPath = _texturePathMaterialPath[texturePath];
        subset.SetMaterialPath(materialPath);
    }
    return index;
}

int
USDExporter::_cacheDisplayMaterial(pxr::SdfPath path, MeshSubset& subset, int index) {
    if (_useSharedFallbackMaterial) {
        subset.SetMaterialPath(_fallbackDisplayMaterialPath);
        return index;
    }
    // okay, we're not going with the shared one
    std::string materialName = "DisplayMaterial";
    if (index != 0) {
        materialName += "_" + std::to_string(index);
    }
    index++;
    pxr::SdfPath materialPath = path.AppendChild(pxr::TfToken(materialName));
    subset.SetMaterialPath(materialPath);
    _ExportDisplayMaterial(materialPath);
    return index;
}

bool
USDExporter::_someMaterialsToExport() {
    for (MeshSubset& subset : _meshFrontFaceSubsets) {
        if (!subset.GetMaterialTextureName().empty()) {
            return true;
        }
        if (subset.GetMaterialPath() != _fallbackDisplayMaterialPath) {
            return true;
        }
    }
    for (MeshSubset& subset : _meshBackFaceSubsets) {
        if (!subset.GetMaterialTextureName().empty()) {
            return true;
        }
        if (subset.GetMaterialPath() != _fallbackDisplayMaterialPath) {
            return true;
        }
    }
    // there are no textures and all the display subsets are just referencing
    // the shared fallback material
    return false;
}

bool
USDExporter::_ExportMaterials(const pxr::SdfPath parentPath) {
    if (_meshFrontFaceSubsets.empty() && _meshBackFaceSubsets.empty()) {
        // no need - no materials
        return false;
    }
    // we might have a single mesh that has many materials, many of which are
    // the same. Since SketchUp has such a simple material schema (just a
    // texture map at most), we want to coalesce these as much as possible.
    if (_exportMaterials && _someMaterialsToExport()) {
        pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("Materials"));
        auto primSchema = pxr::UsdGeomScope::Define(_stage, path);
        int displayIndex = 0;
        int textureIndex = 0;
        for (MeshSubset& subset : _meshFrontFaceSubsets) {
            if (subset.GetMaterialTextureName().empty()) {
                displayIndex = _cacheDisplayMaterial(path, subset, displayIndex);
            } else {
                textureIndex = _cacheTextureMaterial(path, subset, textureIndex);
            }
        }
        for (MeshSubset& subset : _meshBackFaceSubsets) {
            if (subset.GetMaterialTextureName().empty()) {
                displayIndex = _cacheDisplayMaterial(path, subset, displayIndex);
            } else {
                textureIndex = _cacheTextureMaterial(path, subset, textureIndex);
            }
        }
    }
    return true;
}

bool
USDExporter::_bothDisplayColorAreEqual() {
    if (_frontFaceRGBs.size() != _backFaceRGBs.size()) {
        return false;
    }
    for (int i = 0; i < _frontFaceRGBs.size(); i++) {
        pxr::GfVec3f frontFaceRGB = _frontFaceRGBs[i];
        pxr::GfVec3f backFaceRGB = _backFaceRGBs[i];
        if (frontFaceRGB != backFaceRGB) {
            return false;
        }
    }
    return true;
}

bool
USDExporter::_bothDisplayOpacityAreEqual() {
    if (_frontFaceAs.size() != _backFaceAs.size()) {
        return false;
    }
    for (int i = 0; i < _frontFaceAs.size(); i++) {
        float frontFaceA = _frontFaceAs[i];
        float backFaceA = _backFaceAs[i];
        if (frontFaceA != backFaceA) {
            return false;
        }
    }
    return true;
}


void
USDExporter::_ExportFaces(const pxr::SdfPath parentPath,
                          SUEntitiesRef entities) {
    size_t num = 0;
    SU_CALL(SUEntitiesGetNumFaces(entities, &num));
    if (!num) {
        return;
    }
    _clearFacesExport();
    std::vector<SUFaceRef> faces(num);
    SU_CALL(SUEntitiesGetFaces(entities, num, &faces[0], &num));
    size_t exportedFaceCount = 0;
    // if there is more than one face, we need to use the UsdGeomSubset API
    // to specify the materials.
    // if we bisect a quad in both ways, we have 4 faces, but then each of these
    // faces generates two triangles, each of which is a separate face to USD
    // Note that as of USD 18.09 Hydra does not currently render GeomSubsets,
    // but SceneKit on iOS 12 and macOS Mojave does.
    for (size_t i = 0; i < num; i++) {
        pxr::VtArray<int> currentFaceIndices;
        size_t faceCount = _gatherFaceInfo(parentPath, faces[i]);
        for (size_t i = 0; i < faceCount; i++) {
            int index = (int)(i + exportedFaceCount);
            currentFaceIndices.push_back(index);
        }
        exportedFaceCount += faceCount;
        if (GetExportMaterials()) {
            MeshSubset frontSubset(_frontFaceTextureName, currentFaceIndices);
            _meshFrontFaceSubsets.push_back(frontSubset);
            MeshSubset backSubset(_backFaceTextureName, currentFaceIndices);
            _meshBackFaceSubsets.push_back(backSubset);
        }
    }
    if (exportedFaceCount) {
        if (GetExportMaterials()) {
            // if we exported any materials, then we should export two copies of
            // the mesh, one facing forward and one facing backwards.
            if (_ExportMaterials(parentPath)) {
                _ExportMeshes(parentPath);
            } else {
                // if we didn't export any materials, and the color is the same
                // on both sides of the face, we can put out a single
                // double-sided mesh, otherwise we need to put out a
                // front-facing mesh and a back-facing mesh, each with their
                // own displayColor and Opacity
                if (_bothDisplayColorAreEqual() && _bothDisplayOpacityAreEqual()) {
                    _ExportDoubleSidedMesh(parentPath);
                } else {
                    _ExportMeshes(parentPath);
                }
            }
        } else {
            // If the color is the same on both sides of the face, we can
            // put out a single double-sided mesh, otherwise we need to put
            // out a front-facing mesh and a back-facing mesh, each with
            // their own displayColor and Opacity
            if (_bothDisplayColorAreEqual() && _bothDisplayOpacityAreEqual()) {
                _ExportDoubleSidedMesh(parentPath);
            } else {
                _ExportMeshes(parentPath);
            }
        }
    }
}

size_t
USDExporter::_gatherFaceInfo(const pxr::SdfPath parentPath, SUFaceRef face) {
    SUDrawingElementRef drawingElement = SUFaceToDrawingElement(face);
    if (SUIsValid(drawingElement)) {
        bool isHidden = 0;
        SUDrawingElementGetHidden(drawingElement, &isHidden);
        if (isHidden) {
            return 0;
        }
        // need to find out what layer it's on, and make sure that layer
        // is visible
        SULayerRef layer;
        SU_CALL(SUDrawingElementGetLayer(drawingElement, &layer));
        bool visible = true;
        SU_CALL(SULayerGetVisibility(layer, &visible));
        if (!visible) {
            //std::cerr << cName << " is on a hidden layer - skipping" << std::endl;
            return 0;
        }
    }
    return _addFaceAsTexturedTriangles(face);
}

std::string
USDExporter::_textureFileName(SUTextureRef textureRef) {
    SUStringRef fileName;
    SUSetInvalid(fileName);
    SUStringCreate(&fileName);
    SUTextureGetFileName(textureRef, &fileName);
    size_t length;
    SUStringGetUTF8Length(fileName, &length);
    std::string string;
    string.resize(length);
    size_t returned_length;
    SUStringGetUTF8(fileName, length, &string[0], &returned_length);
    // this might be some windows name that has directory info in it
    // when we wrote it out, we ignored the path info, so we should ignore
    // it here as well. The Tf code will deal with this on Windows, but on the
    // Mac, it doesn't, so we'll need an additional check.
    std::string baseName = pxr::TfGetBaseName(string);
    if (pxr::TfGetPathName(string) == "") {
        // make sure it's not got a windows path embedded in there, like:
        // C:\Users\Owner\Pictures\Other\Textures for Google Sketchup\norway_maple_tree.jpg
        const std::string::size_type i = string.find_last_of("\\/");
        if (string.size() > i) {
            baseName = string.substr(i+1, string.size());
        }
    }
    return baseName;
}

// In SketchUp, a face can have:
// - no material (so we use a default color based on if it's front or back)
// - material containing a color and NO texture
// - material containing NO color and A texture
// - material containing A solid color and A texture
// so we're guaranteed to have a color, but not a texture
// we return a bool that corresponds to if we've changed from the default
void
USDExporter::_addFrontFaceMaterial(SUFaceRef face) {
    _frontRGBA = defaultFrontFaceRGBA;
    SUMaterialRef material = SU_INVALID;
    SUFaceGetFrontMaterial(face, &material);
    if SUIsInvalid(material) {
        return ;
    }
    SUColor color;
    SU_RESULT result = SUMaterialGetColor(material, &color);
    if (result == SU_ERROR_NONE) {
        _frontRGBA[0] = ((int)color.red)/255.0;
        _frontRGBA[1] = ((int)color.green)/255.0;
        _frontRGBA[2] = ((int)color.blue)/255.0;
        _frontRGBA[3] = ((int)color.alpha)/255.0;
    }
    SUTextureRef textureRef = SU_INVALID;
    if (SU_ERROR_NONE == SUMaterialGetTexture(material, &textureRef)) {
        _frontFaceTextureName = _textureFileName(textureRef);
    } else {
        _frontFaceTextureName.clear();
    }
    return ;
}

void
USDExporter::_addBackFaceMaterial(SUFaceRef face) {
    _backRGBA = defaultBackFaceRGBA;
    SUMaterialRef material = SU_INVALID;
    SUFaceGetBackMaterial(face, &material);
    if SUIsInvalid(material) {
        return ;
    }
    SUColor color;
    SU_RESULT result = SUMaterialGetColor(material, &color);
    if (result == SU_ERROR_NONE) {
        _backRGBA[0] = ((int)color.red)/255.0;
        _backRGBA[1] = ((int)color.green)/255.0;
        _backRGBA[2] = ((int)color.blue)/255.0;
        _backRGBA[3] = ((int)color.alpha)/255.0;
    }
    SUTextureRef textureRef = SU_INVALID;
    if (SU_ERROR_NONE == SUMaterialGetTexture(material, &textureRef)) {
        _backFaceTextureName = _textureFileName(textureRef);
    } else {
        _backFaceTextureName.clear();
    }
    return ;
}

size_t
USDExporter::_addFaceAsTexturedTriangles(SUFaceRef face) {
    if (SUIsInvalid(face)) {
        return 0;
    }
    // let's cache our material info - if we have a non-default color & texture
    _addFrontFaceMaterial(face);
    _addBackFaceMaterial(face);

    // Create a triangulated mesh from face.
    SUMeshHelperRef mesh_ref = SU_INVALID;
    SU_CALL(SUMeshHelperCreateWithTextureWriter(&mesh_ref, face,
                                                _textureWriter));
    
    size_t num_vertices = 0;
    SU_CALL(SUMeshHelperGetNumVertices(mesh_ref, &num_vertices));
    if (!num_vertices) {
        // free all the memory we allocated here via the SU API
        SU_CALL(SUMeshHelperRelease(&mesh_ref));
        return num_vertices;
    }
    std::vector<SUPoint3D> vertices(num_vertices);
    SU_CALL(SUMeshHelperGetVertices(mesh_ref, num_vertices,
                                    &vertices[0], &num_vertices));
    
    size_t actual;
    std::vector<SUVector3D> normals(num_vertices);
    SU_CALL(SUMeshHelperGetNormals(mesh_ref, num_vertices,
                                   &normals[0], &actual));

    std::vector<SUPoint3D> front_stq(num_vertices);
    SU_CALL(SUMeshHelperGetFrontSTQCoords(mesh_ref, num_vertices,
                                          &front_stq[0], &actual));

    std::vector<SUPoint3D> back_stq(num_vertices);
    SU_CALL(SUMeshHelperGetBackSTQCoords(mesh_ref, num_vertices,
                                         &back_stq[0], &actual));

    for (size_t i = 0; i < num_vertices; i++) {
        SUPoint3D pt = vertices[i];
        // note: SketchUp uses inches. Pretty much every other DCC out
        // there uses metric units, and most use cm. Because of that,
        // I'm going to export to cm. Note we'll need to modify the
        // translate component of the objects' 4x4 and the camera's 4x4
        pxr::GfVec3f vertex(inchesToCM * pt.x,
                            inchesToCM * pt.y,
                            inchesToCM * pt.z);
        _points.push_back(vertex);

        SUVector3D nv = normals[i];
        pxr::GfVec3f vertexNormal(nv.x, nv.y, nv.z);
        _vertexNormals.push_back(vertexNormal);
        pxr::GfVec3f vertexFlippedNormal(-1.0 * nv.x, -1.0 * nv.y, -1.0 * nv.z);
        _vertexFlippedNormals.push_back(vertexFlippedNormal);
        
        pxr::GfVec2f uvFront(front_stq[i].x, front_stq[i].y);
        _frontUVs.push_back(uvFront);
        pxr::GfVec2f uvBack(back_stq[i].x, back_stq[i].y);
        _backUVs.push_back(uvBack);
    }
    size_t num_triangles = 0;
    SU_CALL(SUMeshHelperGetNumTriangles(mesh_ref, &num_triangles));
    
    const size_t num_indices = 3 * num_triangles;
    size_t num_retrieved = 0;
    std::vector<size_t> indices(num_indices);
    SU_CALL(SUMeshHelperGetVertexIndices(mesh_ref, num_indices,
                                         &indices[0], &num_retrieved));
    
    int indexOrigin = _currentVertexIndex;
    pxr::GfVec3f frontRGB(_frontRGBA[0], _frontRGBA[1], _frontRGBA[2]);
    float frontA = _frontRGBA[3];
    pxr::GfVec3f backRGB(_backRGBA[0], _backRGBA[1], _backRGBA[2]);
    float backA = _backRGBA[3];
    pxr::VtArray<int> submeshIndices;
    for (size_t i = 0; i < num_triangles; i++) {
        size_t index = i * 3;
        _faceVertexCounts.push_back(3); // Three vertices per triangle
        
        size_t localIndex = indices[index++];
        int meshIndex = int(indexOrigin + localIndex);
        submeshIndices.push_back(meshIndex);
        _flattenedFaceVertexIndices.push_back(meshIndex);
        localIndex = indices[index++];
        meshIndex = int(indexOrigin + localIndex);
        submeshIndices.push_back(meshIndex);
        _flattenedFaceVertexIndices.push_back(meshIndex);
        localIndex = indices[index++];
        meshIndex = int(indexOrigin + localIndex);
        submeshIndices.push_back(meshIndex);
        _flattenedFaceVertexIndices.push_back(meshIndex);
        // we have a front & back RGBA for each triangle, from the original face
        _frontFaceRGBs.push_back(frontRGB);
        _frontFaceAs.push_back(frontA);
        _backFaceRGBs.push_back(backRGB);
        _backFaceAs.push_back(backA);
    }
    _currentVertexIndex += num_vertices;
    // free all the memory we allocated here via the SU API
    SU_CALL(SUMeshHelperRelease(&mesh_ref));
    return num_triangles;
}

void
USDExporter::_exportMesh(pxr::SdfPath path,
                         std::vector<MeshSubset> meshSubsets,
                         pxr::TfToken const orientation,
                         pxr::VtArray<pxr::GfVec3f>& rgb,
                         pxr::VtArray<float>& a,
                         pxr::VtArray<pxr::GfVec2f>& uv,
                         pxr::VtArray<pxr::GfVec3f>& extent,
                         bool flipNormals,
                         bool doubleSided = false
                         ) {
    _meshCount++;
    auto primSchema = pxr::UsdGeomMesh::Define(_stage, path);
    primSchema.CreateExtentAttr().Set(extent);
    primSchema.CreateSubdivisionSchemeAttr().Set(pxr::UsdGeomTokens->none);
    primSchema.CreateOrientationAttr().Set(orientation);
    primSchema.CreateDoubleSidedAttr().Set(doubleSided);
    primSchema.CreatePointsAttr().Set(_points);
    if (GetExportNormals()) {
        if (_points.size() != _vertexNormals.size()) {
            // What's the right TF_XXX call to log this?
            std::cerr << "we have " << _points.size() << " points" << std::endl;
            std::cerr << "BUT we have " << _vertexNormals.size()
            << " normals" << std::endl;
        } else {
            primSchema.SetNormalsInterpolation(pxr::UsdGeomTokens->vertex);
            if (flipNormals) {
                primSchema.CreateNormalsAttr().Set(_vertexNormals);
            } else {
                primSchema.CreateNormalsAttr().Set(_vertexFlippedNormals);
            }
        }
    }
    primSchema.CreateFaceVertexCountsAttr().Set(_faceVertexCounts);
    primSchema.CreateFaceVertexIndicesAttr().Set(_flattenedFaceVertexIndices);
    auto displayColorPrimvar = primSchema.CreateDisplayColorPrimvar();
    displayColorPrimvar.Set(rgb);
    displayColorPrimvar.SetInterpolation(pxr::UsdGeomTokens->uniform);
    auto alphaPrimvar = primSchema.CreateDisplayOpacityPrimvar();
    alphaPrimvar.Set(a);
    alphaPrimvar.SetInterpolation(pxr::UsdGeomTokens->uniform);
    auto uvPrimvar = primSchema.CreatePrimvar(pxr::TfToken("st"),
                                              pxr::SdfValueTypeNames->Float2Array,
                                              pxr::UsdGeomTokens->vertex);
    uvPrimvar.Set(uv);
    if (!_exportMaterials) {
        // not exporting materials - we're done
        return ;
    }
    // we should first bind a "default material" to this mesh that will map
    // its displayColor and displayOpacity to it. Ideally, that shader would
    // have been declared at some top level, but for now, we'll declare it
    // locally just to have it. Eventually we should look into putting it
    // higher.
    
    
    // now need to bind the materials that have been already been created to
    // this mesh. If it has one material, we'll bind it to the whole mesh.
    // We need to use the UsdGeomSubset API on this
    // mesh and bind each material to the appropriate set of indices
    // note: even if there is only one, we need to use the geom subset stuff,
    // because the material might be only on a subset of the faces of this mesh.
    pxr::TfToken relName = pxr::UsdShadeTokens->materialBinding;
    pxr::TfToken bindName = pxr::UsdShadeTokens->materialBind;
    std::string subsetBaseName = "SubsetForMaterial";
    int index = 0;
    
    bool needWorkaround = true; // needed as of USD release 18.09
    if (meshSubsets.size() < 500) {
        // if we have less than 500 (pretty arbitrary), this will be performant
        needWorkaround = false;
    }
    if (needWorkaround) {
        // this is a workaround for the fact that currently, if I have to
        // create a lot (say, 1,000s or more) subsets, it can run very
        // slowly in Usd.  So we break it up into two parts - the first
        // we use the Sdf API to declare the GeomSubset, and then we assign
        // it later on, avoiding the slowdown, but is clearly a short term hack.
        {
            pxr::SdfChangeBlock block;
            auto l = _stage->GetRootLayer();
            pxr::SdfPrimSpecHandle prim = l->GetPrimAtPath(path);
            for (MeshSubset& meshSubset : meshSubsets) {
                (void)meshSubset; // to silence the unused variable warning
                std::string subsetName = subsetBaseName;
                if (index != 0) {
                    subsetName += "_" + std::to_string(index);
                }
                index++;
                pxr::SdfPrimSpec::New(prim,
                                      subsetName,
                                      pxr::SdfSpecifierDef,
                                      "GeomSubset");
            }
        }
    }
    index = 0;
    for (MeshSubset& meshSubset : meshSubsets) {
        _geomSubsetsCount++;
        std::string subsetName = subsetBaseName;
        if (index != 0) {
            subsetName += "_" + std::to_string(index);
        }
        index++;
        pxr::SdfPath subsetPath = path.AppendChild(pxr::TfToken(subsetName));
        auto subset = pxr::UsdGeomSubset::CreateGeomSubset(primSchema,
                                                           subsetName,
                                                           pxr::UsdGeomTokens->face,
                                                           meshSubset.GetFaceIndices(),
                                                           bindName,
                                                           pxr::UsdGeomTokens->nonOverlapping);
        pxr::UsdPrim prim = subset.GetPrim();
        pxr::SdfPath materialPath = meshSubset.GetMaterialPath();
        prim.CreateRelationship(relName).AddTarget(materialPath);
    }
    return ;
}

void
USDExporter::_ExportMeshes(const pxr::SdfPath parentPath) {
    // In SketchUp, each face has two distinct sides. USD can have double-sided
    // geometry, but both sides would have the same material assignment.
    // Therefore, we write out two meshes for each face, each with the
    // appropriate display color and material.
    // We also mark the front mesh as rightHanded, and the back mesh
    // as left handed, which will allow the normals to be treated correctly.
    // Note that if we wrote out explicit normals, we flip them for the back
    pxr::VtArray<pxr::GfVec3f> extent(2);
    pxr::UsdGeomPointBased::ComputeExtent(_points, &extent);
    const pxr::TfToken materials("Materials");
    pxr::SdfPath materialsPath = parentPath.AppendChild(materials);
    // this is a little clunky, because SdfPath("") will print out a warning
    // so we explicitly set it to "EmptyPath()" if there isn't a material
    const pxr::TfToken front("FrontMaterial");
    pxr::SdfPath frontMaterialsPath = materialsPath.AppendChild(front);
    if (_frontFaceTextureName.empty()) {
        frontMaterialsPath = pxr::SdfPath::EmptyPath();
    }
    //std::cerr << "appending front " << frontSide << " to parentPath" << parentPath << std::endl;
    pxr::SdfPath frontPath = parentPath.AppendChild(pxr::TfToken(frontSide));
    _exportMesh(frontPath, _meshFrontFaceSubsets,
                pxr::UsdGeomTokens->rightHanded,
                _frontFaceRGBs, _frontFaceAs, _frontUVs, extent, false, false);

    const pxr::TfToken back("BackMaterial");
    pxr::SdfPath backMaterialsPath = materialsPath.AppendChild(back);
    if (_backFaceTextureName.empty()) {
        backMaterialsPath = pxr::SdfPath::EmptyPath();
    }
    pxr::SdfPath backPath = parentPath.AppendChild(pxr::TfToken(backSide));
    _exportMesh(backPath, _meshBackFaceSubsets,
                pxr::UsdGeomTokens->leftHanded,
                _backFaceRGBs, _backFaceAs, _backUVs, extent, true, false);

    _clearFacesExport(); // free the info
}

void
USDExporter::_ExportDoubleSidedMesh(const pxr::SdfPath parentPath) {
    // In SketchUp, each face has two distinct sides, each of which could have
    // its own material. But in the case where the mesh doesn't have a material
    // assigned on either side or where the material assignments are the same,
    // it makes sense to put out the mesh once and just mark it as double-sided.
    pxr::VtArray<pxr::GfVec3f> extent(2);
    pxr::UsdGeomPointBased::ComputeExtent(_points, &extent);
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken(bothSides));
    std::vector<MeshSubset> empty;
    _exportMesh(path, empty,
                pxr::UsdGeomTokens->rightHanded,
                _frontFaceRGBs, _frontFaceAs, _frontUVs, extent, false, true);
    _clearFacesExport(); // free the info
}

#pragma mark Edges:
void
USDExporter::_ExportEdges(const pxr::SdfPath parentPath,
                          SUEntitiesRef entities) {
    size_t num_edges = 0;
    bool standAloneOnly = true; // Write only edges not connected to faces.
    SU_CALL(SUEntitiesGetNumEdges(entities, standAloneOnly, &num_edges));
    if (!num_edges) {
        return ;
    }
    std::vector<SUEdgeRef> edges(num_edges);
    SU_CALL(SUEntitiesGetEdges(entities, standAloneOnly, num_edges,
                               &edges[0], &num_edges));
    _edgePoints.clear();
    _edgeVertexCounts.clear();
    _curvesCount += num_edges;
    for (size_t i = 0; i < num_edges; i++) {
        _gatherEdgeInfo(edges[i]);
    }
    pxr::VtArray<float> widths(1);
    widths[0] = 1.0f;
    pxr::VtArray<pxr::GfVec3f> extent(2);
    pxr::UsdGeomCurves::ComputeExtent(_edgePoints, widths, &extent);

    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("Edges"));
    auto primSchema = pxr::UsdGeomBasisCurves::Define(_stage, path);
    primSchema.CreateExtentAttr().Set(extent);
    primSchema.GetPrim().SetDocumentation("Edges not part of a Face");
    primSchema.CreateTypeAttr().Set(pxr::UsdGeomTokens->linear);
    primSchema.CreatePointsAttr().Set(_edgePoints);
    primSchema.SetWidthsInterpolation(pxr::UsdGeomTokens->constant);
    primSchema.CreateWidthsAttr().Set(widths);
    primSchema.CreateCurveVertexCountsAttr().Set(_edgeVertexCounts);
    _edgePoints.clear();
    _edgeVertexCounts.clear();
}

void
USDExporter::_gatherEdgeInfo(SUEdgeRef edge) {
    if (SUIsInvalid(edge)) {
        return;
    }
    SUVertexRef start_vertex = SU_INVALID;
    SU_CALL(SUEdgeGetStartVertex(edge, &start_vertex));
    SUPoint3D startP;
    SU_CALL(SUVertexGetPosition(start_vertex, &startP));
    _edgePoints.push_back(pxr::GfVec3f(inchesToCM * startP.x,
                                      inchesToCM * startP.y,
                                      inchesToCM * startP.z));
                             
    SUVertexRef end_vertex = SU_INVALID;
    SU_CALL(SUEdgeGetEndVertex(edge, &end_vertex));
    SUPoint3D endP;
    SU_CALL(SUVertexGetPosition(end_vertex, &endP));
    _edgePoints.push_back(pxr::GfVec3f(inchesToCM * endP.x,
                                      inchesToCM * endP.y,
                                      inchesToCM * endP.z));

    _edgeVertexCounts.push_back(2);
}

#pragma mark Curves:
void
USDExporter::_ExportCurves(const pxr::SdfPath parentPath,
                           SUEntitiesRef entities) {
    size_t nCurves = 0;
    SU_CALL(SUEntitiesGetNumCurves(entities, &nCurves));
    if (!nCurves) {
        return ;
    }
    std::vector<SUCurveRef> curves(nCurves);
    SU_CALL(SUEntitiesGetCurves(entities, nCurves, &curves[0], &nCurves));
    _curvePoints.clear();
    _curveVertexCounts.clear();
    _curvesCount += nCurves;
    for (size_t i = 0; i < nCurves; i++) {
        _gatherCurveInfo(curves[i]);
    }
    pxr::VtArray<float> widths(1);
    widths[0] = 1.0f;
    pxr::VtArray<pxr::GfVec3f> extent(2);
    pxr::UsdGeomCurves::ComputeExtent(_edgePoints, widths, &extent);

    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("Curves"));
    auto primSchema = pxr::UsdGeomBasisCurves::Define(_stage, path);
    primSchema.CreateExtentAttr().Set(extent);
    primSchema.GetPrim().SetDocumentation("Curves not part of a Face");
    primSchema.CreateTypeAttr().Set(pxr::UsdGeomTokens->linear);
    primSchema.SetWidthsInterpolation(pxr::UsdGeomTokens->constant);
    primSchema.CreateWidthsAttr().Set(widths);
    primSchema.CreatePointsAttr().Set(_curvePoints);
    primSchema.CreateCurveVertexCountsAttr().Set(_curveVertexCounts);
    _curvePoints.clear();
    _curveVertexCounts.clear();
}

void
USDExporter::_gatherCurveInfo(SUCurveRef curve) {
    if (SUIsInvalid(curve)) {
        return ;
    }
    size_t num_edges = 0;
    SU_CALL(SUCurveGetNumEdges(curve, &num_edges));
    std::vector<SUEdgeRef> edges(num_edges);
    SU_CALL(SUCurveGetEdges(curve, num_edges, &edges[0], &num_edges));
    int actuallyEdgesFound = 0;
    for (size_t i = 0; i < num_edges; ++i) {
        SUEdgeRef edge = edges[i];
        if (SUIsInvalid(edge)) {
            continue ;
        }
        actuallyEdgesFound++;
        SUVertexRef start_vertex = SU_INVALID;
        SU_CALL(SUEdgeGetStartVertex(edge, &start_vertex));
        SUPoint3D startP;
        SU_CALL(SUVertexGetPosition(start_vertex, &startP));
        _curvePoints.push_back(pxr::GfVec3f(inchesToCM * startP.x,
                                           inchesToCM * startP.y,
                                           inchesToCM * startP.z));
        
        SUVertexRef end_vertex = SU_INVALID;
        SU_CALL(SUEdgeGetEndVertex(edge, &end_vertex));
        SUPoint3D endP;
        SU_CALL(SUVertexGetPosition(end_vertex, &endP));
        _curvePoints.push_back(pxr::GfVec3f(inchesToCM * endP.x,
                                           inchesToCM * endP.y,
                                           inchesToCM * endP.z));
    }
    _curveVertexCounts.push_back(2 * actuallyEdgesFound);
}

#pragma mark Polyline3D:
void
USDExporter::_ExportPolylines(const pxr::SdfPath parentPath,
                              SUEntitiesRef entities) {
    size_t nPolylines = 0;
    SU_CALL(SUEntitiesGetNumPolyline3ds(entities, &nPolylines));
    if (!nPolylines) {
        return ;
    }
    std::vector<SUPolyline3dRef> polylines(nPolylines);
    SU_CALL(SUEntitiesGetPolyline3ds(entities, nPolylines,
                                     &polylines[0], &nPolylines));
    _polylinePoints.clear();
    _polylineVertexCounts.clear();
    _curvesCount += nPolylines;
    for (size_t i = 0; i < nPolylines; i++) {
        _gatherPolylineInfo(polylines[i]);
    }
    pxr::VtArray<float> widths(1);
    widths[0] = 1.0f;
    pxr::VtArray<pxr::GfVec3f> extent(2);
    pxr::UsdGeomCurves::ComputeExtent(_edgePoints, widths, &extent);

    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("Polylines"));
    auto primSchema = pxr::UsdGeomBasisCurves::Define(_stage, path);
    primSchema.CreateExtentAttr().Set(extent);
    primSchema.GetPrim().SetDocumentation("Polylines not part of a Face");
    primSchema.CreateTypeAttr().Set(pxr::UsdGeomTokens->linear);
    primSchema.SetWidthsInterpolation(pxr::UsdGeomTokens->constant);
    primSchema.CreateWidthsAttr().Set(widths);
    primSchema.CreatePointsAttr().Set(_polylinePoints);
    primSchema.CreateCurveVertexCountsAttr().Set(_polylineVertexCounts);
    _polylinePoints.clear();
    _polylineVertexCounts.clear();
}

void
USDExporter::_gatherPolylineInfo(SUPolyline3dRef polyline) {
    if (SUIsInvalid(polyline)) {
        return ;
    }
    size_t nPoints = 0;
    SU_CALL(SUPolyline3dGetNumPoints(polyline, &nPoints));
    std::vector<SUPoint3D> pts(nPoints);
    SU_CALL(SUPolyline3dGetPoints(polyline, nPoints, &pts[0], &nPoints));
    for (size_t i = 0; i < nPoints; ++i) {
        SUPoint3D pt = pts[i];
        _polylinePoints.push_back(pxr::GfVec3f(inchesToCM * pt.x,
                                               inchesToCM * pt.y,
                                               inchesToCM * pt.z));
    }
    _polylineVertexCounts.push_back((int)nPoints);
}

#pragma mark Cameras:
void
USDExporter::_ExportCameras(const pxr::SdfPath parentPath) {
    size_t num_scenes = 0;
    SU_CALL(SUModelGetNumScenes(_model, &num_scenes));
    if (!num_scenes) {
        return ;
    }
    // don't emit the scope if we don't have any scenes/cameras
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken("SketchUpScenes"));
    auto primSchema = pxr::UsdGeomXform::Define(_stage, path);
    _usedCameraNames.clear();
    std::vector<SUSceneRef> scenes(num_scenes);
    _camerasCount = num_scenes;
    std::string msg = std::string("Writing ") + std::to_string(num_scenes)
    + " Cameras";
    SU_HandleProgress(_progressCallback, 95.0, msg);
    SU_CALL(SUModelGetScenes(_model, num_scenes, &scenes[0], &num_scenes));
    for (size_t def = 0; def < num_scenes; ++def) {
        _ExportCamera(path, scenes[def]);
    }
}

void
USDExporter::_ExportCamera(const pxr::SdfPath parentPath, SUSceneRef scene) {
    std::string cameraName = SafeNameFromExclusionList(GetSceneName(scene),
                                                       _usedCameraNames);
    _usedCameraNames.insert(cameraName);
    SUCameraRef camera;
    SU_CALL(SUSceneGetCamera(scene, &camera));

    double camAspectRatio = _aspectRatio;
    double aspect_ratio = _aspectRatio;
    SU_RESULT result = SUCameraGetAspectRatio(camera, &aspect_ratio);
    if (result == SU_ERROR_NONE) {
        // we got a specific aspect ratio for this camera - use it
        camAspectRatio = aspect_ratio;
    }

    SUPoint3D position;
    SUPoint3D target;
    SUVector3D up_vector;
    SU_CALL(SUCameraGetOrientation(camera, &position, &target, &up_vector));
    pxr::GfVec3d eyePoint(inchesToCM * position.x,
                          inchesToCM * position.y,
                          inchesToCM * position.z);
    pxr::GfVec3d centerPoint(inchesToCM * target.x,
                             inchesToCM * target.y,
                             inchesToCM * target.z);
    pxr::GfVec3d upDirection(up_vector.x,
                             up_vector.y,
                             up_vector.z);
    // SetLookAt() computes a y-up view matrix which aligns the
    // view direction with the negative z-axis.
    // We then invert that to get where to place the camera
    auto transform = pxr::GfMatrix4d().SetLookAt(eyePoint, centerPoint,
                                                 upDirection).GetInverse();
    
    pxr::SdfPath path = parentPath.AppendChild(pxr::TfToken(cameraName));
    auto primSchema = pxr::UsdGeomCamera::Define(_stage, path);

    auto prim = primSchema.GetPrim();
    auto keyPath = pxr::TfToken("SketchUp:eyePoint");
    pxr::GfVec3d eyePointSU(position.x, position.y, position.z);
    pxr::VtValue eyePointV(eyePointSU);
    prim.SetCustomDataByKey(keyPath, eyePointV);

    keyPath = pxr::TfToken("SketchUp:centerPoint");
    pxr::GfVec3d centerPointSU(target.x, target.y, target.z);
    pxr::VtValue centerPointV(centerPointSU);
    prim.SetCustomDataByKey(keyPath, centerPointV);

    keyPath = pxr::TfToken("SketchUp:upDirection");
    pxr::VtValue upDirectionV(upDirection);
    prim.SetCustomDataByKey(keyPath, centerPointV);
    
    keyPath = pxr::TfToken("SketchUp:aspectRatio");
    pxr::VtValue aspectRatioV(_aspectRatio);
    prim.SetCustomDataByKey(keyPath, aspectRatioV);
    
    // currently, these values seem pretty bogus, so not exporting for now
    bool exportClippingRange = false;
    if (exportClippingRange) {
        double zNear = 0;
        double zFar = 0;
        SU_CALL(SUCameraGetClippingDistances(camera, &zNear, &zFar));
        float scaledZNear = inchesToCM * zNear;
        float scaledZFar = inchesToCM * zFar;
        pxr::GfVec2f clippingRange(scaledZNear, scaledZFar);
        primSchema.CreateClippingRangeAttr().Set(clippingRange);
    }
    bool isPerspective = false;
    SU_CALL(SUCameraGetPerspective(camera, &isPerspective));
    keyPath = pxr::TfToken("SketchUp:isPerspective");
    pxr::VtValue isPerspectiveV(isPerspective);
    prim.SetCustomDataByKey(keyPath, isPerspectiveV);
    if (isPerspective) {
        double verticalFOV = 0;
        SU_CALL(SUCameraGetPerspectiveFrustumFOV(camera, &verticalFOV));
        keyPath = pxr::TfToken("SketchUp:perspectiveFrustrumFOV");
        pxr::VtValue verticalFOVV(verticalFOV);
        prim.SetCustomDataByKey(keyPath, verticalFOVV);

        primSchema.CreateProjectionAttr().Set(pxr::UsdGeomTokens->perspective);
        float verticalHeightMM = _sensorHeight;
        primSchema.CreateVerticalApertureAttr().Set(verticalHeightMM);
        bool useMagicNumber = true;
        if (useMagicNumber) {
            float horizontalWidthMM = verticalHeightMM * _aspectRatio;
            primSchema.CreateHorizontalApertureAttr().Set(horizontalWidthMM);
            float focalLengthMM = 20.5;
            // Empirically, this is what it looks like it should be
            // Need to come back and figure out why the math isn't
            // giving us what we need
            primSchema.CreateFocalLengthAttr().Set(focalLengthMM);
        } else {
            // for now, we assume a 35mm film back and verticalFOV
            // focalLength = (height/2) / tan(vFOV/2)
            // focalLength = (width/2) / tan(hFOV/2)
            // we should really check on the "advanced camera info",
            // but that info is easy to get via the Ruby API,
            // but it's not clear how to get at it from C++
            double radiansVersionOfFOV = pxr::GfDegreesToRadians(verticalFOV);
            double tanPart = tan(radiansVersionOfFOV/2.0);
            float focalLengthMM = verticalHeightMM/(2.0 * tanPart);
            primSchema.CreateFocalLengthAttr().Set(focalLengthMM);
            float horizontalWidthMM = _sensorHeight * _aspectRatio;
            primSchema.CreateHorizontalApertureAttr().Set(horizontalWidthMM);
        }
    } else {
        double height = 1;
        SU_CALL(SUCameraGetOrthographicFrustumHeight(camera, &height));
        keyPath = pxr::TfToken("SketchUp:orthographicFrustumHeight");
        pxr::VtValue heightV(height);
        prim.SetCustomDataByKey(keyPath, heightV);
        // NOTE: this number is very frustrating. There doesn't seem to be a
        // linear scaling from this value to what USD looks at for an
        // orthographic scale, so we pass it through as is and assume the
        // importer will modify it as needed.
        float orthographicScale = height;
        primSchema.CreateProjectionAttr().Set(pxr::UsdGeomTokens->orthographic);
        primSchema.CreateVerticalApertureAttr().Set(orthographicScale);
    }
    primSchema.MakeMatrixXform().Set(transform);
}
