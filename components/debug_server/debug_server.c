#include "debug_server.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "camera_ctrl.h"

static const char *TAG = "debug_server";
static const char *BOUNDARY = "123456789000000000000987654321";

static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = camera_ctrl_get_frame();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_ctrl_return_frame(fb);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];
    static const char *STREAM_CONTENT_TYPE =
        "multipart/x-mixed-replace;boundary=123456789000000000000987654321";

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    while (true) {
        fb = camera_ctrl_get_frame();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed in stream");
            res = ESP_FAIL;
            break;
        }
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            BOUNDARY, (unsigned int)fb->len);
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) { res = ESP_FAIL; }
        if (res == ESP_OK && httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) { res = ESP_FAIL; }
        camera_ctrl_return_frame(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
    const char *html =
        "<!DOCTYPE html><html><head><title>Smart Lock Debug</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;}"
        "img{max-width:90%;border:2px solid #444;}h1{color:#0f0;}</style></head>"
        "<body><h1>Smart Lock Camera Debug</h1>"
        "<p>Live stream:</p><img src='/stream'/>"
        "<p><a href='/capture' style='color:#0af;'>Single snapshot</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

esp_err_t debug_server_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &stream_uri);
    ESP_LOGI(TAG, "HTTP debug server started on port 80");
    return ESP_OK;
}