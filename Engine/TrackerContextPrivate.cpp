/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2015 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */
// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "TrackerContextPrivate.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/KnobTypes.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/TLSHolder.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerFrameAccessor.h"
#include "Engine/TrackerNode.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER;

/**
 * @brief Creates a duplicate of the knob identified by knobName which is a knob in the internalNode onto the effect and add it to the given page.
 * If otherNode is set, also fetch a knob of the given name on the otherNode and link it to the newly created knob.
 **/

template <typename KNOBTYPE>
boost::shared_ptr<KNOBTYPE>
createDuplicateKnob( const std::string& knobName,
                     const NodePtr& internalNode,
                     const EffectInstPtr& effect,
                     const boost::shared_ptr<KnobPage>& page = boost::shared_ptr<KnobPage>(),
                     const boost::shared_ptr<KnobGroup>& group = boost::shared_ptr<KnobGroup>(),
                     const NodePtr& otherNode = NodePtr() )
{
    KnobPtr internalNodeKnob = internalNode->getKnobByName(knobName);
    if (!internalNodeKnob) {
        return boost::shared_ptr<KNOBTYPE>();
    }
    assert(internalNodeKnob);
    KnobPtr duplicateKnob = internalNodeKnob->createDuplicateOnNode(effect.get(), page, group, -1, true, internalNodeKnob->getName(), internalNodeKnob->getLabel(), internalNodeKnob->getHintToolTip(), false, false);

    if (otherNode) {
        KnobPtr otherNodeKnob = otherNode->getKnobByName(knobName);
        assert(otherNodeKnob);
        for (int i = 0; i < otherNodeKnob->getDimension(); ++i) {
            otherNodeKnob->slaveTo(i, duplicateKnob, i);
        }
    }

    return boost::dynamic_pointer_cast<KNOBTYPE>(duplicateKnob);
}

TrackerContextPrivate::TrackerContextPrivate(TrackerContext* publicInterface,
                                             const boost::shared_ptr<Node> &node)
    : _publicInterface(publicInterface)
    , node(node)
    , perTrackKnobs()
    , enableTrackRed()
    , enableTrackGreen()
    , enableTrackBlue()
    , maxError()
    , maxIterations()
    , bruteForcePreTrack()
    , useNormalizedIntensities()
    , preBlurSigma()
    , exportDataSep()
    , exportButton()
    , referenceFrame()
    , trackerContextMutex()
    , markers()
    , selectedMarkers()
    , markersToSlave()
    , markersToUnslave()
    , beginSelectionCounter(0)
    , selectionRecursion(0)
    , scheduler(_publicInterface, node, &TrackerContextPrivate::trackStepLibMV)
{
    EffectInstPtr effect = node->getEffectInstance();
    //needs to be blocking, otherwise the progressUpdate() call could be made before startProgress
    QObject::connect( &scheduler, SIGNAL(trackingStarted(int)), _publicInterface, SIGNAL(trackingStarted(int)), Qt::BlockingQueuedConnection );
    QObject::connect( &scheduler, SIGNAL(trackingFinished()), _publicInterface, SIGNAL(trackingFinished()) );
    boost::shared_ptr<TrackerNode> isTrackerNode = boost::dynamic_pointer_cast<TrackerNode>(effect);
    QString fixedNamePrefix = QString::fromUtf8( node->getScriptName_mt_safe().c_str() );

    fixedNamePrefix.append( QLatin1Char('_') );
    if (isTrackerNode) {
        NodePtr input, output;
        //NodePtr maskInput;

        {
            CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_OUTPUT), eCreateNodeReasonInternal, isTrackerNode);
            args.createGui = false;
            args.addToProject = false;
            output = node->getApp()->createNode(args);
            try {
                output->setScriptName("Output");
            } catch (...) {
            }

            assert(output);
        }
        {
            CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_INPUT), eCreateNodeReasonInternal, isTrackerNode);
            args.fixedName = QLatin1String("Source");
            args.createGui = false;
            args.addToProject = false;
            input = node->getApp()->createNode(args);
            assert(input);
        }

        /* {
             CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_INPUT), eCreateNodeReasonInternal, isTrackerNode);
             args.fixedName = QLatin1String("Mask");
             args.createGui = false;
             args.addToProject = false;
             maskInput = node->getApp()->createNode(args);
             assert(maskInput);
             KnobPtr isMaskInputKnob = maskInput->getKnobByName(kNatronGroupInputIsMaskParamName);
             if (isMaskInputKnob) {
                 KnobBool* maskInputKnob = dynamic_cast<KnobBool*>(isMaskInputKnob.get());
                 assert(maskInputKnob);
                 if (maskInputKnob) {
                     maskInputKnob->setValue(true);
                 }
             }
             KnobPtr isOptionalInputKnob = maskInput->getKnobByName(kNatronGroupInputIsOptionalParamName);
             if (isOptionalInputKnob) {
                 KnobBool* optionalInputKnob = dynamic_cast<KnobBool*>(isOptionalInputKnob.get());
                 assert(optionalInputKnob);
                 if (optionalInputKnob) {
                     optionalInputKnob->setValue(true);
                 }
             }

           }*/

        {
            QString cornerPinName = fixedNamePrefix + QLatin1String("CornerPin");
            CreateNodeArgs args(QString::fromUtf8(PLUGINID_OFX_CORNERPIN), eCreateNodeReasonInternal, isTrackerNode);
            args.fixedName = cornerPinName;
            args.createGui = false;
            args.addToProject = false;
            NodePtr cpNode = node->getApp()->createNode(args);
            if (!cpNode) {
                throw std::runtime_error( QObject::tr("The Tracker node requires the Misc.ofx.bundle plug-in to be installed").toStdString() );
            }
            cpNode->setNodeDisabled(true);
            cornerPinNode = cpNode;
        }

        {
            QString transformName = fixedNamePrefix + QLatin1String("Transform");
            CreateNodeArgs args(QString::fromUtf8(PLUGINID_OFX_TRANSFORM), eCreateNodeReasonInternal, isTrackerNode);
            args.fixedName = transformName;
            args.createGui = false;
            args.addToProject = false;
            NodePtr tNode = node->getApp()->createNode(args);
            tNode->setNodeDisabled(true);
            transformNode = tNode;

            output->connectInput(tNode, 0);
            NodePtr cpNode = cornerPinNode.lock();
            tNode->connectInput(cpNode, 0);
            cpNode->connectInput(input, 0);
        }
    }

    boost::shared_ptr<KnobPage> settingsPage = AppManager::createKnob<KnobPage>(effect.get(), "Tracking", 1, false);
    boost::shared_ptr<KnobPage> transformPage = AppManager::createKnob<KnobPage>(effect.get(), "Transform", 1, false);
    transformPageKnob = transformPage;

    boost::shared_ptr<KnobBool> enableTrackRedKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamTrackRedLabel, 1, false);
    enableTrackRedKnob->setName(kTrackerParamTrackRed);
    enableTrackRedKnob->setHintToolTip(kTrackerParamTrackRedHint);
    enableTrackRedKnob->setDefaultValue(true);
    enableTrackRedKnob->setAnimationEnabled(false);
    enableTrackRedKnob->setAddNewLine(false);
    enableTrackRedKnob->setEvaluateOnChange(false);
    settingsPage->addKnob(enableTrackRedKnob);
    enableTrackRed = enableTrackRedKnob;

    boost::shared_ptr<KnobBool> enableTrackGreenKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamTrackGreenLabel, 1, false);
    enableTrackGreenKnob->setName(kTrackerParamTrackGreen);
    enableTrackGreenKnob->setHintToolTip(kTrackerParamTrackGreenHint);
    enableTrackGreenKnob->setDefaultValue(true);
    enableTrackGreenKnob->setAnimationEnabled(false);
    enableTrackGreenKnob->setAddNewLine(false);
    enableTrackGreenKnob->setEvaluateOnChange(false);
    settingsPage->addKnob(enableTrackGreenKnob);
    enableTrackGreen = enableTrackGreenKnob;

    boost::shared_ptr<KnobBool> enableTrackBlueKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamTrackBlueLabel, 1, false);
    enableTrackBlueKnob->setName(kTrackerParamTrackBlue);
    enableTrackBlueKnob->setHintToolTip(kTrackerParamTrackBlueHint);
    enableTrackBlueKnob->setDefaultValue(true);
    enableTrackBlueKnob->setAnimationEnabled(false);
    enableTrackBlueKnob->setEvaluateOnChange(false);
    settingsPage->addKnob(enableTrackBlueKnob);
    enableTrackBlue = enableTrackBlueKnob;

    boost::shared_ptr<KnobDouble> maxErrorKnob = AppManager::createKnob<KnobDouble>(effect.get(), kTrackerParamMaxErrorLabel, 1, false);
    maxErrorKnob->setName(kTrackerParamMaxError);
    maxErrorKnob->setHintToolTip(kTrackerParamMaxErrorHint);
    maxErrorKnob->setAnimationEnabled(false);
    maxErrorKnob->setMinimum(0.);
    maxErrorKnob->setMaximum(1.);
    maxErrorKnob->setDefaultValue(0.2);
    maxErrorKnob->setEvaluateOnChange(false);
    settingsPage->addKnob(maxErrorKnob);
    maxError = maxErrorKnob;

    boost::shared_ptr<KnobInt> maxItKnob = AppManager::createKnob<KnobInt>(effect.get(), kTrackerParamMaximumIterationLabel, 1, false);
    maxItKnob->setName(kTrackerParamMaximumIteration);
    maxItKnob->setHintToolTip(kTrackerParamMaximumIterationHint);
    maxItKnob->setAnimationEnabled(false);
    maxItKnob->setMinimum(0);
    maxItKnob->setMaximum(150);
    maxItKnob->setEvaluateOnChange(false);
    maxItKnob->setDefaultValue(50);
    settingsPage->addKnob(maxItKnob);
    maxIterations = maxItKnob;

    boost::shared_ptr<KnobBool> usePretTrackBF = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamBruteForcePreTrackLabel, 1, false);
    usePretTrackBF->setName(kTrackerParamBruteForcePreTrack);
    usePretTrackBF->setHintToolTip(kTrackerParamBruteForcePreTrackHint);
    usePretTrackBF->setDefaultValue(true);
    usePretTrackBF->setAnimationEnabled(false);
    usePretTrackBF->setEvaluateOnChange(false);
    usePretTrackBF->setAddNewLine(false);
    settingsPage->addKnob(usePretTrackBF);
    bruteForcePreTrack = usePretTrackBF;

    boost::shared_ptr<KnobBool> useNormalizedInt = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamNormalizeIntensitiesLabel, 1, false);
    useNormalizedInt->setName(kTrackerParamNormalizeIntensities);
    useNormalizedInt->setHintToolTip(kTrackerParamNormalizeIntensitiesHint);
    useNormalizedInt->setDefaultValue(false);
    useNormalizedInt->setAnimationEnabled(false);
    useNormalizedInt->setEvaluateOnChange(false);
    settingsPage->addKnob(useNormalizedInt);
    useNormalizedIntensities = useNormalizedInt;

    boost::shared_ptr<KnobDouble> preBlurSigmaKnob = AppManager::createKnob<KnobDouble>(effect.get(), kTrackerParamPreBlurSigmaLabel, 1, false);
    preBlurSigmaKnob->setName(kTrackerParamPreBlurSigma);
    preBlurSigmaKnob->setHintToolTip(kTrackerParamPreBlurSigmaHint);
    preBlurSigmaKnob->setAnimationEnabled(false);
    preBlurSigmaKnob->setMinimum(0);
    preBlurSigmaKnob->setMaximum(10.);
    preBlurSigmaKnob->setDefaultValue(0.9);
    preBlurSigmaKnob->setEvaluateOnChange(false);
    settingsPage->addKnob(preBlurSigmaKnob);
    preBlurSigma = preBlurSigmaKnob;

    boost::shared_ptr<KnobBool> enableTrackKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamEnabledLabel, 1, false);
    enableTrackKnob->setName(kTrackerParamEnabled);
    enableTrackKnob->setHintToolTip(kTrackerParamEnabledHint);
    enableTrackKnob->setAnimationEnabled(true);
    enableTrackKnob->setDefaultValue(true);
    enableTrackKnob->setEvaluateOnChange(false);
    enableTrackKnob->setAllDimensionsEnabled(false);
    settingsPage->addKnob(enableTrackKnob);
    activateTrack = enableTrackKnob;
    perTrackKnobs.push_back(enableTrackKnob);


    boost::shared_ptr<KnobSeparator>  transformGenerationSeparatorKnob = AppManager::createKnob<KnobSeparator>(effect.get(), "Transform Generation", 3);
    transformPage->addKnob(transformGenerationSeparatorKnob);
    transformGenerationSeparator = transformGenerationSeparatorKnob;

    boost::shared_ptr<KnobChoice> motionTypeKnob = AppManager::createKnob<KnobChoice>(effect.get(), kTrackerParamMotionTypeLabel, 1);
    motionTypeKnob->setName(kTrackerParamMotionType);
    motionTypeKnob->setHintToolTip(kTrackerParamMotionTypeHint);
    {
        std::vector<std::string> choices, helps;
        choices.push_back(kTrackerParamMotionTypeNone);
        helps.push_back(kTrackerParamMotionTypeNoneHelp);
        choices.push_back(kTrackerParamMotionTypeStabilize);
        helps.push_back(kTrackerParamMotionTypeStabilizeHelp);
        choices.push_back(kTrackerParamMotionTypeMatchMove);
        helps.push_back(kTrackerParamMotionTypeMatchMoveHelp);
        choices.push_back(kTrackerParamMotionTypeRemoveJitter);
        helps.push_back(kTrackerParamMotionTypeRemoveJitterHelp);
        choices.push_back(kTrackerParamMotionTypeAddJitter);
        helps.push_back(kTrackerParamMotionTypeAddJitterHelp);

        motionTypeKnob->populateChoices(choices, helps);
    }
    motionTypeKnob->setAddNewLine(false);
    motionType = motionTypeKnob;
    transformPage->addKnob(motionTypeKnob);

    boost::shared_ptr<KnobChoice> transformTypeKnob = AppManager::createKnob<KnobChoice>(effect.get(), kTrackerParamTransformTypeLabel, 1);
    transformTypeKnob->setName(kTrackerParamTransformType);
    transformTypeKnob->setHintToolTip(kTrackerParamTransformTypeHint);
    {
        std::vector<std::string> choices, helps;
        choices.push_back(kTrackerParamTransformTypeTransform);
        helps.push_back(kTrackerParamTransformTypeTransformHelp);
        choices.push_back(kTrackerParamTransformTypeCornerPin);
        helps.push_back(kTrackerParamTransformTypeCornerPinHelp);

        transformTypeKnob->populateChoices(choices, helps);
    }
    transformType = transformTypeKnob;
    transformPage->addKnob(transformTypeKnob);

    boost::shared_ptr<KnobInt> referenceFrameKnob = AppManager::createKnob<KnobInt>(effect.get(), kTrackerParamReferenceFrameLabel, 1);
    referenceFrameKnob->setName(kTrackerParamReferenceFrame);
    referenceFrameKnob->setHintToolTip(kTrackerParamReferenceFrameHint);
    referenceFrameKnob->setAnimationEnabled(false);
    referenceFrameKnob->setDefaultValue(0);
    referenceFrameKnob->setAddNewLine(false);
    referenceFrameKnob->setEvaluateOnChange(false);
    transformPage->addKnob(referenceFrameKnob);
    referenceFrame = referenceFrameKnob;

    boost::shared_ptr<KnobButton> setCurrentFrameKnob = AppManager::createKnob<KnobButton>(effect.get(), kTrackerParamSetReferenceFrameLabel, 1);
    setCurrentFrameKnob->setName(kTrackerParamSetReferenceFrame);
    setCurrentFrameKnob->setHintToolTip(kTrackerParamSetReferenceFrameHint);
    transformPage->addKnob(setCurrentFrameKnob);
    setCurrentFrameButton = setCurrentFrameKnob;

    boost::shared_ptr<KnobInt>  jitterPeriodKnob = AppManager::createKnob<KnobInt>(effect.get(), kTrackerParamJitterPeriodLabel, 1);
    jitterPeriodKnob->setName(kTrackerParamJitterPeriod);
    jitterPeriodKnob->setHintToolTip(kTrackerParamJitterPeriodHint);
    jitterPeriodKnob->setAnimationEnabled(false);
    jitterPeriodKnob->setDefaultValue(10);
    jitterPeriodKnob->setMinimum(0, 0);
    jitterPeriodKnob->setEvaluateOnChange(false);
    transformPage->addKnob(jitterPeriodKnob);
    jitterPeriod = jitterPeriodKnob;

    boost::shared_ptr<KnobInt>  smoothTransformKnob = AppManager::createKnob<KnobInt>(effect.get(), kTrackerParamSmoothLabel, 3);
    smoothTransformKnob->setName(kTrackerParamSmooth);
    smoothTransformKnob->setHintToolTip(kTrackerParamSmoothHint);
    smoothTransformKnob->setAnimationEnabled(false);
    smoothTransformKnob->disableSlider();
    smoothTransformKnob->setDimensionName(0, "t");
    smoothTransformKnob->setDimensionName(1, "r");
    smoothTransformKnob->setDimensionName(2, "s");
    for (int i = 0; i < 3; ++i) {
        smoothTransformKnob->setMinimum(0, i);
    }
    smoothTransformKnob->setEvaluateOnChange(false);
    transformPage->addKnob(smoothTransformKnob);
    smoothTransform = smoothTransformKnob;

    boost::shared_ptr<KnobInt>  smoothCornerPinKnob = AppManager::createKnob<KnobInt>(effect.get(), kTrackerParamSmoothCornerPinLabel, 1);
    smoothCornerPinKnob->setName(kTrackerParamSmoothCornerPin);
    smoothCornerPinKnob->setHintToolTip(kTrackerParamSmoothCornerPinHint);
    smoothCornerPinKnob->setAnimationEnabled(false);
    smoothCornerPinKnob->disableSlider();
    smoothCornerPinKnob->setMinimum(0, 0);
    smoothCornerPinKnob->setEvaluateOnChange(false);
    smoothCornerPinKnob->setSecret(true);
    transformPage->addKnob(smoothCornerPinKnob);
    smoothCornerPin = smoothCornerPinKnob;

    boost::shared_ptr<KnobSeparator>  transformSeparator = AppManager::createKnob<KnobSeparator>(effect.get(), "Transform Controls", 3);
    transformPage->addKnob(transformSeparator);
    transformControlsSeparator = transformSeparator;

    NodePtr tNode = transformNode.lock();
    if (tNode) {
        translate = createDuplicateKnob<KnobDouble>(kTransformParamTranslate, tNode, effect, transformPage);
        rotate = createDuplicateKnob<KnobDouble>(kTransformParamRotate, tNode, effect, transformPage);
        scale = createDuplicateKnob<KnobDouble>(kTransformParamScale, tNode, effect, transformPage);
        scale.lock()->setAddNewLine(false);
        scaleUniform = createDuplicateKnob<KnobBool>(kTransformParamUniform, tNode, effect, transformPage);
        skewX = createDuplicateKnob<KnobDouble>(kTransformParamSkewX, tNode, effect, transformPage);
        skewY = createDuplicateKnob<KnobDouble>(kTransformParamSkewY, tNode, effect, transformPage);
        skewOrder = createDuplicateKnob<KnobChoice>(kTransformParamSkewOrder, tNode, effect, transformPage);
        center = createDuplicateKnob<KnobDouble>(kTransformParamCenter, tNode, effect, transformPage);
    } // tNode
    NodePtr cNode = cornerPinNode.lock();
    if (cNode) {
        boost::shared_ptr<KnobGroup>  toGroupKnob = AppManager::createKnob<KnobGroup>(effect.get(), kCornerPinParamTo, 1);
        toGroupKnob->setName(kCornerPinParamTo);
        toGroupKnob->setAsTab();
        toGroupKnob->setSecret(true);
        transformPage->addKnob(toGroupKnob);
        toGroup = toGroupKnob;

        boost::shared_ptr<KnobGroup>  fromGroupKnob = AppManager::createKnob<KnobGroup>(effect.get(), kCornerPinParamFrom, 1);
        fromGroupKnob->setName(kCornerPinParamFrom);
        fromGroupKnob->setAsTab();
        fromGroupKnob->setSecret(true);
        transformPage->addKnob(fromGroupKnob);
        fromGroup = fromGroupKnob;

        const char* fromPointNames[4] = {kCornerPinParamFrom1, kCornerPinParamFrom2, kCornerPinParamFrom3, kCornerPinParamFrom4};
        const char* toPointNames[4] = {kCornerPinParamTo1, kCornerPinParamTo2, kCornerPinParamTo3, kCornerPinParamTo4};
        const char* enablePointNames[4] = {kCornerPinParamEnable1, kCornerPinParamEnable2, kCornerPinParamEnable3, kCornerPinParamEnable4};

        for (int i = 0; i < 4; ++i) {
            fromPoints[i] = createDuplicateKnob<KnobDouble>(fromPointNames[i], cNode, effect, transformPage, fromGroupKnob);

            toPoints[i] = createDuplicateKnob<KnobDouble>(toPointNames[i], cNode, effect, transformPage, toGroupKnob);
            toPoints[i].lock()->setAddNewLine(false);
            enableToPoint[i] = createDuplicateKnob<KnobBool>(enablePointNames[i], cNode, effect, transformPage, toGroupKnob);
        }

        cornerPinOverlayPoints = createDuplicateKnob<KnobChoice>(kCornerPinParamOverlayPoints, cNode, effect, transformPage);
        cornerPinOverlayPoints.lock()->setSecret(true);

        boost::shared_ptr<KnobDouble> matrix = createDuplicateKnob<KnobDouble>(kCornerPinParamMatrix, cNode, effect, transformPage);
        if (matrix) {
            cornerPinMatrix = matrix;
            matrix->setSecret(true);
        }
    } // cNode

    // Add filtering & motion blur knobs
    if (tNode) {
        invertTransform = createDuplicateKnob<KnobBool>(kTransformParamInvert, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        filter = createDuplicateKnob<KnobChoice>(kTransformParamFilter, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        filter.lock()->setAddNewLine(false);
        clamp = createDuplicateKnob<KnobBool>(kTransformParamClamp, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        clamp.lock()->setAddNewLine(false);
        blackOutside = createDuplicateKnob<KnobBool>(kTransformParamBlackOutside, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        motionBlur = createDuplicateKnob<KnobDouble>(kTransformParamMotionBlur, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        shutter = createDuplicateKnob<KnobDouble>(kTransformParamShutter, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        shutterOffset = createDuplicateKnob<KnobChoice>(kTransformParamShutterOffset, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);
        customShutterOffset = createDuplicateKnob<KnobDouble>(kTransformParamCustomShutterOffset, tNode, effect, transformPage, boost::shared_ptr<KnobGroup>(), cNode);

        node->addTransformInteract(translate.lock(), scale.lock(), scaleUniform.lock(), rotate.lock(), skewX.lock(), skewY.lock(), skewOrder.lock(), center.lock(), invertTransform.lock(), boost::shared_ptr<KnobBool>() /*interactive*/);

        node->addCornerPinInteract(fromPoints[0].lock(), fromPoints[1].lock(), fromPoints[2].lock(), fromPoints[3].lock(),
                                   toPoints[0].lock(), toPoints[1].lock(), toPoints[2].lock(), toPoints[3].lock(),
                                   enableToPoint[0].lock(), enableToPoint[1].lock(), enableToPoint[2].lock(), enableToPoint[3].lock(), cornerPinOverlayPoints.lock(), invertTransform.lock(), boost::shared_ptr<KnobBool>() /*interactive*/);
    }

    boost::shared_ptr<KnobSeparator> exportDataSepKnob = AppManager::createKnob<KnobSeparator>(effect.get(), kTrackerParamExportDataSeparatorLabel, 1, false);
    exportDataSepKnob->setName(kTrackerParamExportDataSeparator);
    transformPage->addKnob(exportDataSepKnob);
    exportDataSep = exportDataSepKnob;

    boost::shared_ptr<KnobBool> exporLinkKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamExportLinkLabel, 1, false);
    exporLinkKnob->setName(kTrackerParamExportLink);
    exporLinkKnob->setHintToolTip(kTrackerParamExportLinkHint);
    exporLinkKnob->setAnimationEnabled(false);
    exporLinkKnob->setAddNewLine(false);
    exporLinkKnob->setDefaultValue(true);
    transformPage->addKnob(exporLinkKnob);
    exportLink = exporLinkKnob;

    /* boost::shared_ptr<KnobBool> exporUseCurFrameKnob = AppManager::createKnob<KnobBool>(effect.get(), kTrackerParamExportUseCurrentFrameLabel, 1, false);
       exporUseCurFrameKnob->setName(kTrackerParamExportUseCurrentFrame);
       exporUseCurFrameKnob->setHintToolTip(kTrackerParamExportUseCurrentFrameHint);
       exporUseCurFrameKnob->setAnimationEnabled(false);
       exporUseCurFrameKnob->setAddNewLine(false);
       exporUseCurFrameKnob->setDefaultValue(false);
       transformPage->addKnob(exporUseCurFrameKnob);
       exportUseCurrentFrame = exporUseCurFrameKnob;
       knobs.push_back(exporUseCurFrameKnob);*/

    boost::shared_ptr<KnobButton> exportButtonKnob = AppManager::createKnob<KnobButton>(effect.get(), kTrackerParamExportButtonLabel, 1);
    exportButtonKnob->setName(kTrackerParamExportButton);
    exportButtonKnob->setHintToolTip(kTrackerParamExportButtonHint);
    transformPage->addKnob(exportButtonKnob);
    exportButton = exportButtonKnob;
}

void
TrackArgsLibMV::getEnabledChannels(bool* r,
                                   bool* g,
                                   bool* b) const
{
    _fa->getEnabledChannels(r, g, b);
}

void
TrackArgsLibMV::getRedrawAreasNeeded(int time,
                                     std::list<RectD>* canonicalRects) const
{
    for (std::vector<boost::shared_ptr<TrackMarkerAndOptions> >::const_iterator it = _tracks.begin(); it != _tracks.end(); ++it) {
        boost::shared_ptr<KnobDouble> searchBtmLeft = (*it)->natronMarker->getSearchWindowBottomLeftKnob();
        boost::shared_ptr<KnobDouble> searchTopRight = (*it)->natronMarker->getSearchWindowTopRightKnob();
        boost::shared_ptr<KnobDouble> centerKnob = (*it)->natronMarker->getCenterKnob();
        boost::shared_ptr<KnobDouble> offsetKnob = (*it)->natronMarker->getOffsetKnob();
        Point offset, center, btmLeft, topRight;
        offset.x = offsetKnob->getValueAtTime(time, 0);
        offset.y = offsetKnob->getValueAtTime(time, 1);

        center.x = centerKnob->getValueAtTime(time, 0);
        center.y = centerKnob->getValueAtTime(time, 1);

        btmLeft.x = searchBtmLeft->getValueAtTime(time, 0) + center.x + offset.x;
        btmLeft.y = searchBtmLeft->getValueAtTime(time, 1) + center.y + offset.y;

        topRight.x = searchTopRight->getValueAtTime(time, 0) + center.x + offset.x;
        topRight.y = searchTopRight->getValueAtTime(time, 1) + center.y + offset.y;

        RectD rect;
        rect.x1 = btmLeft.x;
        rect.y1 = btmLeft.y;
        rect.x2 = topRight.x;
        rect.y2 = topRight.y;
        canonicalRects->push_back(rect);
    }
}

/**
 * @brief Set keyframes on knobs from Marker data
 **/
void
TrackerContextPrivate::setKnobKeyframesFromMarker(const mv::Marker& mvMarker,
                                                  int formatHeight,
                                                  const libmv::TrackRegionResult* result,
                                                  const TrackMarkerPtr& natronMarker)
{
    int time = mvMarker.frame;
    boost::shared_ptr<KnobDouble> errorKnob = natronMarker->getErrorKnob();

    if (result) {
        double corr = result->correlation;
        if (corr != corr) {
            corr = 1.;
        }
        errorKnob->setValueAtTime(time, 1. - corr, ViewSpec::current(), 0);
    } else {
        errorKnob->setValueAtTime(time, 0., ViewSpec::current(), 0);
    }

    Point center;
    center.x = (double)mvMarker.center(0);
    center.y = (double)TrackerFrameAccessor::invertYCoordinate(mvMarker.center(1), formatHeight);

    boost::shared_ptr<KnobDouble> offsetKnob = natronMarker->getOffsetKnob();
    Point offset;
    offset.x = offsetKnob->getValueAtTime(time, 0);
    offset.y = offsetKnob->getValueAtTime(time, 1);

    // Set the center
    boost::shared_ptr<KnobDouble> centerKnob = natronMarker->getCenterKnob();
    centerKnob->setValuesAtTime(time, center.x, center.y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);

    Point topLeftCorner, topRightCorner, btmRightCorner, btmLeftCorner;
    topLeftCorner.x = mvMarker.patch.coordinates(0, 0) - offset.x - center.x;
    topLeftCorner.y = TrackerFrameAccessor::invertYCoordinate(mvMarker.patch.coordinates(0, 1), formatHeight) - offset.y - center.y;

    topRightCorner.x = mvMarker.patch.coordinates(1, 0) - offset.x - center.x;
    topRightCorner.y = TrackerFrameAccessor::invertYCoordinate(mvMarker.patch.coordinates(1, 1), formatHeight) - offset.y - center.y;

    btmRightCorner.x = mvMarker.patch.coordinates(2, 0) - offset.x - center.x;
    btmRightCorner.y = TrackerFrameAccessor::invertYCoordinate(mvMarker.patch.coordinates(2, 1), formatHeight) - offset.y - center.y;

    btmLeftCorner.x = mvMarker.patch.coordinates(3, 0) - offset.x - center.x;
    btmLeftCorner.y = TrackerFrameAccessor::invertYCoordinate(mvMarker.patch.coordinates(3, 1), formatHeight) - offset.y - center.y;

    boost::shared_ptr<KnobDouble> pntTopLeftKnob = natronMarker->getPatternTopLeftKnob();
    boost::shared_ptr<KnobDouble> pntTopRightKnob = natronMarker->getPatternTopRightKnob();
    boost::shared_ptr<KnobDouble> pntBtmLeftKnob = natronMarker->getPatternBtmLeftKnob();
    boost::shared_ptr<KnobDouble> pntBtmRightKnob = natronMarker->getPatternBtmRightKnob();

    // Set the pattern Quad
    pntTopLeftKnob->setValuesAtTime(time, topLeftCorner.x, topLeftCorner.y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    pntTopRightKnob->setValuesAtTime(time, topRightCorner.x, topRightCorner.y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    pntBtmLeftKnob->setValuesAtTime(time, btmLeftCorner.x, btmLeftCorner.y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    pntBtmRightKnob->setValuesAtTime(time, btmRightCorner.x, btmRightCorner.y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
} // TrackerContextPrivate::setKnobKeyframesFromMarker

/// Converts a Natron track marker to the one used in LibMV. This is expensive: many calls to getValue are made
void
TrackerContextPrivate::natronTrackerToLibMVTracker(bool isReferenceMarker,
                                                   bool trackChannels[3],
                                                   const TrackMarker& marker,
                                                   int trackIndex,
                                                   int trackedTime,
                                                   int frameStep,
                                                   double formatHeight,
                                                   mv::Marker* mvMarker)
{
    boost::shared_ptr<KnobDouble> searchWindowBtmLeftKnob = marker.getSearchWindowBottomLeftKnob();
    boost::shared_ptr<KnobDouble> searchWindowTopRightKnob = marker.getSearchWindowTopRightKnob();
    boost::shared_ptr<KnobDouble> patternTopLeftKnob = marker.getPatternTopLeftKnob();
    boost::shared_ptr<KnobDouble> patternTopRightKnob = marker.getPatternTopRightKnob();
    boost::shared_ptr<KnobDouble> patternBtmRightKnob = marker.getPatternBtmRightKnob();
    boost::shared_ptr<KnobDouble> patternBtmLeftKnob = marker.getPatternBtmLeftKnob();

#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
    boost::shared_ptr<KnobDouble> weightKnob = marker.getWeightKnob();
#endif
    boost::shared_ptr<KnobDouble> centerKnob = marker.getCenterKnob();
    boost::shared_ptr<KnobDouble> offsetKnob = marker.getOffsetKnob();

    // We don't use the clip in Natron
    mvMarker->clip = 0;
    mvMarker->reference_clip = 0;

#ifdef NATRON_TRACK_MARKER_USE_WEIGHT
    mvMarker->weight = (float)weightKnob->getValue();
#else
    mvMarker->weight = 1.;
#endif

    mvMarker->frame = trackedTime;
    const int referenceTime = marker.getReferenceFrame(trackedTime, frameStep);
    mvMarker->reference_frame = referenceTime;
    if (marker.isUserKeyframe(trackedTime)) {
        mvMarker->source = mv::Marker::MANUAL;
    } else {
        mvMarker->source = mv::Marker::TRACKED;
    }
    
    // This doesn't seem to be used at all by libmv TrackRegion
    mvMarker->model_type = mv::Marker::POINT;
    mvMarker->model_id = 0;
    mvMarker->track = trackIndex;

    mvMarker->disabled_channels =
        (trackChannels[0] ? LIBMV_MARKER_CHANNEL_R : 0) |
        (trackChannels[1] ? LIBMV_MARKER_CHANNEL_G : 0) |
        (trackChannels[2] ? LIBMV_MARKER_CHANNEL_B : 0);


    //int searchWinTime = isReferenceMarker ? trackedTime : mvMarker->reference_frame;

    // The patch is a quad (4 points); generally in 2D or 3D (here 2D)
    //
    //    +----------> x
    //    |\.
    //    | \.
    //    |  z (z goes into screen)
    //    |
    //    |     r0----->r1
    //    |      ^       |
    //    |      |   .   |
    //    |      |       V
    //    |     r3<-----r2
    //    |              \.
    //    |               \.
    //    v                normal goes away (right handed).
    //    y
    //
    // Each row is one of the corners coordinates; either (x, y) or (x, y, z).
    // TrackMarker extracts the patch coordinates as such:
    /*
     Quad2Df offset_quad = marker.patch;
     Vec2f origin = marker.search_region.Rounded().min;
     offset_quad.coordinates.rowwise() -= origin.transpose();
     QuadToArrays(offset_quad, x, y);
     x[4] = marker.center.x() - origin(0);
     y[4] = marker.center.y() - origin(1);
     */
    // The patch coordinates should be canonical

    
    Point tl, tr, br, bl;
    tl.x = patternTopLeftKnob->getValueAtTime(trackedTime, 0);
    tl.y = patternTopLeftKnob->getValueAtTime(trackedTime, 1);

    tr.x = patternTopRightKnob->getValueAtTime(trackedTime, 0);
    tr.y = patternTopRightKnob->getValueAtTime(trackedTime, 1);

    br.x = patternBtmRightKnob->getValueAtTime(trackedTime, 0);
    br.y = patternBtmRightKnob->getValueAtTime(trackedTime, 1);

    bl.x = patternBtmLeftKnob->getValueAtTime(trackedTime, 0);
    bl.y = patternBtmLeftKnob->getValueAtTime(trackedTime, 1);

    /*RectD patternBbox;
       patternBbox.setupInfinity();
       updateBbox(tl, &patternBbox);
       updateBbox(tr, &patternBbox);
       updateBbox(br, &patternBbox);
       updateBbox(bl, &patternBbox);*/

    // The search-region is laid out as such:
    //
    //    +----------> x
    //    |
    //    |   (min.x, min.y)           (max.x, min.y)
    //    |        +-------------------------+
    //    |        |                         |
    //    |        |                         |
    //    |        |                         |
    //    |        +-------------------------+
    //    v   (min.x, max.y)           (max.x, max.y)
    //
    
    int searchWinTime = isReferenceMarker ? trackedTime : referenceTime;
    
    Point searchWndBtmLeft, searchWndTopRight;
    searchWndBtmLeft.x = searchWindowBtmLeftKnob->getValueAtTime(searchWinTime, 0);
    searchWndBtmLeft.y = searchWindowBtmLeftKnob->getValueAtTime(searchWinTime, 1);
    
    searchWndTopRight.x = searchWindowTopRightKnob->getValueAtTime(searchWinTime, 0);
    searchWndTopRight.y = searchWindowTopRightKnob->getValueAtTime(searchWinTime, 1);
    
    /*
     Center and offset are always sampled at the reference time
     */
    Point centerAtTrackedTime;
    centerAtTrackedTime.x = centerKnob->getValueAtTime(trackedTime, 0);
    centerAtTrackedTime.y = centerKnob->getValueAtTime(trackedTime, 1);
    
    Point offsetAtTrackedTime;
    offsetAtTrackedTime.x = offsetKnob->getValueAtTime(trackedTime, 0);
    offsetAtTrackedTime.y = offsetKnob->getValueAtTime(trackedTime, 1);
    
    mvMarker->center(0) = centerAtTrackedTime.x;
    mvMarker->center(1) = TrackerFrameAccessor::invertYCoordinate(centerAtTrackedTime.y, formatHeight);
    
    Point centerPlusOffset;
    centerPlusOffset.x = centerAtTrackedTime.x + offsetAtTrackedTime.x;
    centerPlusOffset.y = centerAtTrackedTime.y + offsetAtTrackedTime.y;
    
    searchWndBtmLeft.x += centerPlusOffset.x;
    searchWndBtmLeft.y += centerPlusOffset.y;
    
    searchWndTopRight.x += centerPlusOffset.x;
    searchWndTopRight.y += centerPlusOffset.y;
    
    tl.x += centerPlusOffset.x;
    tl.y += centerPlusOffset.y;
    
    tr.x += centerPlusOffset.x;
    tr.y += centerPlusOffset.y;
    
    br.x += centerPlusOffset.x;
    br.y += centerPlusOffset.y;
    
    bl.x += centerPlusOffset.x;
    bl.y += centerPlusOffset.y;
    
    mvMarker->search_region.min(0) = searchWndBtmLeft.x;
    mvMarker->search_region.min(1) = TrackerFrameAccessor::invertYCoordinate(searchWndTopRight.y, formatHeight);
    mvMarker->search_region.max(0) = searchWndTopRight.x ;
    mvMarker->search_region.max(1) = TrackerFrameAccessor::invertYCoordinate(searchWndBtmLeft.y, formatHeight);


    
    mvMarker->patch.coordinates(0, 0) = tl.x;
    mvMarker->patch.coordinates(0, 1) = TrackerFrameAccessor::invertYCoordinate(tl.y, formatHeight);

    mvMarker->patch.coordinates(1, 0) = tr.x;
    mvMarker->patch.coordinates(1, 1) = TrackerFrameAccessor::invertYCoordinate(tr.y, formatHeight);

    mvMarker->patch.coordinates(2, 0) = br.x;
    mvMarker->patch.coordinates(2, 1) = TrackerFrameAccessor::invertYCoordinate(br.y, formatHeight);

    mvMarker->patch.coordinates(3, 0) = bl.x;
    mvMarker->patch.coordinates(3, 1) = TrackerFrameAccessor::invertYCoordinate(bl.y, formatHeight);
} // TrackerContextPrivate::natronTrackerToLibMVTracker

void
TrackerContextPrivate::beginLibMVOptionsForTrack(mv::TrackRegionOptions* options) const
{
    options->minimum_correlation = 1. - maxError.lock()->getValue();
    assert(options->minimum_correlation >= 0. && options->minimum_correlation <= 1.);
    options->max_iterations = maxIterations.lock()->getValue();
    options->use_brute_initialization = bruteForcePreTrack.lock()->getValue();
    options->use_normalized_intensities = useNormalizedIntensities.lock()->getValue();
    options->sigma = preBlurSigma.lock()->getValue();
}

void
TrackerContextPrivate::endLibMVOptionsForTrack(const TrackMarker& marker,
                                               mv::TrackRegionOptions* options) const
{
    int mode_i = marker.getMotionModelKnob()->getValue();

    switch (mode_i) {
    case 0:
        options->mode = mv::TrackRegionOptions::TRANSLATION;
        break;
    case 1:
        options->mode = mv::TrackRegionOptions::TRANSLATION_ROTATION;
        break;
    case 2:
        options->mode = mv::TrackRegionOptions::TRANSLATION_SCALE;
        break;
    case 3:
        options->mode = mv::TrackRegionOptions::TRANSLATION_ROTATION_SCALE;
        break;
    case 4:
        options->mode = mv::TrackRegionOptions::AFFINE;
        break;
    case 5:
        options->mode = mv::TrackRegionOptions::HOMOGRAPHY;
        break;
    default:
        options->mode = mv::TrackRegionOptions::AFFINE;
        break;
    }
}

/*
 * @brief This is the internal tracking function that makes use of LivMV to do 1 track step
 * @param trackingIndex This is the index of the Marker we should track in the args
 * @param args Multiple arguments global to the whole track, not just this step
 * @param trackTime The search frame time, that is, the frame to track
 */
bool
TrackerContextPrivate::trackStepLibMV(int trackIndex,
                                      const TrackArgsLibMV& args,
                                      int trackTime)
{
    assert( trackIndex >= 0 && trackIndex < args.getNumTracks() );

    const std::vector<boost::shared_ptr<TrackMarkerAndOptions> >& tracks = args.getTracks();
    const boost::shared_ptr<TrackMarkerAndOptions>& track = tracks[trackIndex];
    boost::shared_ptr<mv::AutoTrack> autoTrack = args.getLibMVAutoTrack();
    QMutex* autoTrackMutex = args.getAutoTrackMutex();
    bool enabledChans[3];
    args.getEnabledChannels(&enabledChans[0], &enabledChans[1], &enabledChans[2]);


    {
        //Add the tracked marker
        QMutexLocker k(autoTrackMutex);
        natronTrackerToLibMVTracker(false, enabledChans, *track->natronMarker, trackIndex, trackTime, args.getStep(), args.getFormatHeight(), &track->mvMarker);
        autoTrack->AddMarker(track->mvMarker);
    }


    //The frame on the mvMarker should have been set accordingly
    assert(track->mvMarker.frame == trackTime);

    if (track->mvMarker.source == mv::Marker::MANUAL) {
        // This is a user keyframe or the first frame, we do not track it
        assert(trackTime == args.getStart() || track->natronMarker->isUserKeyframe(track->mvMarker.frame));
#ifdef TRACE_LIB_MV
        qDebug() << QThread::currentThread() << "TrackStep:" << time << "is a keyframe";
#endif
        setKnobKeyframesFromMarker(track->mvMarker, args.getFormatHeight(), 0, track->natronMarker);
    } else {
        // Set the reference rame
        

        // Make sure the reference frame has the same search window as the tracked frame and exists in the AutoTrack
        {
            QMutexLocker k(autoTrackMutex);
            mv::Marker m;
            natronTrackerToLibMVTracker(true, enabledChans, *track->natronMarker, track->mvMarker.track, track->mvMarker.reference_frame, args.getStep(), args.getFormatHeight(), &m);
            autoTrack->AddMarker(m);
        }
        
        
        assert(track->mvMarker.reference_frame != track->mvMarker.frame);



#ifdef TRACE_LIB_MV
        qDebug() << QThread::currentThread() << ">>>> Tracking marker" << trackIndex << "at frame" << trackTime <<
            "with reference frame" << track->mvMarker.reference_frame;
#endif

        // Do the actual tracking
        libmv::TrackRegionResult result;
        if ( !autoTrack->TrackMarker(&track->mvMarker, &result, &track->mvOptions) || !result.is_usable() ) {
#ifdef TRACE_LIB_MV
            qDebug() << QThread::currentThread() << "Tracking FAILED (" << (int)result.termination <<  ") for track" << trackIndex << "at frame" << trackTime;
#endif

            // Todo: report error to user
            return false;
        }

        //Ok the tracking has succeeded, now the marker is in this state:
        //the source is TRACKED, the search_window has been offset by the center delta,  the center has been offset

#ifdef TRACE_LIB_MV
        qDebug() << QThread::currentThread() << "Tracking SUCCESS for track" << trackIndex << "at frame" << trackTime;
#endif

        //Extract the marker to the knob keyframes
        setKnobKeyframesFromMarker(track->mvMarker, args.getFormatHeight(), &result, track->natronMarker);

        //Add the marker to the autotrack
        /*{
           QMutexLocker k(autoTrackMutex);
           autoTrack->AddMarker(track->mvMarker);
           }*/
    } // if (track->mvMarker.source == mv::Marker::MANUAL) {

    appPTR->getAppTLS()->cleanupTLSForThread();

    return true;
} // TrackerContextPrivate::trackStepLibMV

void
TrackerContext::trackMarkers(const std::list<TrackMarkerPtr >& markers,
                             int start,
                             int end,
                             int frameStep,
                             ViewerInstance* viewer)
{
    if ( markers.empty() ) {
        return;
    }

    /// The channels we are going to use for tracking
    bool enabledChannels[3];
    enabledChannels[0] = _imp->enableTrackRed.lock()->getValue();
    enabledChannels[1] = _imp->enableTrackGreen.lock()->getValue();
    enabledChannels[2] = _imp->enableTrackBlue.lock()->getValue();

    double formatWidth, formatHeight;
    Format f;
    getNode()->getApp()->getProject()->getProjectDefaultFormat(&f);
    formatWidth = f.width();
    formatHeight = f.height();

    /// The accessor and its cache is local to a track operation, it is wiped once the whole sequence track is finished.
    boost::shared_ptr<TrackerFrameAccessor> accessor( new TrackerFrameAccessor(this, enabledChannels, formatHeight) );
    boost::shared_ptr<mv::AutoTrack> trackContext( new mv::AutoTrack( accessor.get() ) );
    std::vector<boost::shared_ptr<TrackMarkerAndOptions> > trackAndOptions;
    mv::TrackRegionOptions mvOptions;
    /*
       Get the global parameters for the LivMV track: pre-blur sigma, No iterations, normalized intensities, etc...
     */
    _imp->beginLibMVOptionsForTrack(&mvOptions);

    /*
       For the given markers, do the following:
       - Get the "User" keyframes that have been set and create a LibMV marker for each keyframe as well as for the "start" time
       - Set the "per-track" parameters that were given by the user, that is the mv::TrackRegionOptions
       - t->mvMarker will contain the marker that evolves throughout the tracking
     */
    int trackIndex = 0;
    for (std::list<TrackMarkerPtr >::const_iterator it = markers.begin(); it != markers.end(); ++it, ++trackIndex) {
        boost::shared_ptr<TrackMarkerAndOptions> t(new TrackMarkerAndOptions);
        t->natronMarker = *it;

        std::set<int> userKeys;
        t->natronMarker->getUserKeyframes(&userKeys);

        // Add a libmv marker for all keyframes
        for (std::set<int>::iterator it2 = userKeys.begin(); it2 != userKeys.end(); ++it2) {
            if (*it2 == start) {
                //Will be added in the track step
                continue;
            } else {
                mv::Marker marker;
                TrackerContextPrivate::natronTrackerToLibMVTracker(true, enabledChannels, *t->natronMarker, trackIndex, *it2, frameStep, formatHeight, &marker);
                assert(marker.source == mv::Marker::MANUAL);
                trackContext->AddMarker(marker);
            }
        }


        //For all already tracked frames which are not keyframes, add them to the AutoTrack too
        std::set<double> centerKeys;
        t->natronMarker->getCenterKeyframes(&centerKeys);
        for (std::set<double>::iterator it2 = centerKeys.begin(); it2 != centerKeys.end(); ++it2) {
            if ( userKeys.find(*it2) != userKeys.end() ) {
                continue;
            }
            if (*it2 == start) {
                //Will be added in the track step
                continue;
            } else {
                mv::Marker marker;
                TrackerContextPrivate::natronTrackerToLibMVTracker(true, enabledChannels, *t->natronMarker, trackIndex, *it2, frameStep, formatHeight, &marker);
                assert(marker.source == mv::Marker::TRACKED);
                trackContext->AddMarker(marker);
            }
        }


        t->mvOptions = mvOptions;
        _imp->endLibMVOptionsForTrack(*t->natronMarker, &t->mvOptions);
        trackAndOptions.push_back(t);
    }


    /*
       Launch tracking in the scheduler thread.
     */
    TrackArgsLibMV args(start, end, frameStep, getNode()->getApp()->getTimeLine(), viewer, trackContext, accessor, trackAndOptions, formatWidth, formatHeight);
    _imp->scheduler.track(args);
} // TrackerContext::trackMarkers

void
TrackerContextPrivate::linkMarkerKnobsToGuiKnobs(const std::list<TrackMarkerPtr >& markers,
                                                 bool multipleTrackSelected,
                                                 bool slave)
{
    std::list<TrackMarkerPtr >::const_iterator next = markers.begin();

    if ( !markers.empty() ) {
        ++next;
    }
    for (std::list<TrackMarkerPtr >::const_iterator it = markers.begin(); it != markers.end(); ++it) {
        const KnobsVec& trackKnobs = (*it)->getKnobs();
        for (KnobsVec::const_iterator it2 = trackKnobs.begin(); it2 != trackKnobs.end(); ++it2) {
            // Find the knob in the TrackerContext knobs
            boost::shared_ptr<KnobI> found;
            for (std::list<boost::weak_ptr<KnobI> >::iterator it3 = perTrackKnobs.begin(); it3 != perTrackKnobs.end(); ++it3) {
                boost::shared_ptr<KnobI> k = it3->lock();
                if ( k->getName() == (*it2)->getName() ) {
                    found = k;
                    break;
                }
            }

            if (!found) {
                continue;
            }

            //Clone current state only for the last marker
            if ( next == markers.end() ) {
                found->cloneAndUpdateGui( it2->get() );
            }

            //Slave internal knobs
            assert( (*it2)->getDimension() == found->getDimension() );
            for (int i = 0; i < (*it2)->getDimension(); ++i) {
                if (slave) {
                    (*it2)->slaveTo(i, found, i);
                } else {
                    (*it2)->unSlave(i, !multipleTrackSelected);
                }
            }

            if (!slave) {
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(double,ViewSpec,int,int,bool)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(double,ViewSpec,int,int)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(ViewSpec,int,double,double)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(ViewSpec,int)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(double,ViewSpec,int)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::disconnect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(double,ViewSpec,int)),
                                     _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
            } else {
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(double,ViewSpec,int,int,bool)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(double,ViewSpec,int,int)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(ViewSpec,int,double,double)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(ViewSpec,int)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(double,ViewSpec,int)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
                QObject::connect( (*it2)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(double,ViewSpec,int)),
                                  _publicInterface, SLOT(onSelectedKnobCurveChanged()) );
            }
        }
        if ( next != markers.end() ) {
            ++next;
        }
    } // for (std::list<TrackMarkerPtr >::const_iterator it = markers() ; it!=markers(); ++it)

} // TrackerContextPrivate::linkMarkerKnobsToGuiKnobs

void
TrackerContextPrivate::refreshVisibilityFromTransformTypeInternal(TrackerTransformNodeEnum transformType)
{
    if ( !transformNode.lock() ) {
        return;
    }

    boost::shared_ptr<KnobChoice> motionTypeKnob = motionType.lock();
    if (!motionTypeKnob) {
        return;
    }
    int motionType_i = motionTypeKnob->getValue();
    TrackerMotionTypeEnum motionType = (TrackerMotionTypeEnum)motionType_i;

    transformNode.lock()->setNodeDisabled(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    cornerPinNode.lock()->setNodeDisabled(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);

    transformControlsSeparator.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    if (transformType == eTrackerTransformNodeTransform) {
        transformControlsSeparator.lock()->setLabel("Transform Controls");
    } else if (transformType == eTrackerTransformNodeCornerPin) {
        transformControlsSeparator.lock()->setLabel("CornerPin Controls");
    }
    

    smoothTransform.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    smoothCornerPin.lock()->setSecret(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);

    toGroup.lock()->setSecret(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);
    fromGroup.lock()->setSecret(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);
    cornerPinOverlayPoints.lock()->setSecret(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);
    boost::shared_ptr<KnobDouble> matrix = cornerPinMatrix.lock();
    if (matrix) {
        matrix->setSecret(transformType == eTrackerTransformNodeTransform || motionType == eTrackerMotionTypeNone);
    }

    translate.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    scale.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    scaleUniform.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    rotate.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    center.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    skewX.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    skewY.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    skewOrder.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    filter.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    clamp.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    blackOutside.lock()->setSecret(transformType == eTrackerTransformNodeCornerPin || motionType == eTrackerMotionTypeNone);
    
    invertTransform.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    motionBlur.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    shutter.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    shutterOffset.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    customShutterOffset.lock()->setSecret(motionType == eTrackerMotionTypeNone);
    
    exportLink.lock()->setEnabled(0, motionType != eTrackerMotionTypeNone);
    exportButton.lock()->setEnabled(0, motionType != eTrackerMotionTypeNone);
}

void
TrackerContextPrivate::refreshVisibilityFromTransformType()
{
    boost::shared_ptr<KnobChoice> transformTypeKnob = transformType.lock();

    assert(transformTypeKnob);
    int transformType_i = transformTypeKnob->getValue();
    TrackerTransformNodeEnum transformType = (TrackerTransformNodeEnum)transformType_i;
    refreshVisibilityFromTransformTypeInternal(transformType);
}


RectD
TrackerContextPrivate::getInputRoDAtTime(double time) const
{
    NodePtr thisNode = node.lock();
    NodePtr input = thisNode->getInput(0);
    bool useProjFormat = false;
    RectD ret;
    
    if (!input) {
        useProjFormat = true;
    } else {
        StatusEnum stat = input->getEffectInstance()->getRegionOfDefinition_public(input->getHashValue(), time, RenderScale(1.), ViewIdx(0), &ret, 0);
        if (stat == eStatusFailed) {
            useProjFormat = true;
        } else {
            return ret;
        }
    }
    if (useProjFormat) {
        Format f;
        thisNode->getApp()->getProject()->getProjectDefaultFormat(&f);
        ret.x1 = f.x1;
        ret.x2 = f.x2;
        ret.y1 = f.y1;
        ret.y2 = f.y2;
    }
    
    return ret;
}

using namespace openMVG::robust;
static void
throwProsacError(ProsacReturnCodeEnum c,
                 int nMinSamples)
{
    switch (c) {
        case openMVG::robust::eProsacReturnCodeFoundModel:
            break;
        case openMVG::robust::eProsacReturnCodeInliersIsMinSamples:
            break;
        case openMVG::robust::eProsacReturnCodeNoModelFound:
            throw std::runtime_error("Could not find a model for the given correspondences.");
            break;
        case openMVG::robust::eProsacReturnCodeNotEnoughPoints: {
            std::stringstream ss;
            ss << "This model requires a minimum of ";
            ss << nMinSamples;
            ss << " correspondences.";
            throw std::runtime_error( ss.str() );
        }
            break;
        case openMVG::robust::eProsacReturnCodeMaxIterationsFromProportionParamReached:
            throw std::runtime_error("Maximum iterations computed from outliers proportion reached");
            break;
        case openMVG::robust::eProsacReturnCodeMaxIterationsParamReached:
            throw std::runtime_error("Maximum solver iterations reached");
            break;
    }
}

template <typename MODELTYPE>
void
runProsacForModel(const std::vector<Point>& x1,
                  const std::vector<Point>& x2,
                  int w1,
                  int h1,
                  int w2,
                  int h2,
                  typename MODELTYPE::Model* foundModel)
{
    typedef ProsacKernelAdaptor<MODELTYPE> KernelType;
    
    assert( x1.size() == x2.size() );
    openMVG::Mat M1( 2, x1.size() ), M2( 2, x2.size() );
    for (std::size_t i = 0; i < x1.size(); ++i) {
        M1(0, i) = x1[i].x;
        M1(1, i) = x1[i].y;
        
        M2(0, i) = x2[i].x;
        M2(1, i) = x2[i].y;
    }
    
    KernelType kernel(M1, w1, h1, M2, w2, h2);
    ProsacReturnCodeEnum ret = prosac(kernel, foundModel);
    throwProsacError( ret, KernelType::MinimumSamples() );
}

void
TrackerContextPrivate::computeTranslationFromNPoints(const std::vector<Point>& x1,
                                              const std::vector<Point>& x2,
                                              int w1,
                                              int h1,
                                              int w2,
                                              int h2,
                                              Point* translation)
{
    openMVG::Vec2 model;
    
    runProsacForModel<openMVG::robust::Translation2DSolver>(x1, x2, w1, h1, w2, h2, &model);
    translation->x = model(0);
    translation->y = model(1);
}

void
TrackerContextPrivate::computeSimilarityFromNPoints(const std::vector<Point>& x1,
                                             const std::vector<Point>& x2,
                                             int w1,
                                             int h1,
                                             int w2,
                                             int h2,
                                             Point* translation,
                                             double* rotate,
                                             double* scale)
{
    openMVG::Vec4 model;
    
    runProsacForModel<openMVG::robust::Similarity2DSolver>(x1, x2, w1, h1, w2, h2, &model);
    openMVG::robust::Similarity2DSolver::rtsFromVec4(model, &translation->x, &translation->y, scale, rotate);
    *rotate = Transform::toDegrees(*rotate);
}

void
TrackerContextPrivate::computeHomographyFromNPoints(const std::vector<Point>& x1,
                                             const std::vector<Point>& x2,
                                             int w1,
                                             int h1,
                                             int w2,
                                             int h2,
                                             Transform::Matrix3x3* homog)
{
    openMVG::Mat3 model;
    
    runProsacForModel<openMVG::robust::Homography2DSolver>(x1, x2, w1, h1, w2, h2, &model);
    
    *homog = Transform::Matrix3x3( model(0, 0), model(0, 1), model(0, 2),
                                  model(1, 0), model(1, 1), model(1, 2),
                                  model(2, 0), model(2, 1), model(2, 2) );
}

void
TrackerContextPrivate::computeFundamentalFromNPoints(const std::vector<Point>& x1,
                                              const std::vector<Point>& x2,
                                              int w1,
                                              int h1,
                                              int w2,
                                              int h2,
                                              Transform::Matrix3x3* fundamental)
{
    openMVG::Mat3 model;
    
    runProsacForModel<openMVG::robust::FundamentalSolver>(x1, x2, w1, h1, w2, h2, &model);
    
    *fundamental = Transform::Matrix3x3( model(0, 0), model(0, 1), model(0, 2),
                                        model(1, 0), model(1, 1), model(1, 2),
                                        model(2, 0), model(2, 1), model(2, 2) );
}

struct PointWithError
{
    Point p1, p2;
    double error;
};

static bool
PointWithErrorCompareLess(const PointWithError& lhs,
                          const PointWithError& rhs)
{
    return lhs.error < rhs.error;
}

void
TrackerContextPrivate::extractSortedPointsFromMarkers(double refTime,
                                               double time,
                                               const std::vector<TrackMarkerPtr>& markers,
                                               int jitterPeriod,
                                               bool jitterAdd,
                                               std::vector<Point>* x1,
                                               std::vector<Point>* x2)
{
    assert( !markers.empty() );
    
    std::vector<PointWithError> pointsWithErrors;
    bool useJitter = (jitterPeriod > 1);
    int halfJitter = std::max(0, jitterPeriod / 2);
    // Prosac expects the points to be sorted by decreasing correlation score (increasing error)
    
    int pIndex = 0;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        boost::shared_ptr<KnobDouble> centerKnob = markers[i]->getCenterKnob();
        boost::shared_ptr<KnobDouble> errorKnob = markers[i]->getErrorKnob();
        
        if (centerKnob->getKeyFrameIndex(ViewSpec::current(), 0, time) < 0) {
            continue;
        }
        pointsWithErrors.resize(pointsWithErrors.size() + 1);
        
        PointWithError& perr = pointsWithErrors[pIndex];
        
        if (!useJitter) {
            perr.p1.x = centerKnob->getValueAtTime(refTime, 0);
            perr.p1.y = centerKnob->getValueAtTime(refTime, 1);
            perr.p2.x = centerKnob->getValueAtTime(time, 0);
            perr.p2.y = centerKnob->getValueAtTime(time, 1);
        } else {
            // Average halfJitter frames before and after refTime and time together to smooth the center
            std::vector<Point> x1PointJitter, x2PointJitter;
            for (double t = refTime - halfJitter; t <= refTime + halfJitter; t += 1.) {
                Point p;
                p.x = centerKnob->getValueAtTime(t, 0);
                p.y = centerKnob->getValueAtTime(t, 1);
                x1PointJitter.push_back(p);
            }
            for (double t = time - halfJitter; t <= time + halfJitter; t += 1.) {
                Point p;
                p.x = centerKnob->getValueAtTime(t, 0);
                p.y = centerKnob->getValueAtTime(t, 1);
                x2PointJitter.push_back(p);
            }
            assert( x1PointJitter.size() == x2PointJitter.size() );
            Point x1AtTime = x1PointJitter[(x1PointJitter.size() - 1) / 2];
            Point x2AtTime = x2PointJitter[(x2PointJitter.size() - 1) / 2];
            Point x1avg = {0, 0}, x2avg = {0, 0};
            for (std::size_t i = 0; i < x1PointJitter.size(); ++i) {
                x1avg.x += x1PointJitter[i].x;
                x1avg.y += x1PointJitter[i].y;
                x2avg.x += x2PointJitter[i].x;
                x2avg.y += x2PointJitter[i].y;
            }
            if ( !x1PointJitter.empty() ) {
                x1avg.x /= x1PointJitter.size();
                x1avg.y /= x1PointJitter.size();
                
                x2avg.x /= x1PointJitter.size();
                x2avg.y /= x1PointJitter.size();
            }
            if (!jitterAdd) {
                perr.p1.x = x1avg.x;
                perr.p1.y = x1avg.y;
                perr.p2.x = x2avg.x;
                perr.p2.y = x2avg.y;
            } else {
                Point highFreqX1, highFreqX2;
                highFreqX1.x = x1AtTime.x - x1avg.x;
                highFreqX1.y = x1AtTime.y - x1avg.y;
                
                highFreqX2.x = x2AtTime.x - x2avg.x;
                highFreqX2.y = x2AtTime.y - x2avg.y;
                
                perr.p1.x = x1AtTime.x + highFreqX1.x;
                perr.p1.y = x1AtTime.y + highFreqX1.y;
                perr.p2.x = x2AtTime.x + highFreqX2.x;
                perr.p2.y = x2AtTime.y + highFreqX2.y;
            }
        }
        
        perr.error = errorKnob->getValueAtTime(time, 0);
        ++pIndex;
    }
    
    std::sort(pointsWithErrors.begin(), pointsWithErrors.end(), PointWithErrorCompareLess);
    
    x1->resize( pointsWithErrors.size() );
    x2->resize( pointsWithErrors.size() );
    int r = 0;
    for (int i = (int)pointsWithErrors.size() - 1; i >= 0; --i, ++r) {
        (*x1)[r] = pointsWithErrors[i].p1;
        (*x2)[r] = pointsWithErrors[i].p2;
    }
} // TrackerContext::extractSortedPointsFromMarkers

TrackerContextPrivate::TransformData
TrackerContextPrivate::computeTransformParamsFromTracksAtTime(double refTime,
                                                       double time,
                                                       int jitterPeriod,
                                                       bool jitterAdd,
                                                       const std::vector<TrackMarkerPtr>& allMarkers)
{
    std::vector<TrackMarkerPtr> markers;
    for (std::size_t i = 0; i < allMarkers.size(); ++i) {
        if ( allMarkers[i]->isEnabled(time) ) {
            markers.push_back(allMarkers[i]);
        }
    }
    TrackerContextPrivate::TransformData data;
    data.time = time;
    data.valid = true;
    assert( !markers.empty() );
    std::vector<Point> x1, x2;
    extractSortedPointsFromMarkers(refTime, time, markers, jitterPeriod, jitterAdd, &x1, &x2);
    assert( x1.size() == x2.size() );
    if ( x1.empty() ) {
        data.valid = false;
        return data;
    }
    if (refTime == time) {
        data.hasRotationAndScale = x1.size() > 1;
        data.translation.x = data.translation.y = data.rotation = 0;
        data.scale = 1.;
        
        return data;
    }
    
    RectD rodRef = getInputRoDAtTime(refTime);
    RectD rodTime = getInputRoDAtTime(time);
    int w1 = rodRef.width();
    int h1 = rodRef.height();
    int w2 = rodTime.width();
    int h2 = rodTime.height();
    
    try {
        if (x1.size() == 1) {
            data.hasRotationAndScale = false;
            computeTranslationFromNPoints(x1, x2, w1, h1, w2, h2, &data.translation);
        } else {
            data.hasRotationAndScale = true;
            computeSimilarityFromNPoints(x1, x2, w1, h1, w2, h2, &data.translation, &data.rotation, &data.scale);
        }
    } catch (...) {
        data.valid = false;
    }
    return data;
}

static Transform::Point3D
euclideanToHomogenous(const Point& p)
{
    Transform::Point3D r;
    
    r.x = p.x;
    r.y = p.y;
    r.z = 1;
    
    return r;
}

TrackerContextPrivate::CornerPinData
TrackerContextPrivate::computeCornerPinParamsFromTracksAtTime(double refTime,
                                                       double time,
                                                       int jitterPeriod,
                                                       bool jitterAdd,
                                                       const std::vector<TrackMarkerPtr>& allMarkers)
{
    std::vector<TrackMarkerPtr> markers;
    for (std::size_t i = 0; i < allMarkers.size(); ++i) {
        if ( allMarkers[i]->isEnabled(time) ) {
            markers.push_back(allMarkers[i]);
        }
    }
    TrackerContextPrivate::CornerPinData data;
    data.time = time;
    data.valid = true;
    assert( !markers.empty() );
    std::vector<Point> x1, x2;
    extractSortedPointsFromMarkers(refTime, time, markers, jitterPeriod, jitterAdd, &x1, &x2);
    assert( x1.size() == x2.size() );
    if (x1.empty()) {
        data.valid = false;
        return data;
    }
    if (refTime == time) {
        data.h.setIdentity();
        data.nbEnabledPoints = 4;
        
        return data;
    }
    
    RectD rodRef = getInputRoDAtTime(refTime);
    RectD rodTime = getInputRoDAtTime(time);
    int w1 = rodRef.width();
    int h1 = rodRef.height();
    int w2 = rodTime.width();
    int h2 = rodTime.height();
    
    if (x1.size() == 1) {
        data.h.setTranslationFromOnePoint( euclideanToHomogenous(x1[0]), euclideanToHomogenous(x2[0]) );
        data.nbEnabledPoints = 1;
    } else if (x1.size() == 2) {
        data.h.setSimilarityFromTwoPoints( euclideanToHomogenous(x1[0]), euclideanToHomogenous(x1[1]), euclideanToHomogenous(x2[0]), euclideanToHomogenous(x2[1]) );
        data.nbEnabledPoints = 2;
    } else if (x1.size() == 3) {
        data.h.setAffineFromThreePoints( euclideanToHomogenous(x1[0]), euclideanToHomogenous(x1[1]), euclideanToHomogenous(x1[2]), euclideanToHomogenous(x2[0]), euclideanToHomogenous(x2[1]), euclideanToHomogenous(x2[2]) );
        data.nbEnabledPoints = 3;
    } else {
        try {
            computeHomographyFromNPoints(x1, x2, w1, h1, w2, h2, &data.h);
            data.nbEnabledPoints = 4;
        } catch (...) {
            data.valid = false;
        }
    }
    return data;
}

static Point
applyHomography(const Point& p,
                const Transform::Matrix3x3& h)
{
    Transform::Point3D a = euclideanToHomogenous(p);
    
    Transform::Point3D r = Transform::matApply(h, a);
    Point ret;
    ret.x = r.x / r.z;
    ret.y = r.y / r.z;
    
    return ret;
}

void
TrackerContextPrivate::computeCornerParamsFromTracksEnd(double refTime,
                                                 const QList<CornerPinData>& results)
{
    
    QList<CornerPinData> validResults;
    for (QList<CornerPinData>::const_iterator it = results.begin(); it!=results.end(); ++it) {
        if (it->valid) {
            validResults.push_back(*it);
        }
    }
    
    boost::shared_ptr<KnobInt> smoothCornerPinKnob = smoothCornerPin.lock();
    int smoothJitter = smoothCornerPinKnob->getValue();
    
    
    RectD rodRef = getInputRoDAtTime(refTime);
    boost::shared_ptr<KnobDouble> fromPointsKnob[4];
    boost::shared_ptr<KnobDouble> toPointsKnob[4];
    boost::shared_ptr<KnobBool> enabledPointsKnob[4];
    for (int i = 0; i < 4; ++i) {
        fromPointsKnob[i] = fromPoints[i].lock();
        toPointsKnob[i] = toPoints[i].lock();
        enabledPointsKnob[i] = enableToPoint[i].lock();
    }    

    std::list<KnobPtr> animatedKnobsChanged;
    
    
    for (int i = 0; i < 4; ++i) {
        toPointsKnob[i]->blockValueChanges();
        animatedKnobsChanged.push_back(toPointsKnob[i]);
        
    }
    
    resetTransformParamsAnimation();
    
    
    Point refFrom[4];
    refFrom[0].x = rodRef.x1;
    refFrom[0].y = rodRef.y1;
    
    refFrom[1].x = rodRef.x2;
    refFrom[1].y = rodRef.y1;
    
    refFrom[2].x = rodRef.x2;
    refFrom[2].y = rodRef.y2;
    
    refFrom[3].x = rodRef.x1;
    refFrom[3].y = rodRef.y2;
    
    for (int c = 0; c < 4; ++c) {
        fromPointsKnob[c]->setValues(refFrom[c].x, refFrom[c].y, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    }
    
    // Create temporary curves and clone the toPoint internal curves at once because setValueAtTime will be slow since it emits
    // signals to create keyframes in keyframeSet
    Curve tmpToPointsCurveX[4],tmpToPointsCurveY[4];
    
    for (QList<CornerPinData>::const_iterator itResults = validResults.begin(); itResults != validResults.end(); ++itResults) {
        
        const CornerPinData& dataAtTime = *itResults;
        
        if (smoothJitter > 1) {
            int halfJitter = smoothJitter / 2;
            Point avgTos[4] = {
                {0, 0}, {0, 0}, {0, 0}, {0, 0}
            };
            
            // Get halfJitter samples before the given time
            QList<CornerPinData>::const_iterator prevHalfIt = itResults;
            int nSamplesBefore = 0;
            const CornerPinData* lastCornerPinData = 0;
            while (prevHalfIt != validResults.begin() && nSamplesBefore <= halfJitter) {
                Point to[4];
                for (int c = 0; c < 4; ++c) {
                    to[c] = applyHomography(refFrom[c], prevHalfIt->h);
                    avgTos[c].x += to[c].x;
                    avgTos[c].y += to[c].y;
                }
                if (!lastCornerPinData) {
                    lastCornerPinData = &(*prevHalfIt);
                }
                ++nSamplesBefore;
                --prevHalfIt;
            }
            
            while (nSamplesBefore <= halfJitter && lastCornerPinData) {
                assert(prevHalfIt == validResults.begin());
                Point to[4];
                for (int c = 0; c < 4; ++c) {
                    to[c] = applyHomography(refFrom[c], lastCornerPinData->h);
                    avgTos[c].x += to[c].x;
                    avgTos[c].y += to[c].y;
                }
                ++nSamplesBefore;
            }
            
            // Get halfJitter samples after the given time
            QList<CornerPinData>::const_iterator nextHalfIt = itResults;
            int nSamplesAfter = 0;
            ++nextHalfIt;
            lastCornerPinData = 0;
            while (nextHalfIt != validResults.end() && nSamplesAfter < halfJitter) {
                Point to[4];
                for (int c = 0; c < 4; ++c) {
                    to[c] = applyHomography(refFrom[c], nextHalfIt->h);
                    avgTos[c].x += to[c].x;
                    avgTos[c].y += to[c].y;
                }
                if (!lastCornerPinData) {
                    lastCornerPinData = &(*nextHalfIt);
                }
                ++nSamplesAfter;
                ++nextHalfIt;
            }
            
            while (nSamplesAfter < halfJitter && lastCornerPinData) {
                assert(nextHalfIt == validResults.end());
                Point to[4];
                for (int c = 0; c < 4; ++c) {
                    to[c] = applyHomography(refFrom[c], lastCornerPinData->h);
                    avgTos[c].x += to[c].x;
                    avgTos[c].y += to[c].y;
                }
                ++nSamplesAfter;
            }
            
            const int nSamples = nSamplesAfter + nSamplesBefore;
            
            
            if (nSamples > 0) {
                for (int c = 0; c < 4; ++c) {
                    avgTos[c].x /= nSamples;
                    avgTos[c].y /= nSamples;
                }
                
                for (int c = 0; c < 4; ++c) {
                    KeyFrame kx(dataAtTime.time,avgTos[c].x);
                    KeyFrame ky(dataAtTime.time,avgTos[c].y);
                    tmpToPointsCurveX[c].addKeyFrame(kx);
                    tmpToPointsCurveY[c].addKeyFrame(ky);
                }
                
            }
        } else {
            for (int c = 0; c < 4; ++c) {
                Point toPoint;
                toPoint = applyHomography(refFrom[c], dataAtTime.h);
                KeyFrame kx(dataAtTime.time,toPoint.x);
                KeyFrame ky(dataAtTime.time,toPoint.y);
                tmpToPointsCurveX[c].addKeyFrame(kx);
                tmpToPointsCurveY[c].addKeyFrame(ky);
                //toPoints[c]->setValuesAtTime(dataAtTime[i].time, toPoint.x, toPoint.y, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
            }

        }
    } // for (std::size_t i = 0; i < dataAtTime.size(); ++i)
    
    for (int c = 0; c < 4; ++c) {
        toPointsKnob[c]->cloneCurve(ViewSpec::all(), 0, tmpToPointsCurveX[c]);
        toPointsKnob[c]->cloneCurve(ViewSpec::all(), 1, tmpToPointsCurveY[c]);
    }
    for (std::list<KnobPtr>::iterator it = animatedKnobsChanged.begin(); it != animatedKnobsChanged.end(); ++it) {
        (*it)->unblockValueChanges();
        int nDims = (*it)->getDimension();
        for (int i = 0; i < nDims; ++i) {
            (*it)->evaluateValueChange(i, refTime, ViewIdx(0), eValueChangedReasonNatronInternalEdited);
        }
    }
    
    endSolve();
}

void
TrackerContextPrivate::computeCornerParamsFromTracks()
{
    
#ifndef TRACKER_GENERATE_DATA_SEQUENTIALLY
    lastSolveRequest.tWatcher.reset();
    lastSolveRequest.cpWatcher.reset(new QFutureWatcher<TrackerContextPrivate::CornerPinData>());
    QObject::connect(lastSolveRequest.cpWatcher.get(), SIGNAL(finished()), this, SLOT(onCornerPinSolverWatcherFinished()));
    QObject::connect(lastSolveRequest.cpWatcher.get(), SIGNAL(progressValueChanged(int)), this, SLOT(onCornerPinSolverWatcherProgress(int)));
    lastSolveRequest.cpWatcher->setFuture(QtConcurrent::mapped(lastSolveRequest.keyframes, boost::bind(&TrackerContextPrivate::computeCornerPinParamsFromTracksAtTime, this, lastSolveRequest.refTime, _1, lastSolveRequest.jitterPeriod, lastSolveRequest.jitterAdd, lastSolveRequest.allMarkers)));
#else
    NodePtr thisNode = node.lock();
    QList<CornerPinData> validResults;
    {
        int nKeys = (int)keyframes.size();
        int keyIndex = 0;
        for (std::set<double>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it, ++keyIndex) {
            CornerPinData data = computeCornerPinParamsFromTracksAtTime(refTime, *it, jitterPeriod, jitterAdd, allMarkers);
            if (data.valid) {
                validResults.push_back(data);
            }
            double progress = (keyIndex + 1) / (double)nKeys;
            thisNode->getApp()->progressUpdate(node, progress);
        }
    }
    computeCornerParamsFromTracksEnd(refTime, validResults);
#endif
    
} // TrackerContext::computeCornerParamsFromTracks

void
TrackerContextPrivate::resetTransformParamsAnimation()
{
    {
        // Revert animation on the corner pin
        boost::shared_ptr<KnobDouble> fromPointsKnob[4];
        boost::shared_ptr<KnobDouble> toPointsKnob[4];
        boost::shared_ptr<KnobBool> enabledPointsKnob[4];
        for (int i = 0; i < 4; ++i) {
            fromPointsKnob[i] = fromPoints[i].lock();
            toPointsKnob[i] = toPoints[i].lock();
            enabledPointsKnob[i] = enableToPoint[i].lock();
        }
        
        
        for (int i = 0; i < 4; ++i) {
            fromPointsKnob[i]->removeAnimation(ViewSpec::all(), 0);
            fromPointsKnob[i]->removeAnimation(ViewSpec::all(), 1);
            toPointsKnob[i]->removeAnimation(ViewSpec::all(),0);
            toPointsKnob[i]->removeAnimation(ViewSpec::all(),1);
            enabledPointsKnob[i]->removeAnimation(ViewSpec::all(),0);
        }
    }
    boost::shared_ptr<KnobDouble> centerKnob = center.lock();
    centerKnob->removeAnimation(ViewSpec::all(),0);
    centerKnob->removeAnimation(ViewSpec::all(),1);
    {
        // Revert animation on the transform
        boost::shared_ptr<KnobDouble> translationKnob = translate.lock();
        boost::shared_ptr<KnobDouble> scaleKnob = scale.lock();
        boost::shared_ptr<KnobDouble> rotationKnob = rotate.lock();
        
        translationKnob->removeAnimation(ViewSpec::all(),0);
        translationKnob->removeAnimation(ViewSpec::all(),1);
        
        scaleKnob->removeAnimation(ViewSpec::all(),0);
        scaleKnob->removeAnimation(ViewSpec::all(),1);
        
        rotationKnob->removeAnimation(ViewSpec::all(),0);
    }
    
}

void
TrackerContextPrivate::computeTransformParamsFromTracksEnd(double refTime,
                                                    const QList<TransformData>& results)
{
    
    QList<TransformData> validResults;
    for (QList<TransformData>::const_iterator it = results.begin(); it!=results.end(); ++it) {
        if (it->valid) {
            validResults.push_back(*it);
        }
    }
    
    boost::shared_ptr<KnobInt> smoothKnob = smoothTransform.lock();
    int smoothTJitter, smoothRJitter, smoothSJitter;
    
    smoothTJitter = smoothKnob->getValue(0);
    smoothRJitter = smoothKnob->getValue(1);
    smoothSJitter = smoothKnob->getValue(2);
    
    boost::shared_ptr<KnobDouble> translationKnob = translate.lock();
    boost::shared_ptr<KnobDouble> scaleKnob = scale.lock();
    boost::shared_ptr<KnobDouble> rotationKnob = rotate.lock();
    
    translationKnob->blockValueChanges();
    scaleKnob->blockValueChanges();
    rotationKnob->blockValueChanges();
    
    
    std::list<KnobPtr> animatedKnobsChanged;
    animatedKnobsChanged.push_back(translationKnob);
    animatedKnobsChanged.push_back(scaleKnob);
    animatedKnobsChanged.push_back(rotationKnob);
    
    resetTransformParamsAnimation();
    
    Curve tmpTXCurve,tmpTYCurve,tmpRotateCurve,tmpScaleCurve;
    for (QList<TransformData>::const_iterator itResults = validResults.begin(); itResults != validResults.end(); ++itResults) {
        
        const TransformData& dataAtTime = *itResults;
        if (smoothTJitter > 1) {
            int halfJitter = smoothTJitter / 2;
            Point avgT = {0, 0};
            
            // Get halfJitter samples before the given time
            QList<TransformData>::const_iterator prevHalfIt = itResults;
            int nSamplesBefore = 0;
            Point lastSampleWithTranslation = {0, 0};
            if (itResults == validResults.begin()) {
                lastSampleWithTranslation = itResults->translation;
            }
            while (prevHalfIt != validResults.begin() && nSamplesBefore <= halfJitter) {
                avgT.x += prevHalfIt->translation.x;
                avgT.y += prevHalfIt->translation.y;
                lastSampleWithTranslation = prevHalfIt->translation;
                ++nSamplesBefore;
                --prevHalfIt;
            }
            
            while (nSamplesBefore <= halfJitter) {
                assert(prevHalfIt == validResults.begin());
                avgT.x += lastSampleWithTranslation.x;
                avgT.y += lastSampleWithTranslation.y;
                ++nSamplesBefore;
            }
            
            // Get halfJitter samples after the given time
            QList<TransformData>::const_iterator nextHalfIt = itResults;
            int nSamplesAfter = 0;
            ++nextHalfIt;
            lastSampleWithTranslation.x = lastSampleWithTranslation.y = 0.;
            while (nextHalfIt != validResults.end() && nSamplesAfter < halfJitter) {
                avgT.x += nextHalfIt->translation.x;
                avgT.y += nextHalfIt->translation.y;
                lastSampleWithTranslation = nextHalfIt->translation;
                ++nSamplesAfter;
                ++nextHalfIt;
            }
            
            while (nSamplesAfter < halfJitter) {
                assert(nextHalfIt == validResults.end());
                avgT.x += lastSampleWithTranslation.x;
                avgT.y += lastSampleWithTranslation.y;
                ++nSamplesAfter;
            }
            
            
            const int nSamples = nSamplesBefore + nSamplesAfter;
            if (nSamples) {
                avgT.x /= nSamples;
                avgT.y /= nSamples;
            }
            KeyFrame kx(dataAtTime.time, avgT.x);
            KeyFrame ky(dataAtTime.time, avgT.y);
            tmpTXCurve.addKeyFrame(kx);
            tmpTYCurve.addKeyFrame(ky);
            //translationKnob->setValueAtTime(dataAtTime[i].time, avgT.x, ViewSpec::all(), 0);
            //translationKnob->setValueAtTime(dataAtTime[i].time, avgT.y, ViewSpec::all(), 1);
        } else {
            KeyFrame kx(dataAtTime.time, dataAtTime.translation.x);
            KeyFrame ky(dataAtTime.time, dataAtTime.translation.y);
            tmpTXCurve.addKeyFrame(kx);
            tmpTYCurve.addKeyFrame(ky);
            //translationKnob->setValueAtTime(dataAtTime[i].time, dataAtTime[i].data.translation.x, ViewSpec::all(), 0);
            //translationKnob->setValueAtTime(dataAtTime[i].time, dataAtTime[i].data.translation.y, ViewSpec::all(), 1);
        }
        
        if (smoothRJitter > 1) {
            int halfJitter = smoothRJitter / 2;
            
            double avg = 0;
            double lastSampleWithRotation = 0;
            if (itResults == validResults.begin() && itResults->hasRotationAndScale) {
                lastSampleWithRotation = itResults->scale;
            }
            // Get halfJitter samples before the given time
            QList<TransformData>::const_iterator prevHalfIt = itResults;
            int nSamplesBefore = 0;
            while (prevHalfIt != validResults.begin() && nSamplesBefore <= halfJitter) {
                if (prevHalfIt->hasRotationAndScale) {
                    avg += prevHalfIt->rotation;
                    lastSampleWithRotation = prevHalfIt->rotation;
                    ++nSamplesBefore;
                }
                --prevHalfIt;
            }
            
            while (nSamplesBefore <= halfJitter && lastSampleWithRotation != 0. && prevHalfIt->hasRotationAndScale) {
                assert(prevHalfIt == validResults.begin());
                avg += prevHalfIt->rotation;
                ++nSamplesBefore;
            }
            
            // Get halfJitter samples after the given time
            lastSampleWithRotation = 0.;
            QList<TransformData>::const_iterator nextHalfIt = itResults;
            int nSamplesAfter = 0;
            ++nextHalfIt;
            
            while (nextHalfIt != validResults.end() && nSamplesAfter < halfJitter) {
                if (prevHalfIt->hasRotationAndScale) {
                    avg += nextHalfIt->rotation;
                    lastSampleWithRotation = nextHalfIt->rotation;
                    ++nSamplesAfter;
                }
                ++nextHalfIt;
            }
            
            while (nSamplesAfter < halfJitter && lastSampleWithRotation != 0.) {
                assert(nextHalfIt == validResults.end());
                avg += lastSampleWithRotation;
                ++nSamplesAfter;
            }
            
            const int nSamples = nSamplesBefore + nSamplesAfter;
            
            if (nSamples) {
                avg /= nSamples;
                KeyFrame k(dataAtTime.time, avg);
                tmpRotateCurve.addKeyFrame(k);
            }
            
            
            //rotationKnob->setValueAtTime(dataAtTime[i].time, avg, ViewSpec::all(), 0);
        } else {
            if (dataAtTime.hasRotationAndScale) {
                KeyFrame k(dataAtTime.time, dataAtTime.rotation);
                tmpRotateCurve.addKeyFrame(k);
                
                //  rotationKnob->setValueAtTime(dataAtTime[i].time, dataAtTime[i].data.rotation, ViewSpec::all(), 0);
            }
        }
        
        if (smoothSJitter > 1) {
            int halfJitter = smoothSJitter / 2;
            double avg = 0;
            double lastSampleWithScale = 0;
            if (itResults == validResults.begin() && itResults->hasRotationAndScale) {
                lastSampleWithScale = itResults->scale;
            }
            // Get halfJitter samples before the given time
            QList<TransformData>::const_iterator prevHalfIt = itResults;
            int nSamplesBefore = 0;
            while (prevHalfIt != validResults.begin() && nSamplesBefore <= halfJitter) {
                if (prevHalfIt->hasRotationAndScale) {
                    avg += prevHalfIt->scale;
                    lastSampleWithScale = prevHalfIt->scale;
                    ++nSamplesBefore;
                }
                --prevHalfIt;
            }
            
            while (nSamplesBefore <= halfJitter && lastSampleWithScale != 0.) {
                assert(prevHalfIt == validResults.begin());
                avg += lastSampleWithScale;
                ++nSamplesBefore;
            }
            
            // Get halfJitter samples after the given time
            lastSampleWithScale = 0.;
            QList<TransformData>::const_iterator nextHalfIt = itResults;
            int nSamplesAfter = 0;
            ++nextHalfIt;
            
            while (nextHalfIt != validResults.end() && nSamplesAfter < halfJitter) {
                if (prevHalfIt->hasRotationAndScale) {
                    avg += nextHalfIt->scale;
                    lastSampleWithScale = nextHalfIt->scale;
                    ++nSamplesAfter;
                }
                ++nextHalfIt;
            }
            
            while (nSamplesAfter < halfJitter && lastSampleWithScale != 0.) {
                assert(nextHalfIt == validResults.end());
                avg += lastSampleWithScale;
                ++nSamplesAfter;
            }
            
            const int nSamples = nSamplesBefore + nSamplesAfter;
            if (nSamples) {
                avg /= nSamples;
                
                KeyFrame k(dataAtTime.time, avg);
                tmpScaleCurve.addKeyFrame(k);
                //scaleKnob->setValueAtTime(dataAtTime[i].time, avg, ViewSpec::all(), 0);
            }
        } else {
            if (dataAtTime.hasRotationAndScale) {
                KeyFrame k(dataAtTime.time, dataAtTime.scale);
                tmpScaleCurve.addKeyFrame(k);
                //scaleKnob->setValueAtTime(dataAtTime[i].time, dataAtTime[i].data.scale, ViewSpec::all(), 0);
            }
        }
    } // for (std::size_t i = 0; i < dataAtTime.size(); ++i)
    translationKnob->cloneCurve(ViewSpec::all(), 0, tmpTXCurve);
    translationKnob->cloneCurve(ViewSpec::all(), 1, tmpTYCurve);
    rotationKnob->cloneCurve(ViewSpec::all(), 0, tmpRotateCurve);
    scaleKnob->cloneCurve(ViewSpec::all(), 0, tmpScaleCurve);
    scaleKnob->cloneCurve(ViewSpec::all(), 1, tmpScaleCurve);
    
    for (std::list<KnobPtr>::iterator it = animatedKnobsChanged.begin(); it != animatedKnobsChanged.end(); ++it) {
        (*it)->unblockValueChanges();
        int nDims = (*it)->getDimension();
        for (int i = 0; i < nDims; ++i) {
            (*it)->evaluateValueChange(i, refTime, ViewIdx(0), eValueChangedReasonNatronInternalEdited);
        }
    }
    endSolve();
}

void
TrackerContextPrivate::computeTransformParamsFromTracks()
{
    
    
    
#ifndef TRACKER_GENERATE_DATA_SEQUENTIALLY
    lastSolveRequest.cpWatcher.reset();
    lastSolveRequest.tWatcher.reset(new QFutureWatcher<TrackerContextPrivate::TransformData>());
    QObject::connect(lastSolveRequest.tWatcher.get(), SIGNAL(finished()), this, SLOT(onTransformSolverWatcherFinished()));
    QObject::connect(lastSolveRequest.tWatcher.get(), SIGNAL(progressValueChanged(int)), this, SLOT(onTransformSolverWatcherProgress(int)));
    lastSolveRequest.tWatcher->setFuture(QtConcurrent::mapped(lastSolveRequest.keyframes, boost::bind(&TrackerContextPrivate::computeTransformParamsFromTracksAtTime, this, lastSolveRequest.refTime, _1, lastSolveRequest.jitterPeriod, lastSolveRequest.jitterAdd, lastSolveRequest.allMarkers)));
#else
    NodePtr thisNode = node.lock();
    QList<TransformData> validResults;
    {
        int nKeys = keyframes.size();
        int keyIndex = 0;
        for (std::set<double>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it, ++keyIndex) {
            TransformData data = computeTransformParamsFromTracksAtTime(refTime, *it, jitterPeriod, jitterAdd, allMarkers);
            if (data.valid) {
                validResults.push_back(data);
            }
            double progress = (keyIndex + 1) / (double)nKeys;
            thisNode->getApp()->progressUpdate(node, progress);
        }
    }
    computeCornerParamsFromTracksEnd(refTime, validResults);
#endif
    
    
    
} // TrackerContextPrivate::computeTransformParamsFromTracks

void
TrackerContextPrivate::onCornerPinSolverWatcherFinished()
{
    assert(lastSolveRequest.cpWatcher);
    computeCornerParamsFromTracksEnd(lastSolveRequest.refTime, lastSolveRequest.cpWatcher->future().results());
    
}

void
TrackerContextPrivate::onTransformSolverWatcherFinished()
{
    assert(lastSolveRequest.tWatcher);
    computeTransformParamsFromTracksEnd(lastSolveRequest.refTime, lastSolveRequest.tWatcher->future().results());

}

void
TrackerContextPrivate::onCornerPinSolverWatcherProgress(int progress)
{
    assert(lastSolveRequest.cpWatcher);
    NodePtr thisNode = node.lock();
    double min = lastSolveRequest.cpWatcher->progressMinimum();
    double max = lastSolveRequest.cpWatcher->progressMaximum();
    double p = (progress - min) / (max - min);
    thisNode->getApp()->progressUpdate(thisNode, p);
}

void
TrackerContextPrivate::onTransformSolverWatcherProgress(int progress)
{
    assert(lastSolveRequest.tWatcher);
    NodePtr thisNode = node.lock();
    double min = lastSolveRequest.tWatcher->progressMinimum();
    double max = lastSolveRequest.tWatcher->progressMaximum();
    double p = (progress - min) / (max - min);
    thisNode->getApp()->progressUpdate(thisNode, p);
}

void
TrackerContextPrivate::setSolverParamsEnabled(bool enabled)
{
    motionType.lock()->setAllDimensionsEnabled(enabled);
    setCurrentFrameButton.lock()->setAllDimensionsEnabled(enabled);
    referenceFrame.lock()->setAllDimensionsEnabled(enabled);
    transformType.lock()->setAllDimensionsEnabled(enabled);
    jitterPeriod.lock()->setAllDimensionsEnabled(enabled);
    smoothTransform.lock()->setAllDimensionsEnabled(enabled);
    smoothCornerPin.lock()->setAllDimensionsEnabled(enabled);
    
}

void
TrackerContextPrivate::endSolve()
{
    lastSolveRequest.cpWatcher.reset();
    lastSolveRequest.tWatcher.reset();
    lastSolveRequest.keyframes.clear();
    lastSolveRequest.allMarkers.clear();
    setSolverParamsEnabled(true);
    NodePtr n = node.lock();
    n->getApp()->progressEnd(n);
    n->getEffectInstance()->endChanges();

}

NATRON_NAMESPACE_EXIT;
NATRON_NAMESPACE_USING;
#include "moc_TrackerContextPrivate.cpp"