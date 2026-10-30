/*! \brief Camera viewer: shows the live image of a PS3 Eye camera in a window (Windows only for now). */

#include <ps3eye.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> sIsRunning{true};

const wchar_t sWindowClass[] = L"pseyosc-video";

/*! \brief A window that shows the newest frame, scaled to its size.

    setFrame() may be called from any thread; drawing happens on the thread that created the window.
 */
class CameraWindow
{
public:
    CameraWindow(int width, int height)
        : mPixels(static_cast<size_t>(width) * height * 3)
    {
        mInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mInfo.bmiHeader.biWidth = width;
        mInfo.bmiHeader.biHeight = -height; // top-down rows, as the camera delivers them
        mInfo.bmiHeader.biPlanes = 1;
        mInfo.bmiHeader.biBitCount = 24; // BGR
        mInfo.bmiHeader.biCompression = BI_RGB;
    }

    bool create()
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &CameraWindow::windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = sWindowClass;
        RegisterClassExW(&windowClass);

        // The client area starts at the frame's size.
        RECT rect = {0, 0, mInfo.bmiHeader.biWidth, -mInfo.bmiHeader.biHeight};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        mWindow = CreateWindowExW(0, sWindowClass, L"pseyosc-video", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  windowClass.hInstance, this);
        if (!mWindow)
            return false;
        ShowWindow(mWindow, SW_SHOW);
        return true;
    }

    void setTitle(const std::string &title) { SetWindowTextA(mWindow, title.c_str()); }

    /*! \brief Copies \a bgr (width * height * 3 bytes) and schedules a repaint. */
    void setFrame(const uint8_t *bgr)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            std::memcpy(mPixels.data(), bgr, mPixels.size());
        }
        InvalidateRect(mWindow, nullptr, FALSE);
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto *self = reinterpret_cast<CameraWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case WM_PAINT:
            if (self) {
                self->paint(window);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1; // the frame covers the whole client area
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paint;
        const HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        SetStretchBltMode(dc, COLORONCOLOR);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, mInfo.bmiHeader.biWidth,
                          -mInfo.bmiHeader.biHeight, mPixels.data(), &mInfo, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
    }

    HWND mWindow = nullptr;
    BITMAPINFO mInfo = {};
    std::mutex mMutex; //!< guards mPixels
    std::vector<uint8_t> mPixels;
};

void printUsage()
{
    std::printf("usage: pseyosc-video [--camera <index>] [--resolution 640x480|320x240] [--fps <hz>]\n"
                "  --camera <index>     which camera to open, if several are connected (default 0)\n"
                "  --resolution <size>  640x480 (default) or 320x240\n"
                "  --fps <hz>           frame rate (default 60); the camera uses the next lower rate it supports,\n"
                "                       up to 75 at 640x480 and 187 at 320x240\n");
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int cameraIndex = 0;
    int width = 640;
    int height = 480;
    int fps = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            cameraIndex = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--resolution") == 0 && i + 1 < argc) {
            const std::string resolution = argv[++i];
            if (resolution == "320x240") {
                width = 320;
                height = 240;
            } else if (resolution != "640x480") {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = std::atoi(argv[++i]);
            if (fps <= 0) {
                printUsage();
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }

    // PS3EYEDriver also offers higher rates, but notes that their video is partly corrupt.
    fps = std::min(fps, width == 640 ? 75 : 187);

    std::signal(SIGINT, [](int) { sIsRunning = false; });
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break

    // Only cameras libusb can open are listed: on Windows, that needs the WinUSB driver.
    const std::vector<ps3eye::PS3EYECam::PS3EYERef> &cameras = ps3eye::PS3EYECam::getDevices();
    if (cameras.empty()) {
        std::fprintf(stderr, "no PS3 Eye camera found; on Windows, it needs the WinUSB driver on its video\n"
                             "interface (Zadig: \"USB Camera-B4.09.24.1 (Interface 0)\"), see the README\n");
        return 1;
    }
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(cameras.size())) {
        std::fprintf(stderr, "no camera %d: %d connected\n", cameraIndex, static_cast<int>(cameras.size()));
        return 1;
    }
    const ps3eye::PS3EYECam::PS3EYERef camera = cameras[cameraIndex];
    if (!camera->init(width, height, static_cast<uint16_t>(fps), ps3eye::PS3EYECam::EOutputFormat::BGR)) {
        std::fprintf(stderr, "could not open camera %d\n", cameraIndex);
        return 1;
    }
    // A normal picture: automatic gain, exposure and white balance (tracking will need a dark, fixed exposure).
    camera->setAutogain(true);
    camera->setAutoWhiteBalance(true);
    camera->start();
    const int actualFps = camera->getFrameRate();
    std::printf("camera %d: %dx%d @ %d Hz (Esc or Ctrl+C to quit)\n", cameraIndex, width, height, actualFps);

    CameraWindow window(width, height);
    if (!window.create()) {
        std::fprintf(stderr, "could not create the window\n");
        camera->stop();
        return 1;
    }

    std::atomic<int> frameCount{0};
    std::thread capture([&]() {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3);
        while (sIsRunning) {
            camera->getFrame(frame.data()); // blocks until the next frame
            window.setFrame(frame.data());
            ++frameCount;
        }
    });

    // The window's message loop, which also checks for Ctrl+C and shows the measured frame rate in the title.
    DWORD lastTitleUpdate = GetTickCount();
    while (sIsRunning) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                sIsRunning = false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const DWORD now = GetTickCount();
        if (now - lastTitleUpdate >= 1000) {
            const double measuredFps = frameCount.exchange(0) * 1000.0 / (now - lastTitleUpdate);
            lastTitleUpdate = now;
            char title[96];
            std::snprintf(title, sizeof(title), "pseyosc-video: camera %d, %dx%d, %.1f fps", cameraIndex, width,
                          height, measuredFps);
            window.setTitle(title);
        }
    }

    capture.join(); // returns with the next frame
    camera->stop();
    return 0;
}
