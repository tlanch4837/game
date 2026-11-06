// Sequential video decoder on Windows Media Foundation. Frames come out upright, top-down, BGRA.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tbd {

class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    bool Open(const wchar_t* path);

    // Live capture: names of the video capture devices, and opening one of them (frames are the biggest mode
    // up to `maxWidth` pixels wide at 25+ fps). A camera has no duration and cannot Seek.
    static std::vector<std::wstring> ListCameras();
    bool OpenCamera(int index, int maxWidth = 1280);
    int Width() const { return width_; }   // after rotation metadata is applied
    int Height() const { return height_; }
    double Fps() const { return fps_; }
    double Duration() const { return duration_; } // seconds, 0 if unknown

    // Jumps so the next ReadFrame returns the frame at (or just after) `timeSec`.
    bool Seek(double timeSec);

    // Decodes the next frame into `bgra` (Width()*Height()*4 bytes). False at end of stream or on error.
    bool ReadFrame(std::vector<uint8_t>& bgra, double& timeSec);

private:
    bool Setup(); // after the reader exists: output format, size, rate, rotation, duration
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int width_ = 0, height_ = 0;
    double fps_ = 30, duration_ = 0;
};

} // namespace tbd
