//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#define OZZ_INCLUDE_PRIVATE_HEADER  // Allows to include private headers.

#include "animation/offline/fbx/fbx_animation.h"

#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_track.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"

#include "ozz/base/log.h"

#include "ozz/base/maths/transform.h"

namespace ozz {
namespace animation {
namespace offline {
namespace fbx {

namespace {
struct SamplingInfo {
  float start;
  float end;
  float duration;
  float period;
};

SamplingInfo ExtractSamplingInfo(FbxScene* _scene, FbxAnimStack* _anim_stack,
                                 float _sampling_rate) {
  SamplingInfo info;

  // Extract animation duration.
  FbxTimeSpan time_spawn;
  const FbxTakeInfo* take_info = _scene->GetTakeInfo(_anim_stack->GetName());
  if (take_info) {
    time_spawn = take_info->mLocalTimeSpan;
  } else {
    _scene->GetGlobalSettings().GetTimelineDefaultTimeSpan(time_spawn);
  }

  // Get frame rate from the scene.
  FbxTime::EMode mode = _scene->GetGlobalSettings().GetTimeMode();
  const float scene_frame_rate =
      static_cast<float>((mode == FbxTime::eCustom)
                             ? _scene->GetGlobalSettings().GetCustomFrameRate()
                             : FbxTime::GetFrameRate(mode));

  // Deduce sampling period.
  // Scene frame rate is used when provided argument is <= 0.
  float sampling_rate;
  if (_sampling_rate > 0.f) {
    sampling_rate = _sampling_rate;
    log::Log() << "Using sampling rate of " << sampling_rate << "hz."
               << std::endl;
  } else {
    sampling_rate = scene_frame_rate;
    log::Log() << "Using scene sampling rate of " << sampling_rate << "hz."
               << std::endl;
  }
  info.period = 1.f / sampling_rate;

  // Get scene start and end.
  info.start = static_cast<float>(time_spawn.GetStart().GetSecondDouble());
  info.end = static_cast<float>(time_spawn.GetStop().GetSecondDouble());

  // Duration could be 0 if it's just a pose. In this case we'll set a default
  // 1s duration.
  if (info.end > info.start) {
    info.duration = info.end - info.start;
  } else {
    info.duration = 1.f;
  }

  return info;
}

bool ExtractAnimation(FbxSceneLoader* _scene_loader, FbxAnimStack* _anim_stack,
                      const SamplingInfo& _info, const Skeleton& _skeleton,
                      RawAnimation* _animation) {
  FbxScene* scene = _scene_loader->scene();
  assert(scene);

  ozz::log::Log() << "Extracting animation \"" << _anim_stack->GetName() << "\""
                  << std::endl;

  // Setup Fbx animation evaluator.
  scene->SetCurrentAnimationStack(_anim_stack);

  // Set animation data.
  _animation->name = _anim_stack->GetName();
  _animation->duration = _info.duration;

  // Allocates all tracks with the same number of joints as the skeleton.
  // Tracks that would not be found will be set to skeleton bind-pose
  // transformation.
  _animation->tracks.resize(_skeleton.num_joints());

  // Iterate all skeleton joints and fills there track with key frames.
  FbxAnimEvaluator* evaluator = scene->GetAnimationEvaluator();
  for (int i = 0; i < _skeleton.num_joints(); i++) {
    RawAnimation::JointTrack& track = _animation->tracks[i];

    // Find a node that matches skeleton joint.
    const char* joint_name = _skeleton.joint_names()[i];
    FbxNode* node = scene->FindNodeByName(joint_name);

    if (!node) {
      // Empty joint track.
      ozz::log::LogV() << "No animation track found for joint \"" << joint_name
                       << "\". Using skeleton bind pose instead." << std::endl;

      // Get joint's bind pose.
      const ozz::math::Transform& bind_pose =
          ozz::animation::GetJointLocalBindPose(_skeleton, i);

      const RawAnimation::TranslationKey tkey = {0.f, bind_pose.translation};
      track.translations.push_back(tkey);

      const RawAnimation::RotationKey rkey = {0.f, bind_pose.rotation};
      track.rotations.push_back(rkey);

      const RawAnimation::ScaleKey skey = {0.f, bind_pose.scale};
      track.scales.push_back(skey);

      continue;
    }

    // Reserve keys in animation tracks (allocation strategy optimization
    // purpose).
    const int max_keys =
        static_cast<int>(3.f + (_info.end - _info.start) / _info.period);
    track.translations.reserve(max_keys);
    track.rotations.reserve(max_keys);
    track.scales.reserve(max_keys);

    // Evaluate joint transformation at the specified time.
    // Make sure to include "end" time, and enter the loop once at least.
    bool loop_again = true;
    for (float t = _info.start; loop_again; t += _info.period) {
      if (t >= _info.end) {
        t = _info.end;
        loop_again = false;
      }

      // Evaluate transform matrix at t.
      const FbxAMatrix matrix =
          _skeleton.joint_properties()[i].parent == Skeleton::kNoParentIndex
              ? evaluator->GetNodeGlobalTransform(node, FbxTimeSeconds(t))
              : evaluator->GetNodeLocalTransform(node, FbxTimeSeconds(t));

      // Convert to a transform object in ozz unit/axis system.
      ozz::math::Transform transform;
      if (!_scene_loader->converter()->ConvertTransform(matrix, &transform)) {
        ozz::log::Err() << "Failed to extract animation transform for joint \""
                        << joint_name << "\" at t = " << t << "s." << std::endl;
        return false;
      }

      // Fills corresponding track.
      const float local_time = t - _info.start;
      const RawAnimation::TranslationKey translation = {local_time,
                                                        transform.translation};
      track.translations.push_back(translation);
      const RawAnimation::RotationKey rotation = {local_time,
                                                  transform.rotation};
      track.rotations.push_back(rotation);
      const RawAnimation::ScaleKey scale = {local_time, transform.scale};
      track.scales.push_back(scale);
    }
  }

  // Output animation must be valid at that point.
  assert(_animation->Validate());

  return true;
}

bool GetValue(FbxPropertyValue& _property_value, EFbxType _type,
              float* _value) {
  switch (_type) {
    case EFbxType::eFbxBool: {
      bool value;
      bool success = _property_value.Get(&value, EFbxType::eFbxBool);
      *_value = value ? 1.f : 0.f;
      return success;
    }
    case EFbxType::eFbxInt: {
      int value;
      bool success = _property_value.Get(&value, EFbxType::eFbxInt);
      *_value = static_cast<float>(value);
      return success;
    }
    case EFbxType::eFbxFloat: {
      return _property_value.Get(_value, EFbxType::eFbxFloat);
    }
    case EFbxType::eFbxDouble: {
      double value;
      bool success = _property_value.Get(&value, EFbxType::eFbxDouble);
      *_value = static_cast<float>(value);
      return success;
    }
    default: {
      assert(false);
      return false;
    }
  }
}

bool GetValue(FbxPropertyValue& _property_value, EFbxType _type,
              ozz::math::Float2* _value) {
  assert(_type == EFbxType::eFbxDouble2);
  double dvalue[2];
  if (!_property_value.Get(&dvalue, EFbxType::eFbxDouble2)) {
    return false;
  }
  _value->x = static_cast<float>(dvalue[0]);
  _value->y = static_cast<float>(dvalue[1]);

  return true;
}

bool GetValue(FbxPropertyValue& _property_value, EFbxType _type,
              ozz::math::Float3* _value) {
  assert(_type == EFbxType::eFbxDouble3);
  double dvalue[3];
  if (!_property_value.Get(&dvalue, EFbxType::eFbxDouble3)) {
    return false;
  }
  _value->x = static_cast<float>(dvalue[0]);
  _value->y = static_cast<float>(dvalue[1]);
  _value->z = static_cast<float>(dvalue[2]);

  return true;
}

template <typename _Track>
bool ExtractCurve(FbxSceneLoader* _scene_loader, FbxProperty& _property,
                  EFbxType _type, const SamplingInfo& _info, _Track* _track) {
  assert(_track->keyframes.size() == 0);

  FbxScene* scene = _scene_loader->scene();
  assert(scene);

  FbxAnimEvaluator* evaluator = scene->GetAnimationEvaluator();

  if (_property.IsAnimated()) {
    FbxPropertyValue& property_value =
        evaluator->GetPropertyValue(_property, FbxTimeSeconds(0.));

    typename _Track::ValueType value;
    bool success = GetValue(property_value, _type, &value);
    if (!success) {
      return false;
    }

    // Build and push keyframe
    const typename _Track::Keyframe key = {RawTrackInterpolation::kStep, 0.f,
                                           value};
    _track->keyframes.push_back(key);
  } else {
    // Reserve keys
    const int max_keys =
        static_cast<int>(3.f + (_info.end - _info.start) / _info.period);
    _track->keyframes.reserve(max_keys);

    // Evaluate values at the specified time.
    // Make sure to include "end" time, and enter the loop once at least.
    bool loop_again = true;
    for (float t = _info.start; loop_again; t += _info.period) {
      if (t >= _info.end) {
        t = _info.end;
        loop_again = false;
      }

      FbxPropertyValue& property_value =
          evaluator->GetPropertyValue(_property, FbxTimeSeconds(t));

      // It shouldn't fail as property type is known.
      typename _Track::ValueType value;
      bool success = GetValue(property_value, _type, &value);
      (void)success;
      assert(success);

      // Build and push keyframe
      const typename _Track::Keyframe key = {RawTrackInterpolation::kLinear,
                                             (t - _info.start) / _info.duration,
                                             value};
      _track->keyframes.push_back(key);
    }
  }

  return _track->Validate();
}

const char* FbxTypeToString(EFbxType _type) {
  switch (_type) {
    case eFbxUndefined:
      return "eFbxUndefined - Unidentified";
    case eFbxChar:
      return "eFbxChar - 8 bit signed integer";
    case eFbxUChar:
      return "eFbxUChar - 8 bit unsigned integer";
    case eFbxShort:
      return "eFbxShort - 16 bit signed integer";
    case eFbxUShort:
      return "eFbxUShort - 16 bit unsigned integer";
    case eFbxUInt:
      return "eFbxUInt - 32 bit unsigned integer";
    case eFbxLongLong:
      return "eFbxLongLong - 64 bit signed integer";
    case eFbxULongLong:
      return "eFbxULongLong - 64 bit unsigned integer";
    case eFbxHalfFloat:
      return "eFbxHalfFloat - 16 bit floating point";
    case eFbxBool:
      return "eFbxBool - Boolean";
    case eFbxInt:
      return "eFbxInt - 32 bit signed integer";
    case eFbxFloat:
      return "eFbxFloat - Floating point value";
    case eFbxDouble:
      return "eFbxDouble - Double width floating point value";
    case eFbxDouble2:
      return "eFbxDouble2 - Vector of two double values";
    case eFbxDouble3:
      return "eFbxDouble3 - Vector of three double values";
    case eFbxDouble4:
      return "eFbxDouble4 - Vector of four double values";
    case eFbxDouble4x4:
      return "eFbxDouble4x4 - Four vectors of four double values";
    case eFbxEnum:
      return "eFbxEnum - Enumeration";
    case eFbxEnumM:
      return "eFbxEnumM - Enumeration allowing duplicated items";
    case eFbxString:
      return "eFbxString - String";
    case eFbxTime:
      return "eFbxTime - Time value";
    case eFbxReference:
      return "eFbxReference - Reference to object or property";
    case eFbxBlob:
      return "eFbxBlob - Binary data block type";
    case eFbxDistance:
      return "eFbxDistance - Distance";
    case eFbxDateTime:
      return "eFbxDateTime - Date and time";
    default:
      return "Unknown";
  }
}

bool ExtractProperty(FbxSceneLoader* _scene_loader, const SamplingInfo& _info,
                     FbxProperty& _property) {
  const EFbxType type = _property.GetPropertyDataType().GetType();
  switch (type) {
    case EFbxType::eFbxBool:
    case EFbxType::eFbxInt:
    case EFbxType::eFbxFloat:
    case EFbxType::eFbxDouble: {
      RawFloatTrack track;
      return ExtractCurve(_scene_loader, _property, type, _info, &track);
    }
    case EFbxType::eFbxDouble2: {
      RawFloat2Track track;
      return ExtractCurve(_scene_loader, _property, type, _info, &track);
    }
    case EFbxType::eFbxDouble3: {
      RawFloat3Track track;
      return ExtractCurve(_scene_loader, _property, type, _info, &track);
    }
    default: {
      log::Err() << "Unsupported track type: " << FbxTypeToString(type) << "\""
                 << std::endl;
      return false;
    }
  }
}
}  // namespace

bool ExtractTrack(FbxSceneLoader* _scene_loader, const SamplingInfo& _info,
                  const char* _node_name, const char* _property_name) {
  ozz::log::Log() << "Extracting animation track \"" << _node_name << ":"
                  << _property_name << "\"" << std::endl;

  FbxScene* scene = _scene_loader->scene();
  FbxNode* node = scene->FindNodeByName(_node_name);
  if (!node) {
    ozz::log::Err() << "Invalid node name \"" << _node_name << "\""
                    << std::endl;
    return false;
  }

  FbxProperty property = node->FindProperty(_property_name);
  if (!property.IsValid()) {
    ozz::log::Err() << "Invalid property name \"" << _property_name << "\""
                    << std::endl;
    return false;
  }

  return ExtractProperty(_scene_loader, _info, property);
}

bool ExtractAnimations(FbxSceneLoader* _scene_loader, const Skeleton& _skeleton,
                       float _sampling_rate, Animations* _animations) {
  // Clears output
  _animations->clear();

  FbxScene* scene = _scene_loader->scene();
  assert(scene);

  int anim_stacks_count = scene->GetSrcObjectCount<FbxAnimStack>();

  // Early out if no animation's found.
  if (anim_stacks_count == 0) {
    ozz::log::Err() << "No animation found." << std::endl;
    return false;
  }

  // Prepares outputs.
  _animations->resize(anim_stacks_count);

  // Sequentially import all available animations.
  bool success = true;
  for (int i = 0; i < anim_stacks_count && success; ++i) {
    FbxAnimStack* anim_stack = scene->GetSrcObject<FbxAnimStack>(i);

    // Extract sampling info relative to the stack.
    const SamplingInfo& info =
        ExtractSamplingInfo(scene, anim_stack, _sampling_rate);

    success &= ExtractAnimation(_scene_loader, anim_stack, info, _skeleton,
                                &_animations->at(i));

    //success &= ExtractTrack(_scene_loader, info, "Hips", "TranslationMaxZ");
  }

  // Clears output if something failed during import, avoids partial data.
  if (!success) {
    _animations->clear();
  }

  return success;
}  // namespace fbx
}  // namespace fbx
}  // namespace offline
}  // namespace animation
}  // namespace ozz
