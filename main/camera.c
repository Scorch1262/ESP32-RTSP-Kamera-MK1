/*
 * OV5640 ueber MIPI-CSI (2 Lanes) mit esp_video (V4L2-API).
 *
 * Ziel-Format fuer den H.264-Hardware-Encoder ist YUV420 im P4-eigenen
 * Layout O_UYY_E_VYY (= V4L2_PIX_FMT_YUV420 bei esp_video).
 *  - Neuere P4-Revisionen: CSI-Bridge wandelt RGB565 -> YUV420 selbst (zero copy).
 *  - Aeltere Revisionen:   RGB565 holen und per PPA (2D-DMA) nach YUV420 wandeln.
 */
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "camera.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "sdkconfig.h"

static const char *TAG = "camera";

#define CAM_BUF_COUNT   3
#define YUV_ALIGN       128

static int s_fd = -1;
static uint8_t *s_buf[CAM_BUF_COUNT];
static size_t s_buf_len[CAM_BUF_COUNT];
static camera_info_t s_info;

/* Nur im PPA-Modus (Farbwandlung und/oder Drehung) */
static ppa_client_handle_t s_ppa;
static uint32_t s_src_w, s_src_h;              /* Groesse des Kamerabildes (vor der Drehung) */
static ppa_srm_color_mode_t s_src_cm;          /* Format des Kamerabildes */
static ppa_srm_rotation_angle_t s_ppa_angle;   /* PPA dreht gegen den Uhrzeigersinn */
static uint8_t *s_yuv;
static size_t s_yuv_alloc;

static const esp_video_init_csi_config_t s_csi_config = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port    = CONFIG_APP_CAM_SCCB_PORT,
            .scl_pin = CONFIG_APP_CAM_SCCB_SCL_GPIO,
            .sda_pin = CONFIG_APP_CAM_SCCB_SDA_GPIO,
        },
        .freq = CONFIG_APP_CAM_SCCB_FREQ,
    },
    .reset_pin = CONFIG_APP_CAM_RESET_GPIO,
    .pwdn_pin  = CONFIG_APP_CAM_PWDN_GPIO,
};

static const esp_video_init_config_t s_video_config = {
    .csi = &s_csi_config,
};

static const char *fourcc_str(uint32_t f, char out[5])
{
    out[0] = f & 0xFF; out[1] = (f >> 8) & 0xFF; out[2] = (f >> 16) & 0xFF; out[3] = (f >> 24) & 0xFF; out[4] = 0;
    return out;
}

static esp_err_t try_set_format(uint32_t w, uint32_t h, uint32_t pixfmt)
{
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = pixfmt;
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        return ESP_FAIL;
    }
    return (fmt.fmt.pix.pixelformat == pixfmt) ? ESP_OK : ESP_FAIL;
}

esp_err_t camera_init(camera_info_t *info, int rotation)
{
    char fcc[5];

    ESP_RETURN_ON_ERROR(esp_video_init(&s_video_config), TAG, "esp_video_init fehlgeschlagen - Kamera angeschlossen? SCCB-Pins?");

    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG, "%s laesst sich nicht oeffnen", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    struct v4l2_capability cap = {0};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "Treiber: %s  Karte: %s", cap.driver, cap.card);
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "VIDIOC_G_FMT");
    uint32_t w = fmt.fmt.pix.width;
    uint32_t h = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "Sensor-Standardformat: %"PRIu32"x%"PRIu32" %s", w, h, fourcc_str(fmt.fmt.pix.pixelformat, fcc));

    ESP_RETURN_ON_FALSE((w % 16) == 0 && (h % 16) == 0, ESP_ERR_NOT_SUPPORTED, TAG,
                        "H.264-HW-Encoder braucht Breite/Hoehe als Vielfaches von 16");

    rotation = ((rotation % 360) + 360) % 360;
    bool direct_yuv = (try_set_format(w, h, V4L2_PIX_FMT_YUV420) == ESP_OK);
    if (!direct_yuv) {
        ESP_RETURN_ON_ERROR(try_set_format(w, h, V4L2_PIX_FMT_RGB565), TAG, "Weder YUV420 noch RGB565 einstellbar");
    }
    s_src_w = w;
    s_src_h = h;
    s_src_cm = direct_yuv ? PPA_SRM_COLOR_MODE_YUV420 : PPA_SRM_COLOR_MODE_RGB565;
    /* Drehung im Uhrzeigersinn -> PPA-Winkel gegen den Uhrzeigersinn */
    s_ppa_angle = rotation == 90  ? PPA_SRM_ROTATION_ANGLE_270 :
                  rotation == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                  rotation == 270 ? PPA_SRM_ROTATION_ANGLE_90  : PPA_SRM_ROTATION_ANGLE_0;

    s_info.zero_copy = direct_yuv && rotation == 0;
    if (s_info.zero_copy) {
        ESP_LOGI(TAG, "CSI liefert direkt YUV420 (zero copy)");
    } else {
        ESP_LOGI(TAG, "PPA: %s -> YUV420%s", direct_yuv ? "YUV420" : "RGB565",
                 rotation ? ", Drehung" : "");
        if (rotation) {
            ESP_LOGI(TAG, "Bild wird um %d Grad im Uhrzeigersinn gedreht", rotation);
        }
        ppa_client_config_t pcfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        ESP_RETURN_ON_ERROR(ppa_register_client(&pcfg, &s_ppa), TAG, "PPA-Client");
    }

    bool swap = (rotation == 90 || rotation == 270);
    s_info.width = swap ? h : w;
    s_info.height = swap ? w : h;
    s_info.yuv_size = w * h * 3 / 2;

    if (!s_info.zero_copy) {
        s_yuv_alloc = (s_info.yuv_size + YUV_ALIGN - 1) & ~(YUV_ALIGN - 1);
        s_yuv = heap_caps_aligned_calloc(YUV_ALIGN, 1, s_yuv_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_yuv, ESP_ERR_NO_MEM, TAG, "YUV-Puffer");
    }

    struct v4l2_requestbuffers req = {0};
    req.count = CAM_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "VIDIOC_REQBUFS");

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "VIDIOC_QUERYBUF");
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_buf[i] && s_buf[i] != MAP_FAILED, ESP_FAIL, TAG, "mmap");
        s_buf_len[i] = buf.length;
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "VIDIOC_QBUF");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "VIDIOC_STREAMON");

    ESP_LOGI(TAG, "Kamera laeuft: %"PRIu32"x%"PRIu32, w, h);
    *info = s_info;
    return ESP_OK;
}

esp_err_t camera_get_frame(camera_frame_t *frame)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_DQBUF: errno %d", errno);
        return ESP_FAIL;
    }
    frame->timestamp_us = esp_timer_get_time();

    if (s_info.zero_copy) {
        frame->data = s_buf[buf.index];
        frame->len = s_info.yuv_size;
        frame->v4l2_index = buf.index;
        return ESP_OK;
    }

    /* RGB565/YUV420 -> YUV420 (O_UYY_E_VYY) per PPA, ggf. gedreht */
    ppa_srm_oper_config_t srm = {0};
    srm.in.buffer = s_buf[buf.index];
    srm.in.pic_w = s_src_w;
    srm.in.pic_h = s_src_h;
    srm.in.block_w = s_src_w;
    srm.in.block_h = s_src_h;
    srm.in.srm_cm = s_src_cm;
    srm.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    srm.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    srm.out.buffer = s_yuv;
    srm.out.buffer_size = s_yuv_alloc;
    srm.out.pic_w = s_info.width;
    srm.out.pic_h = s_info.height;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    srm.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
    srm.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    srm.rotation_angle = s_ppa_angle;
    srm.scale_x = 1.0f;
    srm.scale_y = 1.0f;
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);

    /* Kamerapuffer sofort zurueckgeben */
    ioctl(s_fd, VIDIOC_QBUF, &buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA-Wandlung fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    frame->data = s_yuv;
    frame->len = s_info.yuv_size;
    frame->v4l2_index = -1;
    return ESP_OK;
}

void camera_put_frame(camera_frame_t *frame)
{
    if (frame->v4l2_index < 0) {
        return;
    }
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = frame->v4l2_index;
    if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QBUF: errno %d", errno);
    }
    frame->v4l2_index = -1;
}
