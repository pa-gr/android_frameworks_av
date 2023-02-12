/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AHAL_DynamicsProcessingLibEffects"

#include <android-base/logging.h>

#include "DynamicsProcessing.h"

#include <dsp/DPBase.h>
#include <dsp/DPFrequency.h>

using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::DynamicsProcessingImpl;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::kDynamicsProcessingImplUUID;
using aidl::android::hardware::audio::effect::State;
using aidl::android::media::audio::common::AudioUuid;
using aidl::android::media::audio::common::PcmType;

extern "C" binder_exception_t createEffect(const AudioUuid* in_impl_uuid,
                                           std::shared_ptr<IEffect>* instanceSpp) {
    if (!in_impl_uuid || *in_impl_uuid != kDynamicsProcessingImplUUID) {
        LOG(ERROR) << __func__ << "uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    if (instanceSpp) {
        *instanceSpp = ndk::SharedRefBase::make<DynamicsProcessingImpl>();
        LOG(DEBUG) << __func__ << " instance " << instanceSpp->get() << " created";
        return EX_NONE;
    } else {
        LOG(ERROR) << __func__ << " invalid input parameter!";
        return EX_ILLEGAL_ARGUMENT;
    }
}

extern "C" binder_exception_t queryEffect(const AudioUuid* in_impl_uuid, Descriptor* _aidl_return) {
    if (!in_impl_uuid || *in_impl_uuid != kDynamicsProcessingImplUUID) {
        LOG(ERROR) << __func__ << "uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    *_aidl_return = DynamicsProcessingImpl::kDescriptor;
    return EX_NONE;
}

namespace aidl::android::hardware::audio::effect {

const std::string DynamicsProcessingImpl::kEffectName = "DynamicsProcessing";
const DynamicsProcessing::Capability DynamicsProcessingImpl::kCapability = {.minCutOffFreq = 220,
                                                                            .maxCutOffFreq = 20000};
const Descriptor DynamicsProcessingImpl::kDescriptor = {
        .common = {.id = {.type = kDynamicsProcessingTypeUUID,
                          .uuid = kDynamicsProcessingImplUUID,
                          .proxy = std::nullopt},
                   .flags = {.type = Flags::Type::INSERT,
                             .insert = Flags::Insert::LAST,
                             .volume = Flags::Volume::CTRL},
                   .name = DynamicsProcessingImpl::kEffectName,
                   .implementor = "The Android Open Source Project"},
        .capability = Capability::make<Capability::dynamicsProcessing>(
                DynamicsProcessingImpl::kCapability)};

ndk::ScopedAStatus DynamicsProcessingImpl::open(const Parameter::Common& common,
                                                const std::optional<Parameter::Specific>& specific,
                                                OpenEffectReturn* ret) {
    LOG(DEBUG) << __func__;
    // effect only support 32bits float
    RETURN_IF(common.input.base.format.pcm != common.output.base.format.pcm ||
                      common.input.base.format.pcm != PcmType::FLOAT_32_BIT,
              EX_ILLEGAL_ARGUMENT, "dataMustBe32BitsFloat");
    RETURN_OK_IF(mState != State::INIT);
    auto context = createContext(common);
    RETURN_IF(!context, EX_NULL_POINTER, "createContextFailed");

    RETURN_IF_ASTATUS_NOT_OK(setParameterCommon(common), "setCommParamErr");
    if (specific.has_value()) {
        RETURN_IF_ASTATUS_NOT_OK(setParameterSpecific(specific.value()), "setSpecParamErr");
    } else {
        Parameter::Specific defaultSpecific =
                Parameter::Specific::make<Parameter::Specific::dynamicsProcessing>(
                        DynamicsProcessing::make<DynamicsProcessing::engineArchitecture>(
                                mContext->getEngineArchitecture()));
        RETURN_IF_ASTATUS_NOT_OK(setParameterSpecific(defaultSpecific), "setDefaultEngineErr");
    }

    mState = State::IDLE;
    context->dupeFmq(ret);
    RETURN_IF(createThread(context, getEffectName()) != RetCode::SUCCESS, EX_UNSUPPORTED_OPERATION,
              "FailedToCreateWorker");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DynamicsProcessingImpl::getDescriptor(Descriptor* _aidl_return) {
    RETURN_IF(!_aidl_return, EX_ILLEGAL_ARGUMENT, "Parameter:nullptr");
    LOG(DEBUG) << __func__ << kDescriptor.toString();
    *_aidl_return = kDescriptor;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DynamicsProcessingImpl::commandImpl(CommandId command) {
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");
    switch (command) {
        case CommandId::START:
            mContext->enable();
            return ndk::ScopedAStatus::ok();
        case CommandId::STOP:
            mContext->disable();
            return ndk::ScopedAStatus::ok();
        case CommandId::RESET:
            mContext->disable();
            mContext->resetBuffer();
            return ndk::ScopedAStatus::ok();
        default:
            // Need this default handling for vendor extendable CommandId::VENDOR_COMMAND_*
            LOG(ERROR) << __func__ << " commandId " << toString(command) << " not supported";
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "commandIdNotSupported");
    }
}

ndk::ScopedAStatus DynamicsProcessingImpl::setParameterSpecific(
        const Parameter::Specific& specific) {
    RETURN_IF(Parameter::Specific::dynamicsProcessing != specific.getTag(), EX_ILLEGAL_ARGUMENT,
              "EffectNotSupported");
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");

    auto& param = specific.get<Parameter::Specific::dynamicsProcessing>();
    auto tag = param.getTag();

    switch (tag) {
        case DynamicsProcessing::engineArchitecture: {
            RETURN_IF(mContext->setEngineArchitecture(
                              param.get<DynamicsProcessing::engineArchitecture>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setEngineArchitectureFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::preEq: {
            RETURN_IF(
                    mContext->setPreEq(param.get<DynamicsProcessing::preEq>()) != RetCode::SUCCESS,
                    EX_ILLEGAL_ARGUMENT, "setPreEqFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::postEq: {
            RETURN_IF(mContext->setPostEq(param.get<DynamicsProcessing::postEq>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setPostEqFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::preEqBand: {
            RETURN_IF(mContext->setPreEqBand(param.get<DynamicsProcessing::preEqBand>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setPreEqBandFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::postEqBand: {
            RETURN_IF(mContext->setPostEqBand(param.get<DynamicsProcessing::postEqBand>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setPostEqBandFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::mbc: {
            RETURN_IF(mContext->setMbc(param.get<DynamicsProcessing::mbc>()) != RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setMbcFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::mbcBand: {
            RETURN_IF(mContext->setMbcBand(param.get<DynamicsProcessing::mbcBand>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setMbcBandFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::limiter: {
            RETURN_IF(mContext->setLimiter(param.get<DynamicsProcessing::limiter>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setLimiterFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::inputGain: {
            RETURN_IF(mContext->setInputGain(param.get<DynamicsProcessing::inputGain>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setInputGainFailed");
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::vendorExtension: {
            LOG(ERROR) << __func__ << " unsupported tag: " << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_ILLEGAL_ARGUMENT, "DPVendorExtensionTagNotSupported");
        }
    }
}

ndk::ScopedAStatus DynamicsProcessingImpl::getParameterSpecific(const Parameter::Id& id,
                                                                Parameter::Specific* specific) {
    RETURN_IF(!specific, EX_NULL_POINTER, "nullPtr");
    auto tag = id.getTag();
    RETURN_IF(Parameter::Id::dynamicsProcessingTag != tag, EX_ILLEGAL_ARGUMENT, "wrongIdTag");
    auto dpId = id.get<Parameter::Id::dynamicsProcessingTag>();
    auto dpIdTag = dpId.getTag();
    switch (dpIdTag) {
        case DynamicsProcessing::Id::commonTag:
            return getParameterDynamicsProcessing(dpId.get<DynamicsProcessing::Id::commonTag>(),
                                                  specific);
        case DynamicsProcessing::Id::vendorExtensionTag:
            LOG(ERROR) << __func__ << " unsupported ID: " << toString(dpIdTag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_ILLEGAL_ARGUMENT, "DPVendorExtensionIdNotSupported");
    }
}

ndk::ScopedAStatus DynamicsProcessingImpl::getParameterDynamicsProcessing(
        const DynamicsProcessing::Tag& tag, Parameter::Specific* specific) {
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");

    switch (tag) {
        case DynamicsProcessing::engineArchitecture: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::engineArchitecture>(
                            mContext->getEngineArchitecture()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::preEq: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::preEq>(mContext->getPreEq()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::postEq: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::postEq>(mContext->getPostEq()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::preEqBand: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::preEqBand>(
                            mContext->getPreEqBand()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::postEqBand: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::postEqBand>(
                            mContext->getPostEqBand()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::mbc: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::mbc>(mContext->getMbc()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::mbcBand: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::mbcBand>(mContext->getMbcBand()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::limiter: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::limiter>(mContext->getLimiter()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::inputGain: {
            specific->set<Parameter::Specific::dynamicsProcessing>(
                    DynamicsProcessing::make<DynamicsProcessing::inputGain>(
                            mContext->getInputGain()));
            return ndk::ScopedAStatus::ok();
        }
        case DynamicsProcessing::vendorExtension: {
            LOG(ERROR) << __func__ << " wrong vendor tag in CommonTag: " << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_ILLEGAL_ARGUMENT, "DPVendorExtensionTagInWrongId");
        }
    }
}

std::shared_ptr<EffectContext> DynamicsProcessingImpl::createContext(
        const Parameter::Common& common) {
    if (mContext) {
        LOG(DEBUG) << __func__ << " context already exist";
        return mContext;
    }

    mContext = std::make_shared<DynamicsProcessingContext>(1 /* statusFmqDepth */, common);
    return mContext;
}

RetCode DynamicsProcessingImpl::releaseContext() {
    if (mContext) {
        mContext->disable();
        mContext->resetBuffer();
        mContext.reset();
    }
    return RetCode::SUCCESS;
}

// Processing method running in EffectWorker thread.
IEffect::Status DynamicsProcessingImpl::effectProcessImpl(float* in, float* out, int samples) {
    IEffect::Status status = {EX_NULL_POINTER, 0, 0};
    RETURN_VALUE_IF(!mContext, status, "nullContext");
    return mContext->lvmProcess(in, out, samples);
}

}  // namespace aidl::android::hardware::audio::effect
