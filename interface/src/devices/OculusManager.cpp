//
//  OculusManager.cpp
//  hifi
//
//  Created by Stephen Birarda on 5/9/13.
//  Copyright (c) 2012 High Fidelity, Inc. All rights reserved.
//

#include "OculusManager.h"
#include <glm/glm.hpp>

bool OculusManager::_isConnected = false;

#ifdef __APPLE__
Ptr<DeviceManager> OculusManager::_deviceManager;
Ptr<HMDDevice> OculusManager::_hmdDevice;
Ptr<SensorDevice> OculusManager::_sensorDevice;
SensorFusion OculusManager::_sensorFusion;
float OculusManager::_yawOffset = 0;
#endif

void OculusManager::connect() {
#ifdef __APPLE__
    System::Init();
    _deviceManager = *DeviceManager::Create();
    _hmdDevice = *_deviceManager->EnumerateDevices<HMDDevice>().CreateDevice();

    if (_hmdDevice) {
        _isConnected = true;
        
        _sensorDevice = *_hmdDevice->GetSensor();
        _sensorFusion.AttachToSensor(_sensorDevice);
        
        // default the yaw to the current orientation
        _sensorFusion.SetMagReference();
    }
#endif
}

void OculusManager::updateYawOffset() {
#ifdef __APPLE__
    float yaw, pitch, roll;
   _sensorFusion.GetOrientation().GetEulerAngles<Axis_Y, Axis_X, Axis_Z, Rotate_CCW, Handed_R>(&yaw, &pitch, &roll);
    _yawOffset = yaw;
#endif
}

void OculusManager::getEulerAngles(float& yaw, float& pitch, float& roll) {
#ifdef __APPLE__
    _sensorFusion.GetOrientation().GetEulerAngles<Axis_Y, Axis_X, Axis_Z, Rotate_CCW, Handed_R>(&yaw, &pitch, &roll);
    
    // convert each angle to degrees
    // remove the yaw offset from the returned yaw
    yaw = glm::degrees(yaw - _yawOffset);
    pitch = glm::degrees(pitch);
    roll = glm::degrees(roll);
#endif
}

