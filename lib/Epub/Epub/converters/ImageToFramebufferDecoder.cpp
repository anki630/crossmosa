#include <cstdarg>
#include <cstdio>
#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <Logging.h>

char ImageToFramebufferDecoder::lastError[64] = "";
bool ImageToFramebufferDecoder::lastErrorTransient = false;
void ImageToFramebufferDecoder::setLastError(const bool transient, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(lastError, sizeof(lastError), fmt, ap);
  va_end(ap);
  lastErrorTransient = transient;
}

bool ImageToFramebufferDecoder::validateAndStoreDimensions(const int64_t width, const int64_t height,
                                                           ImageDimensions& out, const char* format,
                                                           const bool applyPixelCap) {
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid %s dimensions: %lldx%lld", format, static_cast<long long>(width),
            static_cast<long long>(height));
    setLastError(false, "%s-dims %lldx%lld", format, static_cast<long long>(width), static_cast<long long>(height));
    return false;
  }
  if (width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION) {
    LOG_ERR("IMG", "%s dimensions exceed supported limit: %lldx%lld (max %lld per dimension)", format,
            static_cast<long long>(width), static_cast<long long>(height), static_cast<long long>(MAX_SOURCE_DIMENSION));
    setLastError(false, "%s-axis %lldx%lld", format, static_cast<long long>(width), static_cast<long long>(height));
    return false;
  }
  const int64_t pixels = width * height;
  if (applyPixelCap && pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "%s too large (%lldx%lld = %lld pixels), max supported: %lld pixels", format,
            static_cast<long long>(width), static_cast<long long>(height), static_cast<long long>(pixels),
            static_cast<long long>(MAX_SOURCE_PIXELS));
    setLastError(false, "px-cap %lldx%lld", static_cast<long long>(width), static_cast<long long>(height));
    return false;
  }
  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}

void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t& lastYieldMs) {
  const uint32_t now = millis();
  if (now - lastYieldMs >= 250) {
    lastYieldMs = now;
    vTaskDelay(1);
  }
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
