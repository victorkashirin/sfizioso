// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Controller.h"
#include "../ModKey.h"
#include "../../Smoothers.h"
#include "../../ModifierHelpers.h"
#include "../../Resources.h"
#include "../../Config.h"
#include "../../utility/Debug.h"
#include <absl/container/flat_hash_map.h>

namespace sfz {

namespace {
struct SmootherKey {
    ModKey source;
    NumericId<Voice> voice;

    bool operator==(const SmootherKey& other) const noexcept
    {
        return source == other.source && voice == other.voice;
    }
};

struct SmootherKeyHash {
    size_t operator()(const SmootherKey& key) const noexcept
    {
        const size_t sourceHash = key.source.hash();
        const size_t voiceHash = std::hash<int> {}(key.voice.number());
        return sourceHash ^ (voiceHash + size_t(0x9e3779b9)
            + (sourceHash << 6) + (sourceHash >> 2));
    }
};
}

struct ControllerSource::Impl {
    float getLastTransformedValue(
        const ModKey& sourceKey, NumericId<Voice> voice) const noexcept;
    double sampleRate_ = config::defaultSampleRate;
    Resources* res_ = nullptr;
    VoiceManager* voiceManager_ = nullptr;
    absl::flat_hash_map<SmootherKey, Smoother, SmootherKeyHash> smoother_;
    size_t preparedSmootherCount_ = 0;
};

ControllerSource::ControllerSource(Resources& res, VoiceManager& manager)
    : impl_(new Impl)
{
    impl_->res_ = &res;
    impl_->voiceManager_ = &manager;
}

ControllerSource::~ControllerSource()
{
}

float ControllerSource::Impl::getLastTransformedValue(
    const ModKey& sourceKey, NumericId<Voice> voiceId) const noexcept
{
    ASSERT(res_);
    const ModKey::Parameters p = sourceKey.parameters();
    float lastCCValue = res_->getMidiState().getCCValue(p.cc);
    if (voiceId) {
        if (const Voice* voice = voiceManager_->getVoiceById(voiceId)) {
            const EventVector& events = voice->getControllerEvents(p.cc);
            if (!events.empty())
                lastCCValue = events.back().value;
        }
    }
    const Curve& curve = res_->getCurves().getCurve(p.curve);
    return curve.evalNormalized(lastCCValue);
}

void ControllerSource::resetSmoothers()
{
    for (auto& item : impl_->smoother_) {
        item.second.reset(impl_->getLastTransformedValue(
            item.first.source, item.first.voice));
    }
}

void ControllerSource::clearSmoothers()
{
    impl_->smoother_.clear();
    impl_->preparedSmootherCount_ = 0;
}

void ControllerSource::setSampleRate(double sampleRate)
{
    if (impl_->sampleRate_ == sampleRate)
        return;

    impl_->sampleRate_ = sampleRate;

    for (auto& item : impl_->smoother_) {
        const ModKey::Parameters p = item.first.source.parameters();
        item.second.setSmoothing(p.smooth, sampleRate);
    }
}

void ControllerSource::setSamplesPerBlock(unsigned count)
{
    (void)count;
}

void ControllerSource::prepare(const ModKey& sourceKey)
{
    const ModKey::Parameters p = sourceKey.parameters();
    if (p.smooth == 0)
        return;

    const size_t additional = sourceKey.flags() & kModIsPerVoice
        ? config::maxVoices
        : 1;
    impl_->preparedSmootherCount_ += additional;
    impl_->smoother_.reserve(impl_->preparedSmootherCount_);
}

void ControllerSource::init(const ModKey& sourceKey, NumericId<Voice> voiceId, unsigned delay)
{
    (void)delay;

    const ModKey::Parameters p = sourceKey.parameters();
    const NumericId<Voice> smootherVoice =
        sourceKey.flags() & kModIsPerVoice ? voiceId : NumericId<Voice> {};
    const SmootherKey key { sourceKey, smootherVoice };
    if (p.smooth > 0) {
        Smoother s;
        s.setSmoothing(p.smooth, impl_->sampleRate_);
        s.reset(impl_->getLastTransformedValue(sourceKey, smootherVoice));
        impl_->smoother_[key] = s;
    }
    else {
        impl_->smoother_.erase(key);
    }
}

void ControllerSource::generate(const ModKey& sourceKey, NumericId<Voice> voiceId, absl::Span<float> buffer)
{
    const ModKey::Parameters p = sourceKey.parameters();
    const Resources& res = *impl_->res_;
    const Curve& curve = res.getCurves().getCurve(p.curve);
    const MidiState& ms = res.getMidiState();
    bool canShortcut = false;

    auto transformValue = [&] (float x) {
        return curve.evalNormalized(x);
    };

    auto quantize = [&] (float x) {
        if (p.step > 0.0f)
            return std::trunc(x / p.step) * p.step;

        return x;
    };

    switch(p.cc) {
    case ExtendedCCs::polyphonicAftertouch: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue =
                voice && voice->getTriggerEvent().type == TriggerEventType::NoteOn ?
                impl_->res_->getMidiState().getPolyAftertouch(voice->getTriggerEvent().number) : 0.0f;

            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::noteOnVelocity: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue =
                voice && voice->getTriggerEvent().type == TriggerEventType::NoteOn ?
                voice->getTriggerEvent().value : 0.0f;

            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::noteOffVelocity: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue =
                voice && voice->getTriggerEvent().type == TriggerEventType::NoteOff ?
                voice->getTriggerEvent().value : 0.0f;

            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::keyboardNoteNumber: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? normalize7Bits(voice->getTriggerEvent().number) : 0.0f;
            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::keyboardNoteGate: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? voice->getExtendedCCValues().noteGate : 0.0f;
            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::unipolarRandom: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? voice->getExtendedCCValues().unipolar : 0.0f;
            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::bipolarRandom: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? voice->getExtendedCCValues().bipolar : 0.0f;
            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::alternate: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? voice->getExtendedCCValues().alternate : 0.0f;
            sfz::fill(buffer, quantize(transformValue(fillValue)));
            canShortcut = true;
            break;
        }
    case AriaExtendedCCs::keydelta: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? voice->getExtendedCCValues().keydelta : 0.0f;
            sfz::fill(buffer, quantize(fillValue));
            canShortcut = true;
            break;
        }
    case AriaExtendedCCs::absoluteKeydelta: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const float fillValue = voice ? std::abs(voice->getExtendedCCValues().keydelta) : 0.0f;
            sfz::fill(buffer, quantize(fillValue));
            canShortcut = true;
            break;
        }
    case ExtendedCCs::pitchBend: // fallthrough
    case ExtendedCCs::channelAftertouch: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const TriggerEvent event = voice
                ? voice->getTriggerEvent()
                : TriggerEvent { TriggerEventType::CC, 0, 0.0f };
            const EventVector& events = voice ? voice->getControllerEvents(p.cc)
                : ms.getVoiceCCEvents(event.expressionTarget, event.noteId, p.cc);
            linearEnvelope(events, buffer, [](float x) { return x; }, p.step);
            canShortcut = events.size() == 1;
            break;
        }
    default: {
            const auto voice = impl_->voiceManager_->getVoiceById(voiceId);
            const TriggerEvent event = voice
                ? voice->getTriggerEvent()
                : TriggerEvent { TriggerEventType::CC, 0, 0.0f };
            const EventVector& events = voice ? voice->getControllerEvents(p.cc)
                : ms.getVoiceCCEvents(event.expressionTarget, event.noteId, p.cc);
            linearEnvelope(events, buffer, transformValue, p.step);
            canShortcut = events.size() == 1;
        }
    }

    const NumericId<Voice> smootherVoice =
        sourceKey.flags() & kModIsPerVoice ? voiceId : NumericId<Voice> {};
    auto it = impl_->smoother_.find({ sourceKey, smootherVoice });
    if (it != impl_->smoother_.end()) {
        Smoother& s = it->second;
        s.process(buffer, buffer, canShortcut);
    }
}

} // namespace sfz
