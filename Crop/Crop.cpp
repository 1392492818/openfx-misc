/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-misc <https://github.com/devernay/openfx-misc>,
 * Copyright (C) 2013-2016 INRIA
 *
 * openfx-misc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-misc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-misc.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX Crop plugin.
 */

#include <cmath>
#include <cfloat> // DBL_MAX
#include <algorithm>

#include "ofxsProcessing.H"
#include "ofxsCoords.h"
#include "ofxsRectangleInteract.h"
#include "ofxsMacros.h"
#include "ofxsGenerator.h"
#include "ofxsFormatResolution.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "CropOFX"
#define kPluginGrouping "Transform"
#define kPluginDescription "Removes everything outside the defined rectangle and optionally adds black edges so everything outside is black.\n" \
    "If the 'Extent' parameter is set to 'Format', and 'Reformat' is checked, the output pixel aspect ratio is also set to this of the format.\n" \
    "This plugin does not concatenate transforms."
#define kPluginIdentifier "net.sf.openfx.CropPlugin"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kParamReformat "reformat"
#define kParamReformatLabel "Reformat"
#define kParamReformatHint "Translates the bottom left corner of the crop rectangle to be in (0,0)."
#define kParamReformatHintExtraNatron " This sets the output format only if 'Format' is selected as the output Extend. In order to actually change the format of this image stream for other Extent choices, feed the output of this node to a either a NoOp node which sets the proper format, or a Reformat node with the same extent and with 'Resize Type' set to None and 'Center' unchecked."
#define kParamReformatDefault false

#define kParamIntersect "intersect"
#define kParamIntersectLabel "Intersect"
#define kParamIntersectHint "Intersects the crop rectangle with the input region of definition instead of extending it."

#define kParamBlackOutside "blackOutside"
#define kParamBlackOutsideLabel "Black Outside"
#define kParamBlackOutsideHint "Add 1 black and transparent pixel to the region of definition so that all the area outside the crop rectangle is black."

#define kParamSoftness "softness"
#define kParamSoftnessLabel "Softness"
#define kParamSoftnessHint "Size of the fade to black around edges to apply."

static inline
double
rampSmooth(double t)
{
    t *= 2.;
    if (t < 1) {
        return t * t / (2.);
    } else {
        t -= 1.;

        return -0.5 * (t * (t - 2) - 1);
    }
}

class CropProcessorBase
    : public OFX::ImageProcessor
{
protected:
    const OFX::Image *_srcImg;
    OfxRectD _cropRect;
    OfxPointD _renderScale;
    double _par;
    OfxPointI _translation;
    bool _blackOutside;
    double _softness;
    OfxRectI _cropRectPixel;

public:
    CropProcessorBase(OFX::ImageEffect &instance)
        : OFX::ImageProcessor(instance)
        , _srcImg(0)
        , _par(1.)
        , _blackOutside(false)
        , _softness(0)
    {
        _cropRect.x1 = _cropRect.y1 = _cropRect.x2 = _cropRect.y2 = 0.;
        _cropRectPixel.x1 = _cropRectPixel.y1 = _cropRectPixel.x2 = _cropRectPixel.y2 = 0;
        _translation.x = _translation.y = 0;
    }

    /** @brief set the src image */
    void setSrcImg(const OFX::Image *v)
    {
        _srcImg = v;
    }

    void setValues(const OfxRectD& cropRect,
                   const OfxPointD& renderScale,
                   double par,
                   bool blackOutside,
                   bool reformat,
                   double softness)
    {
        _cropRect = cropRect;
        _renderScale = renderScale;
        _par = par;
        _blackOutside = blackOutside;
        _softness = softness;
        OFX::Coords::toPixelNearest(cropRect, renderScale, par, &_cropRectPixel);
        if (reformat) {
            _translation.x = -_cropRectPixel.x1;
            _translation.y = -_cropRectPixel.y1;
        } else {
            _translation.x = 0;
            _translation.y = 0;
        }
    }
};


template <class PIX, int nComponents, int maxValue>
class CropProcessor
    : public CropProcessorBase
{
public:
    CropProcessor(OFX::ImageEffect &instance)
        : CropProcessorBase(instance)
    {
    }

private:
    void multiThreadProcessImages(OfxRectI procWindow)
    {
        //assert(filter == _filter);
        for (int y = procWindow.y1; y < procWindow.y2; ++y) {
            if ( _effect.abort() ) {
                break;
            }

            PIX *dstPix = (PIX *) _dstImg->getPixelAddress(procWindow.x1, y);
            bool yblack = _blackOutside && ( y == (_cropRectPixel.y1 + _translation.y) || y == (_cropRectPixel.y2 - 1 + _translation.y) );

            for (int x = procWindow.x1; x < procWindow.x2; ++x, dstPix += nComponents) {
                bool xblack = _blackOutside && ( x == (_cropRectPixel.x1 + _translation.x) || x == (_cropRectPixel.x2 - 1 + _translation.x) );
                // treat the black case separately
                if (xblack || yblack || !_srcImg) {
                    for (int k = 0; k < nComponents; ++k) {
                        dstPix[k] =  PIX();
                    }
                } else {
                    OfxPointI p_pixel;
                    OfxPointD p;
                    p_pixel.x = x - _translation.x;
                    p_pixel.y = y - _translation.y;
                    OFX::Coords::toCanonical(p_pixel, _dstImg->getRenderScale(), _dstImg->getPixelAspectRatio(), &p);
                    double dx = std::min(p.x - _cropRect.x1, _cropRect.x2 - p.x);
                    double dy = std::min(p.y - _cropRect.y1, _cropRect.y2 - p.y);

                    if ( (dx <= 0) || (dy <= 0) ) {
                        // outside of the rectangle
                        for (int k = 0; k < nComponents; ++k) {
                            dstPix[k] =  PIX();
                        }
                    } else {
                        const PIX *srcPix = (const PIX*)_srcImg->getPixelAddress(p_pixel.x, p_pixel.y);
                        if (!srcPix) {
                            for (int k = 0; k < nComponents; ++k) {
                                dstPix[k] =  PIX();
                            }
                        } else if ( (_softness == 0) || ( (dx >= _softness) && (dy >= _softness) ) ) {
                            // inside of the rectangle
                            for (int k = 0; k < nComponents; ++k) {
                                dstPix[k] =  srcPix[k];
                            }
                        } else {
                            double tx, ty;
                            if (dx >= _softness) {
                                tx = 1.;
                            } else {
                                tx = rampSmooth(dx / _softness);
                            }
                            if (dy >= _softness) {
                                ty = 1.;
                            } else {
                                ty = rampSmooth(dy / _softness);
                            }
                            double t = tx * ty;
                            if (t >= 1) {
                                for (int k = 0; k < nComponents; ++k) {
                                    dstPix[k] =  srcPix[k];
                                }
                            } else {
                                //if (_plinear) {
                                //    // it seems to be the way Nuke does it... I could understand t*t, but why t*t*t?
                                //    t = t*t*t;
                                //}
                                for (int k = 0; k < nComponents; ++k) {
                                    dstPix[k] =  PIX(srcPix[k] * t);
                                }
                            }
                        }
                    }
                }
            }
        }
    } // multiThreadProcessImages
};


////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class CropPlugin
    : public OFX::ImageEffect
{
public:
    /** @brief ctor */
    CropPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(0)
        , _srcClip(0)
        , _extent(0)
        , _format(0)
        , _formatSize(0)
        , _formatPar(0)
        , _btmLeft(0)
        , _size(0)
        , _interactive(0)
        , _recenter(0)
        , _softness(0)
        , _reformat(0)
        , _intersect(0)
        , _blackOutside(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        assert( _dstClip && (!_dstClip->isConnected() || _dstClip->getPixelComponents() == ePixelComponentAlpha ||
                             _dstClip->getPixelComponents() == ePixelComponentRGB ||
                             _dstClip->getPixelComponents() == ePixelComponentRGBA) );
        _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        assert( (!_srcClip && getContext() == OFX::eContextGenerator) ||
                ( _srcClip && (!_srcClip->isConnected() || _srcClip->getPixelComponents() ==  ePixelComponentAlpha ||
                               _srcClip->getPixelComponents() == ePixelComponentRGB ||
                               _srcClip->getPixelComponents() == ePixelComponentRGBA) ) );

        _rectangleInteractEnable = fetchBooleanParam(kParamRectangleInteractEnable);
        _extent = fetchChoiceParam(kParamGeneratorExtent);
        _format = fetchChoiceParam(kParamGeneratorFormat);
        _formatSize = fetchInt2DParam(kParamGeneratorSize);
        _formatPar = fetchDoubleParam(kParamGeneratorPAR);
        _btmLeft = fetchDouble2DParam(kParamRectangleInteractBtmLeft);
        _size = fetchDouble2DParam(kParamRectangleInteractSize);
        _recenter = fetchPushButtonParam(kParamGeneratorCenter);
        _interactive = fetchBooleanParam(kParamRectangleInteractInteractive);
        _softness = fetchDoubleParam(kParamSoftness);
        _reformat = fetchBooleanParam(kParamReformat);
        _intersect = fetchBooleanParam(kParamIntersect);
        _blackOutside = fetchBooleanParam(kParamBlackOutside);

        assert(_rectangleInteractEnable && _btmLeft && _size && _softness && _reformat && _intersect && _blackOutside);

        updateParamsVisibility();
    }

private:
    // override the roi call
    virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) OVERRIDE FINAL;
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    template <int nComponents>
    void renderInternal(const OFX::RenderArguments &args, OFX::BitDepthEnum dstBitDepth);

    /* set up and run a processor */
    void setupAndProcess(CropProcessorBase &, const OFX::RenderArguments &args);

    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    virtual void getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences) OVERRIDE FINAL;

    void getCropRectangle(OfxTime time,
                          const OfxPointD& renderScale,
                          bool forceIntersect,
                          bool useBlackOutside,
                          bool useReformat,
                          OfxRectD* cropRect,
                          double* par) const;

    void updateParamsVisibility();

    OFX::Clip* getSrcClip() const
    {
        return _srcClip;
    }

private:
    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;
    OFX::Clip *_srcClip;
    OFX::BooleanParam* _rectangleInteractEnable;
    OFX::ChoiceParam* _extent;
    OFX::ChoiceParam* _format;
    OFX::Int2DParam* _formatSize;
    OFX::DoubleParam* _formatPar;
    OFX::Double2DParam* _btmLeft;
    OFX::Double2DParam* _size;
    OFX::BooleanParam* _interactive;
    OFX::PushButtonParam *_recenter;
    OFX::DoubleParam* _softness;
    OFX::BooleanParam* _reformat;
    OFX::BooleanParam* _intersect;
    OFX::BooleanParam* _blackOutside;
};

void
CropPlugin::getCropRectangle(OfxTime time,
                             const OfxPointD& renderScale,
                             bool forceIntersect,
                             bool useBlackOutside,
                             bool useReformat,
                             OfxRectD* cropRect,
                             double* pixelAspectRatio) const
{
    bool intersect;

    if (!forceIntersect) {
        _intersect->getValueAtTime(time, intersect);
    } else {
        intersect = true;
    }

    bool blackOutside = false;
    if (useBlackOutside) {
        _blackOutside->getValueAtTime(time, blackOutside);
    }

    bool reformat = false;
    if (useReformat) {
        reformat = _reformat->getValueAtTime(time);
    }

    OfxRectD rod;
    double par;

    // below: see GeneratorPlugin::getRegionOfDefinition

    GeneratorExtentEnum extent = (GeneratorExtentEnum)_extent->getValue();

    switch (extent) {
        case eGeneratorExtentFormat: {
            OfxRectI pixelFormat;
            int w, h;
            _formatSize->getValue(w, h);
            _formatPar->getValue(par);
            pixelFormat.x1 = pixelFormat.y1 = 0;
            pixelFormat.x2 = w;
            pixelFormat.y2 = h;
            const OfxPointD rsOne = {1., 1.};
            OFX::Coords::toCanonical(pixelFormat, rsOne, par, &rod);
            break;
        }
        case eGeneratorExtentSize: {
            par = _srcClip->getPixelAspectRatio();
            _size->getValueAtTime(time, rod.x2, rod.y2);
            _btmLeft->getValue(rod.x1, rod.y1);
            rod.x2 += rod.x1;
            rod.y2 += rod.y1;
            break;
        }
        case eGeneratorExtentProject: {
            OfxPointD siz = getProjectSize();
            OfxPointD off = getProjectOffset();
            rod.x1 = off.x;
            rod.x2 = off.x + siz.x;
            rod.y1 = off.y;
            rod.y2 = off.y + siz.y;
            par = getProjectPixelAspectRatio();
            break;
       }
        case eGeneratorExtentDefault: {
            if (_srcClip->isConnected()) {
                rod = _srcClip->getRegionOfDefinition(time);
                par = _srcClip->getPixelAspectRatio();
           } else {
                OfxPointD siz = getProjectSize();
                OfxPointD off = getProjectOffset();
                rod.x1 = off.x;
                rod.x2 = off.x + siz.x;
                rod.y1 = off.y;
                rod.y2 = off.y + siz.y;
                par = getProjectPixelAspectRatio();
            }
            break;
        }
    }

    if (reformat) {
        rod.x2 -= rod.x1;
        rod.y2 -= rod.y1;
        rod.x1 = 0.;
        rod.y1 = 0.;
    }
    if (intersect && _srcClip) {
        const OfxRectD& srcRoD = _srcClip->getRegionOfDefinition(time);
        OFX::Coords::rectIntersection(rod, srcRoD, &rod);
    }

    if (blackOutside) {
        OfxRectI rodPixel;
        OFX::Coords::toPixelEnclosing(rod, renderScale, par, &rodPixel);
        rodPixel.x1 -= 1;
        rodPixel.y1 -= 1;
        rodPixel.x2 += 1;
        rodPixel.y2 += 1;
        OFX::Coords::toCanonical(rodPixel, renderScale, par, &rod);
    }

    if (cropRect) {
        *cropRect = rod;
    }
    if (pixelAspectRatio) {
        *pixelAspectRatio = par;
    }
}

////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

/* set up and run a processor */
void
CropPlugin::setupAndProcess(CropProcessorBase &processor,
                            const OFX::RenderArguments &args)
{
    const double time = args.time;
    std::auto_ptr<OFX::Image> dst( _dstClip->fetchImage(args.time) );

    if ( !dst.get() ) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    OFX::BitDepthEnum dstBitDepth    = dst->getPixelDepth();
    OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
    if ( ( dstBitDepth != _dstClip->getPixelDepth() ) ||
         ( dstComponents != _dstClip->getPixelComponents() ) ) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if ( (dst->getRenderScale().x != args.renderScale.x) ||
         ( dst->getRenderScale().y != args.renderScale.y) ||
         ( ( dst->getField() != OFX::eFieldNone) /* for DaVinci Resolve */ && ( dst->getField() != args.fieldToRender) ) ) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::auto_ptr<const OFX::Image> src( ( _srcClip && _srcClip->isConnected() ) ?
                                         _srcClip->fetchImage(args.time) : 0 );
    if ( src.get() ) {
        if ( (src->getRenderScale().x != args.renderScale.x) ||
             ( src->getRenderScale().y != args.renderScale.y) ||
             ( ( src->getField() != OFX::eFieldNone) /* for DaVinci Resolve */ && ( src->getField() != args.fieldToRender) ) ) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
        OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
        OFX::BitDepthEnum srcBitDepth      = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
        if ( (srcBitDepth != dstBitDepth) || (srcComponents != dstComponents) ) {
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }

    // set the images
    processor.setDstImg( dst.get() );
    processor.setSrcImg( src.get() );

    // set the render window
    processor.setRenderWindow(args.renderWindow);

    bool reformat = _reformat->getValueAtTime(args.time);
    bool blackOutside = _blackOutside->getValueAtTime(args.time);
    OfxRectD cropRectCanonical;
    double par;
    getCropRectangle(time, args.renderScale, /*forceIntersect=*/false, /*useBlackOutside=*/false, /*useReformat=*/false, &cropRectCanonical, &par);
    double softness = _softness->getValueAtTime(args.time);
    softness *= args.renderScale.x;

    processor.setValues(cropRectCanonical, args.renderScale, par, blackOutside, reformat, softness);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
} // CropPlugin::setupAndProcess

// override the roi call
// Required if the plugin requires a region from the inputs which is different from the rendered region of the output.
// (this is the case here)
void
CropPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                 OFX::RegionOfInterestSetter &rois)
{
    OfxRectD cropRect;
    getCropRectangle(args.time, args.renderScale, /*forceIntersect=*/true, /*useBlackOutside=*/false, /*useReformat=*/false, &cropRect, NULL);

    OfxRectD roi = args.regionOfInterest;

    bool reformat = _reformat->getValueAtTime(args.time);
    if (reformat) {
        // translate, because cropRect will be rendered at (0,0) in this case
        // Remember: this is the region of INTEREST: the region from the input
        // used to render the region args.regionOfInterest
        roi.x1 += cropRect.x1;
        roi.y1 += cropRect.y1;
        roi.x2 += cropRect.x2;
        roi.y2 += cropRect.y2;
    }

    // intersect the crop rectangle with args.regionOfInterest
    OFX::Coords::rectIntersection(cropRect, roi, &cropRect);
    rois.setRegionOfInterest(*_srcClip, cropRect);
}

bool
CropPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args,
                                  OfxRectD &rod)
{
    getCropRectangle(args.time, args.renderScale, /*forceIntersect=*/false, /*useBlackOutside=*/true, /*useReformat=*/true, &rod, NULL);

    return true;
}

// the internal render function
template <int nComponents>
void
CropPlugin::renderInternal(const OFX::RenderArguments &args,
                           OFX::BitDepthEnum dstBitDepth)
{
    switch (dstBitDepth) {
    case OFX::eBitDepthUByte: {
        CropProcessor<unsigned char, nComponents, 255> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case OFX::eBitDepthUShort: {
        CropProcessor<unsigned short, nComponents, 65535> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    case OFX::eBitDepthFloat: {
        CropProcessor<float, nComponents, 1> fred(*this);
        setupAndProcess(fred, args);
        break;
    }
    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// the overridden render function
void
CropPlugin::render(const OFX::RenderArguments &args)
{
    // instantiate the render code based on the pixel depth of the dst clip
    OFX::BitDepthEnum dstBitDepth    = _dstClip->getPixelDepth();
    OFX::PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();

    assert( kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio() );
    assert( kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth() );
    assert(dstComponents == OFX::ePixelComponentRGBA || dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentXY || dstComponents == OFX::ePixelComponentAlpha);
    if (dstComponents == OFX::ePixelComponentRGBA) {
        renderInternal<4>(args, dstBitDepth);
    } else if (dstComponents == OFX::ePixelComponentRGB) {
        renderInternal<3>(args, dstBitDepth);
    } else if (dstComponents == OFX::ePixelComponentXY) {
        renderInternal<2>(args, dstBitDepth);
    } else {
        assert(dstComponents == OFX::ePixelComponentAlpha);
        renderInternal<1>(args, dstBitDepth);
    }
}

void
CropPlugin::updateParamsVisibility()
{
    GeneratorExtentEnum extent = (GeneratorExtentEnum)_extent->getValue();
    bool hasFormat = (extent == eGeneratorExtentFormat);
    bool hasSize = (extent == eGeneratorExtentSize);

    _format->setIsSecretAndDisabled(!hasFormat);
    _size->setIsSecretAndDisabled(!hasSize);
    _recenter->setIsSecretAndDisabled(!hasSize);
    _btmLeft->setIsSecretAndDisabled(!hasSize);
    _interactive->setIsSecretAndDisabled(!hasSize);
}


void
CropPlugin::changedParam(const OFX::InstanceChangedArgs &args,
                         const std::string &paramName)
{
    const double time = args.time;

    if (paramName == kParamReformat) {
        bool reformat;
        _reformat->getValueAtTime(args.time, reformat);
        _rectangleInteractEnable->setValue(!reformat);

    // everything below is from GeneratorPlugin::changedParam()
    } else if ( (paramName == kParamGeneratorExtent) && (args.reason == OFX::eChangeUserEdit) ) {
        updateParamsVisibility();
    } else if (paramName == kParamGeneratorFormat) {
        //the host does not handle the format itself, do it ourselves
        OFX::EParamFormat format = (OFX::EParamFormat)_format->getValue();
        int w = 0, h = 0;
        double par = -1;
        getFormatResolution(format, &w, &h, &par);
        assert(par != -1);
        _formatPar->setValue(par);
        _formatSize->setValue(w, h);
    } else if (paramName == kParamGeneratorCenter) {
        OFX::Clip* srcClip = getSrcClip();
        OfxRectD srcRoD;
        if ( srcClip && srcClip->isConnected() ) {
            srcRoD = srcClip->getRegionOfDefinition(args.time);
        } else {
            OfxPointD siz = getProjectSize();
            OfxPointD off = getProjectOffset();
            srcRoD.x1 = off.x;
            srcRoD.x2 = off.x + siz.x;
            srcRoD.y1 = off.y;
            srcRoD.y2 = off.y + siz.y;
        }
        OfxPointD center;
        center.x = (srcRoD.x2 + srcRoD.x1) / 2.;
        center.y = (srcRoD.y2 + srcRoD.y1) / 2.;

        OfxRectD rectangle;
        _size->getValueAtTime(time, rectangle.x2, rectangle.y2);
        _btmLeft->getValueAtTime(time, rectangle.x1, rectangle.y1);
        rectangle.x2 += rectangle.x1;
        rectangle.y2 += rectangle.y1;

        OfxRectD newRectangle;
        newRectangle.x1 = center.x - (rectangle.x2 - rectangle.x1) / 2.;
        newRectangle.y1 = center.y - (rectangle.y2 - rectangle.y1) / 2.;
        newRectangle.x2 = newRectangle.x1 + (rectangle.x2 - rectangle.x1);
        newRectangle.y2 = newRectangle.y1 + (rectangle.y2 - rectangle.y1);

        _size->setValue(newRectangle.x2 - newRectangle.x1, newRectangle.y2 - newRectangle.y1);
        _btmLeft->setValue(newRectangle.x1, newRectangle.y1);
    }

}

void
CropPlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    if ( _reformat->getValue() ) {
        GeneratorExtentEnum extent = (GeneratorExtentEnum)_extent->getValue();

        if (extent == eGeneratorExtentFormat) {
            //specific output format
            double par = _formatPar->getValue();
            if (par != 0.) {
                clipPreferences.setPixelAspectRatio(*_dstClip, par);
            }
#ifdef OFX_EXTENSIONS_NATRON
            OfxRectI pixelFormat;
            int w, h;
            _formatSize->getValue(w, h);
            pixelFormat.x1 = pixelFormat.y1 = 0;
            pixelFormat.x2 = w;
            pixelFormat.y2 = h;
            if (!OFX::Coords::rectIsEmpty(pixelFormat)) {
                clipPreferences.setOutputFormat(pixelFormat);
            }
#endif
        }
        
    }
}


class CropInteract
    : public RectangleInteract
{
public:

    CropInteract(OfxInteractHandle handle,
                 OFX::ImageEffect* effect)
        : RectangleInteract(handle, effect)
        , _reformat(0)
        , _isReformated(false)
    {
        _reformat = effect->fetchBooleanParam(kParamReformat);
        addParamToSlaveTo(_reformat);
        assert(_reformat);
    }

private:

    virtual OfxPointD getBtmLeft(OfxTime time) const OVERRIDE FINAL
    {
        OfxPointD btmLeft;
        bool reformat;

        _reformat->getValueAtTime(time, reformat);
        if (!reformat) {
            btmLeft = RectangleInteract::getBtmLeft(time);
        } else {
            btmLeft.x = btmLeft.y = 0.;
        }

        return btmLeft;
    }

    virtual void aboutToCheckInteractivity(OfxTime time) OVERRIDE FINAL
    {
        updateReformated(time);
    }

    virtual bool allowTopLeftInteraction() const OVERRIDE FINAL { return !_isReformated; }

    virtual bool allowBtmRightInteraction() const OVERRIDE FINAL { return !_isReformated; }

    virtual bool allowBtmLeftInteraction() const OVERRIDE FINAL { return !_isReformated; }

    virtual bool allowBtmMidInteraction() const OVERRIDE FINAL { return !_isReformated; }

    virtual bool allowMidLeftInteraction() const OVERRIDE FINAL { return !_isReformated; }

    virtual bool allowCenterInteraction() const OVERRIDE FINAL { return !_isReformated; }

    void updateReformated(OfxTime time)
    {
        _reformat->getValueAtTime(time, _isReformated);
    }

private:
    OFX::BooleanParam* _reformat;
    bool _isReformated; //< @see aboutToCheckInteractivity
};

class CropOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<CropOverlayDescriptor, CropInteract>
{
};


mDeclarePluginFactory(CropPluginFactory, {}, {});

void
CropPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);

    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);


    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(true);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);

    desc.setSupportsTiles(kSupportsTiles);

    // in order to support multiresolution, render() must take into account the pixelaspectratio and the renderscale
    // and scale the transform appropriately.
    // All other functions are usually in canonical coordinates.
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setOverlayInteractDescriptor(new CropOverlayDescriptor);
#ifdef OFX_EXTENSIONS_NUKE
    // ask the host to render all planes
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelRenderAllRequestedPlanes);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

OFX::ImageEffect*
CropPluginFactory::createInstance(OfxImageEffectHandle handle,
                                  OFX::ContextEnum /*context*/)
{
    return new CropPlugin(handle);
}

void
CropPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc,
                                     OFX::ContextEnum context)
{
    // Source clip only in the filter context
    // create the mandated source clip
    // always declare the source clip first, because some hosts may consider
    // it as the default input clip (e.g. Nuke)
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);

    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentXY);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentXY);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(kSupportsTiles);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    // rectangleInteractEnable
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamRectangleInteractEnable);
        param->setIsSecretAndDisabled(true);
        param->setDefault(!kParamReformatDefault);
        if (page) {
            page->addChild(*param);
        }
    }
    generatorDescribeInContext(page, desc, /*unused*/ *dstClip, eGeneratorExtentSize, /*unused*/ ePixelComponentRGBA, /*useOutputComponentsAndDepth=*/ false, context);

    // softness
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamSoftness);
        param->setLabel(kParamSoftnessLabel);
        param->setDefault(0);
        param->setRange(0., 1000.);
        param->setDisplayRange(0., 100.);
        param->setIncrement(1.);
        param->setHint(kParamSoftnessHint);
        if (page) {
            page->addChild(*param);
        }
    }

    // reformat
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamReformat);
        param->setLabel(kParamReformatLabel);
        param->setHint( std::string(kParamReformatHint) + (getImageEffectHostDescription()->isNatron ? "" : kParamReformatHintExtraNatron) );
        param->setDefault(kParamReformatDefault);
        param->setAnimates(false);
        param->setLayoutHint(OFX::eLayoutHintNoNewLine, 1);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    // intersect
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamIntersect);
        param->setLabel(kParamIntersectLabel);
        param->setHint(kParamIntersectHint);
        param->setLayoutHint(OFX::eLayoutHintNoNewLine, 1);
        param->setDefault(false);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }

    // blackOutside
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamBlackOutside);
        param->setLabel(kParamBlackOutsideLabel);
        param->setDefault(false);
        param->setAnimates(true);
        param->setHint(kParamBlackOutsideHint);
        if (page) {
            page->addChild(*param);
        }
    }
} // CropPluginFactory::describeInContext

static CropPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
