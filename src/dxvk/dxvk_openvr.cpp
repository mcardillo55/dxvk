#include "dxvk_instance.h"
#include "dxvk_openvr.h"

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <openvr/openvr.hpp>

using VR_InitInternalProc        = vr::IVRSystem* (VR_CALLTYPE *)(vr::EVRInitError*, vr::EVRApplicationType);
using VR_ShutdownInternalProc    = void  (VR_CALLTYPE *)();
using VR_GetGenericInterfaceProc = void* (VR_CALLTYPE *)(const char*, vr::EVRInitError*);

namespace dxvk {
  
  struct VrFunctions {
    VR_InitInternalProc        initInternal        = nullptr;
    VR_ShutdownInternalProc    shutdownInternal    = nullptr;
    VR_GetGenericInterfaceProc getGenericInterface = nullptr;
  };
  
  VrFunctions g_vrFunctions;
  VrInstance  g_vrInstance;

  VrInstance:: VrInstance() { }
  VrInstance::~VrInstance() { }
  
  
  vk::NameSet VrInstance::getInstanceExtensions() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_insExtensions;
  }


  vk::NameSet VrInstance::getDeviceExtensions(uint32_t adapterId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (adapterId < m_devExtensions.size())
      return m_devExtensions[adapterId];
    
    return vk::NameSet();
  }


  void VrInstance::initInstanceExtensions() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_compositor == nullptr)
      m_compositor = this->getCompositor();

    if (m_compositor == nullptr || m_initializedInsExt)
      return;
    
    m_insExtensions = this->queryInstanceExtensions();
    m_initializedInsExt = true;
  }


  void VrInstance::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_compositor == nullptr || m_initializedDevExt)
      return;
    
    for (uint32_t i = 0; instance->enumAdapters(i) != nullptr; i++) {
      m_devExtensions.push_back(this->queryDeviceExtensions(
        instance->enumAdapters(i)->handle()));
    }

    if (m_initializedOpenVr)
      g_vrFunctions.shutdownInternal();

    m_initializedDevExt = true;
  }


  vk::NameSet VrInstance::queryInstanceExtensions() const {
    uint32_t len = m_compositor->GetVulkanInstanceExtensionsRequired(nullptr, 0);
    std::vector<char> extensionList(len);
    len = m_compositor->GetVulkanInstanceExtensionsRequired(extensionList.data(), len);
    return parseExtensionList(std::string(extensionList.data(), len));
  }
  
  
  vk::NameSet VrInstance::queryDeviceExtensions(VkPhysicalDevice adapter) const {
    uint32_t len = m_compositor->GetVulkanDeviceExtensionsRequired(adapter, nullptr, 0);
    std::vector<char> extensionList(len);
    len = m_compositor->GetVulkanDeviceExtensionsRequired(adapter, extensionList.data(), len);
    return parseExtensionList(std::string(extensionList.data(), len));
  }
  
  
  vk::NameSet VrInstance::parseExtensionList(const std::string& str) const {
    vk::NameSet result;
    
    std::stringstream strstream(str);
    std::string       section;
    
    while (std::getline(strstream, section, ' '))
      result.add(section);
    
    return result;
  }
  
  
  vr::IVRCompositor* VrInstance::getCompositor() {
    // Locate the OpenVR DLL if loaded by the process
    HMODULE ovrApi = ::GetModuleHandle("openvr_api.dll");
    
    if (ovrApi == nullptr) {
      Logger::warn("OpenVR: Failed to locate module");
      return nullptr;
    }
    
    // Load method used to retrieve the IVRCompositor interface
    g_vrFunctions.initInternal        = reinterpret_cast<VR_InitInternalProc>       (::GetProcAddress(ovrApi, "VR_InitInternal"));
    g_vrFunctions.shutdownInternal    = reinterpret_cast<VR_ShutdownInternalProc>   (::GetProcAddress(ovrApi, "VR_ShutdownInternal"));
    g_vrFunctions.getGenericInterface = reinterpret_cast<VR_GetGenericInterfaceProc>(::GetProcAddress(ovrApi, "VR_GetGenericInterface"));
    
    if (g_vrFunctions.getGenericInterface == nullptr) {
      Logger::warn("OpenVR: VR_GetGenericInterface not found");
      return nullptr;
    }
    
    // Retrieve the compositor interface
    vr::EVRInitError error = vr::VRInitError_None;
    
    vr::IVRCompositor* compositor = reinterpret_cast<vr::IVRCompositor*>(
      g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
    
    if (error != vr::VRInitError_None || compositor == nullptr) {
      if (g_vrFunctions.initInternal     == nullptr
       || g_vrFunctions.shutdownInternal == nullptr) {
        Logger::warn("OpenVR: VR_InitInternal or VR_ShutdownInternal not found");
        return nullptr;
      }

      // If the app has not initialized OpenVR yet, we need
      // to do it now in order to grab a compositor instance
      g_vrFunctions.initInternal(&error, vr::VRApplication_Background);
      m_initializedOpenVr = error == vr::VRInitError_None;

      if (error != vr::VRInitError_None) {
        Logger::warn("OpenVR: Failed to initialize OpenVR");
        return nullptr;
      }

      compositor = reinterpret_cast<vr::IVRCompositor*>(
        g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
      
      if (error != vr::VRInitError_None || compositor == nullptr) {
        Logger::warn("OpenVR: Failed to query compositor interface");
        g_vrFunctions.shutdownInternal();
        m_initializedOpenVr = false;
        return nullptr;
      }
    }
    
    Logger::info("OpenVR: Compositor interface found");
    return compositor;
  }
  
}