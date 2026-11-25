// This file is part of SolarNX (GPL-3.0).

#include "video_decoder.h"

#include <SDL.h>
#include <media/NdkMediaCodec.h>
#include <string.h>

#include <android/log.h>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SolarNX", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "SolarNX", __VA_ARGS__)

namespace
{

// OMX color formats we understand on the codec's output (buffer mode).
enum
{
    OMX_COLOR_FormatYUV420Planar = 19,
    OMX_COLOR_FormatYUV420SemiPlanar = 0x15, // 21: QCom NV12 family
    OMX_COLOR_FormatYUV420Flexible = 0x7F420888,
    QCOM_COLOR_FormatYUV420SemiPlanar32 = 0x7FA30C00,
};

struct DecodedSlot // written by the core thread, consumed by the render thread
{
    std::mutex m;
    std::vector<uint8_t> pixels; // packed NV12 or I420, visible crop only
    int w = 0, h = 0;
    int format = SDL_PIXELFORMAT_NV12; // or SDL_PIXELFORMAT_IYUV
    uint64_t seq = 0;                // bumped per decoded frame
    bool valid = false;
};

// Vendor quirks: some decoders report YUV420Flexible but actually emit
// semiplanar NV12 in buffer mode (Qualcomm is the known offender).
bool s_vendorSemiplanar;

AMediaCodec *s_codec;
bool s_codecHevc;
bool s_failed; // codec create/config failed - do not retry every frame
int64_t s_ptsUs;
uint32_t s_droppedIn;   // input queue full - frame dropped by the decoder
DecodedSlot s_slot;

// Layout of the codec's output buffer as reported via the MediaFormat.
struct OutLayout
{
    int colorFormat = OMX_COLOR_FormatYUV420Flexible;
    int stride = 0, sliceHeight = 0;
    int cropL = 0, cropT = 0, cropR = -1, cropB = -1; // -1 = absent
    int width = 0, height = 0;
    bool got = false;
} s_layout;

bool fmtInt(AMediaFormat *f, const char *key, int *out)
{
    int32_t v;
    if (AMediaFormat_getInt32(f, key, &v))
    {
        *out = v;
        return true;
    }
    return false;
}

void codec_destroy()
{
    if (s_codec != NULL)
    {
        AMediaCodec_stop(s_codec);
        AMediaCodec_delete(s_codec);
        s_codec = NULL;
    }
}

// Debug overrides read once from /data/local/tmp/snxdec.cfg:
// "name=<codec-name>" forces a specific decoder, "nocsd=1" disables csd.
struct DecCfg { std::string name; bool nocsd = false; };
const DecCfg &dec_cfg()
{
    static DecCfg c;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (FILE *f = fopen("/data/local/tmp/snxdec.cfg", "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                if (strncmp(line, "name=", 5) == 0)
                {
                    c.name = line + 5;
                    while (!c.name.empty() &&
                           (c.name.back() == '\n' || c.name.back() == '\r'))
                        c.name.pop_back();
                }
                else if (strncmp(line, "nocsd=1", 7) == 0)
                    c.nocsd = true;
            }
            fclose(f);
            if (!c.name.empty() || c.nocsd)
                LOGI("decoder cfg: name=%s nocsd=%d",
                     c.name.c_str(), (int)c.nocsd);
        }
    }
    return c;
}

// Walks Annex-B start codes in an access unit and collects the codec-config
// NALs: H.264 SPS -> csd0, PPS -> csd1 (raw NAL payloads); HEVC VPS/SPS/PPS
// concatenated -> csd0, kept in bitstream form WITH start codes (the C2 HEVC
// stack parses csd-0 as an Annex-B chunk - bare NAL payloads silently stall
// both the Qualcomm and the software decoders).
// Passing these via the MediaFormat is what Qualcomm/MTK decoders need before
// they produce output (inline headers alone stall many chipsets).
bool extract_csd(const uint8_t *data, size_t len, bool hevc,
                 std::vector<uint8_t> &csd0, std::vector<uint8_t> &csd1)
{
    csd0.clear();
    csd1.clear();
    size_t i = 0;
    while (i + 4 <= len)
    {
        // Find a start code (00 00 01 or 00 00 00 01).
        if (!(data[i] == 0 && data[i + 1] == 0 &&
              (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < len && data[i + 3] == 1))))
        {
            i++;
            continue;
        }
        size_t nal = i + (data[i + 2] == 1 ? 3 : 4);
        // NAL ends at the next start code or the AU end.
        size_t j = nal;
        while (j + 3 < len &&
               !(data[j] == 0 && data[j + 1] == 0 &&
                 (data[j + 2] == 1 || (data[j + 2] == 0 && j + 4 < len && data[j + 3] == 1))))
            j++;
        size_t end = (j + 3 < len) ? j : len;
        int type = hevc ? (data[nal] >> 1) & 0x3F : data[nal] & 0x1F;
        if (!hevc)
        {
            if (type == 7)
                csd0.assign(data + nal, data + end);
            else if (type == 8)
                csd1.assign(data + nal, data + end);
        }
        else if (type == 32 || type == 33 || type == 34)
            csd0.insert(csd0.end(), data + i, data + end);
        i = end;
    }
    return !csd0.empty();
}

bool codec_create(bool hevc, const std::vector<uint8_t> &csd0,
                  const std::vector<uint8_t> &csd1)
{
    codec_destroy();
    const char *mime = hevc ? "video/hevc" : "video/avc";
    // Debug override: force a specific codec (e.g. c2.android.hevc.decoder).
    const DecCfg &cfg = dec_cfg();
    s_codec = !cfg.name.empty() ? AMediaCodec_createCodecByName(cfg.name.c_str())
                              : AMediaCodec_createDecoderByType(mime);
    if (s_codec == NULL)
    {
        LOGW("no decoder for %s", mime);
        return false;
    }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    // Nominal size; the real one arrives via INFO_OUTPUT_FORMAT_CHANGED.
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          OMX_COLOR_FormatYUV420Flexible);
    AMediaFormat_setInt32(f, "low-latency", 1); // Qualcomm: shrink output delay
    AMediaFormat_setInt32(f, "priority", 0);    // realtime (key constant is API 28+)
    // Debug override: skip csd entirely (rely on in-band config).
    if (!cfg.nocsd)
    {
        if (!csd0.empty())
            AMediaFormat_setBuffer(f, "csd-0", csd0.data(), csd0.size());
        if (!csd1.empty())
            AMediaFormat_setBuffer(f, "csd-1", csd1.data(), csd1.size());
    }
    media_status_t st = AMediaCodec_configure(s_codec, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(s_codec) != AMEDIA_OK)
    {
        LOGW("codec configure/start failed: %d", st);
        codec_destroy();
        return false;
    }
    s_codecHevc = hevc;
    s_layout = OutLayout{};
    std::string name = "?";
    // getName/releaseName are API 28+ (minSdk is 26): resolve at runtime instead.
    typedef media_status_t (*CodecNameFn)(AMediaCodec *, char **);
    typedef media_status_t (*CodecReleaseFn)(AMediaCodec *, char *);
    CodecNameFn getName = (CodecNameFn)dlsym(RTLD_DEFAULT, "AMediaCodec_getName");
    CodecReleaseFn releaseName = (CodecReleaseFn)dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName");
    char *rawName = NULL;
    if (getName != NULL && getName(s_codec, &rawName) == AMEDIA_OK && rawName != NULL)
    {
        name = rawName;
        s_vendorSemiplanar = strstr(rawName, "qti") != NULL ||
                             strstr(rawName, "qcom") != NULL ||
                             strstr(rawName, "QTI") != NULL ||
                             strstr(rawName, "OMX.qcom") != NULL ||
                             strstr(rawName, "exynos") != NULL ||
                             strstr(rawName, "mtk") != NULL ||
                             strstr(rawName, "MTK") != NULL;
        if (releaseName != NULL)
            releaseName(s_codec, rawName);
    }
    LOGI("decoder started: %s [%s] (csd %zu+%zu bytes)", mime, name.c_str(),
         csd0.size(), csd1.size());
    return true;
}

void on_output_format()
{
    AMediaFormat *f = AMediaCodec_getOutputFormat(s_codec);
    OutLayout l;
    fmtInt(f, AMEDIAFORMAT_KEY_WIDTH, &l.width);
    fmtInt(f, AMEDIAFORMAT_KEY_HEIGHT, &l.height);
    fmtInt(f, "color-format", &l.colorFormat);
    fmtInt(f, "stride", &l.stride);
    fmtInt(f, "slice-height", &l.sliceHeight);
    // Crop rectangle (optional): defaults cover the whole frame.
    int cl = 0, ct = 0, cr = -1, cb = -1;
    fmtInt(f, "crop-left", &cl);
    fmtInt(f, "crop-top", &ct);
    fmtInt(f, "crop-right", &cr);
    fmtInt(f, "crop-bottom", &cb);
    l.cropL = cl;
    l.cropT = ct;
    l.cropR = cr;
    l.cropB = cb;
    if (l.stride <= 0)
        l.stride = l.width;
    if (l.sliceHeight <= 0)
        l.sliceHeight = l.height;
    l.got = true;
    s_layout = l;
    AMediaFormat_delete(f);
    LOGI("codec out: %dx%d stride=%d slice=%d color=0x%x crop=%d,%d-%d,%d",
         l.width, l.height, l.stride, l.sliceHeight, l.colorFormat,
         l.cropL, l.cropT, l.cropR, l.cropB);
}

// Copies the codec's YUV output into the packed slot buffer. Handles planar
// (I420) and semiplanar (NV12) layouts with stride/slice-height/crop honored.
void copy_output_to_slot(const uint8_t *buf, size_t size)
{
    const OutLayout &l = s_layout;
    int visW = l.cropR >= 0 ? l.cropR - l.cropL + 1 : l.width;
    int visH = l.cropB >= 0 ? l.cropB - l.cropT + 1 : l.height;
    if (visW <= 0 || visH <= 0 || visW > 4096 || visH > 4096)
        return;
    bool semiplanar = l.colorFormat == OMX_COLOR_FormatYUV420SemiPlanar ||
                      l.colorFormat == QCOM_COLOR_FormatYUV420SemiPlanar32 ||
                      s_vendorSemiplanar; // vendor "flexible" is really NV12
    // Per the MediaCodec contract, true flexible output is planar 4:2:0.
    size_t need = (size_t)visW * visH * 3 / 2;

    std::lock_guard<std::mutex> g(s_slot.m);
    if (s_slot.pixels.size() < need)
        s_slot.pixels.resize(need);

    const uint8_t *yPlane = buf + l.cropT * l.stride + l.cropL;
    uint8_t *dst = s_slot.pixels.data();
    for (int r = 0; r < visH; r++)
        memcpy(dst + (size_t)r * visW, yPlane + (size_t)r * l.stride, (size_t)visW);

    const int cw = visW / 2, ch = visH / 2;
    const uint8_t *uvBase = buf + (size_t)l.stride * l.sliceHeight;
    if (semiplanar)
    {
        const uint8_t *uvPlane = uvBase + (l.cropT / 2) * l.stride + (l.cropL & ~1);
        uint8_t *uv = dst + (size_t)visW * visH;
        for (int r = 0; r < ch; r++)
            memcpy(uv + (size_t)r * visW, uvPlane + (size_t)r * l.stride, (size_t)cw * 2);
        s_slot.format = SDL_PIXELFORMAT_NV12;
    }
    else
    {
        int chromaStride = l.stride / 2;
        const uint8_t *uPlane = uvBase + (l.cropT / 2) * chromaStride + l.cropL / 2;
        const uint8_t *vPlane = uvBase + (size_t)chromaStride * (l.sliceHeight / 2) +
                                (l.cropT / 2) * chromaStride + l.cropL / 2;
        uint8_t *u = dst + (size_t)visW * visH;
        uint8_t *v = u + (size_t)cw * ch;
        for (int r = 0; r < ch; r++)
        {
            memcpy(u + (size_t)r * cw, uPlane + (size_t)r * chromaStride, (size_t)cw);
            memcpy(v + (size_t)r * cw, vPlane + (size_t)r * chromaStride, (size_t)cw);
        }
        s_slot.format = SDL_PIXELFORMAT_IYUV;
    }
    s_slot.w = visW;
    s_slot.h = visH;
    s_slot.valid = true;
    s_slot.seq++;
}

void drain_outputs()
{
    for (;;)
    {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(s_codec, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            on_output_format();
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (idx < 0)
            return;
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getOutputBuffer(s_codec, (size_t)idx, &cap);
        if (buf != NULL && info.size > 0 && s_layout.got)
            copy_output_to_slot(buf + info.offset, (size_t)info.size);
        AMediaCodec_releaseOutputBuffer(s_codec, (size_t)idx, false);
    }
}

} // namespace

void snx_video_decoder_start(void)
{
    s_failed = false;
    s_droppedIn = 0;
    s_ptsUs = 0;
    std::lock_guard<std::mutex> g(s_slot.m);
    s_slot.valid = false;
    s_slot.seq = 0;
}

void snx_video_decoder_stop(void)
{
    codec_destroy();
    s_failed = false;
}

void snx_video_decoder_reset(void)
{
    codec_destroy(); // a new session gets fresh csd on the next keyframe anyway
    std::lock_guard<std::mutex> g(s_slot.m);
    s_slot.valid = false;
}

void snx_video_decoder_feed(const uint8_t *data, size_t len, bool keyframe, bool hevc)
{
    if (s_failed)
        return;
    if (s_codec == NULL || s_codecHevc != hevc)
    {
        // The decoder is (re)created only on a keyframe: that is where the
        // codec-config NALs live, and deltas are undecodable anyway.
        if (!keyframe)
            return;
        static std::vector<uint8_t> csd0, csd1;
        extract_csd(data, len, hevc, csd0, csd1);
        LOGI("keyframe feed: len=%zu head=%02x%02x%02x%02x%02x csd=%zu+%zu", len,
             data[0], data[1], data[2], data[3], data[4], csd0.size(), csd1.size());
        if (!codec_create(hevc, csd0, csd1))
        {
            s_failed = true;
            return;
        }
    }

    // Queue the frame. A keyframe is worth a short wait for an input slot:
    // dropping it stalls the stream until the next one.
    ssize_t in = AMediaCodec_dequeueInputBuffer(s_codec, keyframe ? 30000 : 0);
    if (in >= 0)
    {
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getInputBuffer(s_codec, (size_t)in, &cap);
        if (buf != NULL && len <= cap)
        {
            memcpy(buf, data, len);
            s_ptsUs += 33333;
            media_status_t q = AMediaCodec_queueInputBuffer(
                s_codec, (size_t)in, 0, len, (int64_t)s_ptsUs, 0);
            if (q != AMEDIA_OK)
            {
                // Codec died (bad bitstream, driver error): drop it so the
                // next keyframe recreates a fresh session.
                LOGW("queueInputBuffer failed: %d - recreating codec", q);
                codec_destroy();
                s_layout = OutLayout{};
                return;
            }
        }
        else
        {
            LOGW("input buffer too small: %zu needed %zu", len, cap);
            AMediaCodec_queueInputBuffer(s_codec, (size_t)in, 0, 0, 0, 0);
        }
    }
    else if (++s_droppedIn < 20 || s_droppedIn % 100 == 0)
        LOGW("decoder input full, frame dropped (total %u)", s_droppedIn);

    drain_outputs();
}

void snx_video_decoder_draw(void *opaque, SDL_Renderer *renderer)
{
    SDL_Texture **tex = (SDL_Texture **)opaque;
    std::lock_guard<std::mutex> g(s_slot.m);
    if (!s_slot.valid)
        return;
    static int texW, texH;
    static Uint32 texFmt;
    if (*tex == NULL || texW != s_slot.w || texH != s_slot.h ||
        texFmt != (Uint32)s_slot.format)
    {
        if (*tex != NULL)
            SDL_DestroyTexture(*tex);
        *tex = SDL_CreateTexture(renderer, (Uint32)s_slot.format,
                                 SDL_TEXTUREACCESS_STREAMING, s_slot.w, s_slot.h);
        texW = s_slot.w;
        texH = s_slot.h;
        texFmt = (Uint32)s_slot.format;
        if (*tex == NULL)
            return;
    }
    const uint8_t *p = s_slot.pixels.data();
    if (s_slot.format == SDL_PIXELFORMAT_NV12)
        SDL_UpdateNVTexture(*tex, NULL, p, s_slot.w, p + (size_t)s_slot.w * s_slot.h,
                            s_slot.w);
    else
        SDL_UpdateYUVTexture(*tex, NULL, p, s_slot.w,
                             p + (size_t)s_slot.w * s_slot.h, s_slot.w / 2,
                             p + (size_t)s_slot.w * s_slot.h + (size_t)(s_slot.w / 2) * (s_slot.h / 2),
                             s_slot.w / 2);

    int rw, rh;
    SDL_GetRendererOutputSize(renderer, &rw, &rh);
    float scale = SDL_min((float)rw / s_slot.w, (float)rh / s_slot.h);
    SDL_Rect dst = {(int)((rw - s_slot.w * scale) / 2),
                    (int)((rh - s_slot.h * scale) / 2),
                    (int)(s_slot.w * scale), (int)(s_slot.h * scale)};
    SDL_RenderCopy(renderer, *tex, NULL, &dst);
}

bool snx_video_decoder_active(void)
{
    std::lock_guard<std::mutex> g(s_slot.m);
    return s_slot.valid;
}
