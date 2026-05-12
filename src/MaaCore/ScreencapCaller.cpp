#include "AsstScreencap.h"

#include <cstring>
#include <filesystem>
#include <memory>

#include "MaaUtils/NoWarningCVMat.hpp"
#include "Common/AsstTypes.h"
#include "Controller/MumuExtras.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"

#ifdef _WIN32
#include "Controller/MaaFwControlUnitInterface.h"
#include "Controller/Win32ControlUnitLoader.h"
#endif

static constexpr AsstSize NullSize = static_cast<AsstSize>(-1);
static constexpr AsstBool AsstTrue = 1;
static constexpr AsstBool AsstFalse = 0;

struct AsstScreencapAPI
{
public:
    virtual ~AsstScreencapAPI() = default;

    bool capture()
    {
        cv::Mat image;
        if (!do_capture(image) || image.empty()) {
            m_image.release();
            return false;
        }

        m_image = image.isContinuous() ? image : image.clone();
        return !m_image.empty();
    }

    const cv::Mat& image() const noexcept { return m_image; }

private:
    virtual bool do_capture(cv::Mat& image) = 0;

    cv::Mat m_image;
};

#if ASST_WITH_EMULATOR_EXTRAS
class MumuScreencapAPI final : public AsstScreencapAPI
{
public:
    MumuScreencapAPI(const std::filesystem::path& mumu_path, int mumu_index, const char* package_name)
    {
        if (package_name) {
            m_mumu.set_package_name(package_name);
        }
        m_inited = m_mumu.init(mumu_path, mumu_index);
    }

    ~MumuScreencapAPI() override = default;

    bool inited() const noexcept { return m_inited; }

private:
    bool do_capture(cv::Mat& image) override
    {
        auto image_opt = m_mumu.screencap();
        if (!image_opt) {
            return false;
        }

        image = std::move(*image_opt);
        return true;
    }

    asst::MumuExtras m_mumu;
    bool m_inited = false;
};
#endif

#ifdef _WIN32
class Win32ScreencapAPI final : public AsstScreencapAPI
{
public:
    Win32ScreencapAPI(void* hwnd, asst::Win32ScreencapMethod screencap_method, const char* control_unit_dll_path) :
        m_loader(std::make_unique<asst::Win32ControlUnitLoader>())
    {
        std::filesystem::path dll_path = "MaaWin32ControlUnit";
        if (control_unit_dll_path && control_unit_dll_path[0] != '\0') {
            dll_path = asst::utils::path(control_unit_dll_path);
        }

        if (!m_loader->load(dll_path)) {
            LogError << "Failed to load Win32 control unit" << dll_path;
            return;
        }

        m_unit_handle = m_loader->create(hwnd, screencap_method, asst::Win32Input::None, asst::Win32Input::None);
        if (!m_unit_handle) {
            LogError << "Failed to create Win32 control unit" << hwnd << screencap_method;
            return;
        }

        auto* unit = static_cast<asst::MaaFwControlUnitAPI*>(m_unit_handle);
        if (!unit->connect()) {
            LogError << "Failed to connect Win32 control unit" << hwnd << screencap_method;
            m_loader->destroy(m_unit_handle);
            m_unit_handle = nullptr;
            return;
        }

        m_inited = true;
    }

    ~Win32ScreencapAPI() override
    {
        if (m_unit_handle && m_loader) {
            m_loader->destroy(m_unit_handle);
            m_unit_handle = nullptr;
        }
    }

    bool inited() const noexcept { return m_inited; }

private:
    bool do_capture(cv::Mat& image) override
    {
        auto* unit = static_cast<asst::MaaFwControlUnitAPI*>(m_unit_handle);
        return unit && unit->screencap(image);
    }

    std::unique_ptr<asst::Win32ControlUnitLoader> m_loader;
    void* m_unit_handle = nullptr;
    bool m_inited = false;
};
#endif

AsstScreencapHandle AsstCreateMumuScreencap(const char* mumu_path, int32_t mumu_index, const char* package_name)
{
#if ASST_WITH_EMULATOR_EXTRAS
    if (!mumu_path || mumu_path[0] == '\0') {
        LogError << "Cannot create MuMu screencap backend with empty path";
        return nullptr;
    }

    auto backend = std::make_unique<MumuScreencapAPI>(asst::utils::path(mumu_path), mumu_index, package_name);
    if (!backend->inited()) {
        return nullptr;
    }

    return backend.release();
#else
    LogError << "MuMu screencap backend is not available because ASST_WITH_EMULATOR_EXTRAS is disabled";
    return nullptr;
#endif
}

#ifdef _WIN32
AsstScreencapHandle
    AsstCreateWin32Screencap(void* hwnd, uint64_t screencap_method, const char* control_unit_dll_path)
{
    if (!hwnd) {
        LogError << "Cannot create Win32 screencap backend with empty hwnd";
        return nullptr;
    }

    auto backend = std::make_unique<Win32ScreencapAPI>(
        hwnd,
        static_cast<asst::Win32ScreencapMethod>(screencap_method),
        control_unit_dll_path);
    if (!backend->inited()) {
        return nullptr;
    }

    return backend.release();
}
#endif

void AsstDestroyScreencap(AsstScreencapHandle handle)
{
    delete handle;
}

AsstBool AsstScreencapCapture(AsstScreencapHandle handle)
{
    if (!handle) {
        return AsstFalse;
    }

    return handle->capture() ? AsstTrue : AsstFalse;
}

AsstBool AsstGetScreencapImageInfo(
    AsstScreencapHandle handle,
    int32_t* width,
    int32_t* height,
    int32_t* channels,
    AsstSize* data_size)
{
    if (!handle || !width || !height || !channels || !data_size) {
        return AsstFalse;
    }

    const auto& image = handle->image();
    if (image.empty()) {
        return AsstFalse;
    }

    *width = image.cols;
    *height = image.rows;
    *channels = image.channels();
    *data_size = static_cast<AsstSize>(image.total() * image.elemSize());
    return AsstTrue;
}

AsstSize AsstGetScreencapImage(AsstScreencapHandle handle, void* buff, AsstSize buff_size)
{
    if (!handle || !buff) {
        return NullSize;
    }

    const auto& image = handle->image();
    if (image.empty()) {
        return NullSize;
    }

    const auto data_size = static_cast<AsstSize>(image.total() * image.elemSize());
    if (buff_size < data_size) {
        return NullSize;
    }

    std::memcpy(buff, image.data, data_size);
    return data_size;
}
