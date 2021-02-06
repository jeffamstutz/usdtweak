#pragma once
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

PXR_NAMESPACE_USING_DIRECTIVE
///
void DrawRendererSettings(UsdImagingGLEngine &, UsdImagingGLRenderParams &);
void DrawOpenGLSettings(UsdImagingGLEngine &, UsdImagingGLRenderParams &);
TfToken DrawAovSettings(UsdImagingGLEngine &, TfToken selectedAOV);
void DrawColorCorrection(UsdImagingGLEngine &, UsdImagingGLRenderParams &);