/*
 * ff_subtitle_charenc.c
 *
 * Copyright (c) 2022 debugly <qianlongxu@gmail.com>
 *
 * This file is part of FSPlayer.
 *
 * FSPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * FSPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FSPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Boston, MA 02110-1301 USA
 */

// 外挂字幕编码探测：老字幕（GBK/BIG5）不是合法 UTF-8 时，FFmpeg 文本解码器
// 不会报错，会把字节透传成乱码，do_retry_next_charenc 的报错重试链永远不会触发。
// 这里在读文件头做 UTF-8 严格校验，非 UTF-8 才交给调用方下 sub_charenc，
// 保证 libass 拿到的总是正确解码后的文本。

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ff_subtitle_charenc.h"

// 判断 buf 前 len 字节是否为合法 UTF-8（严格模式：拒绝 overlong/surrogate/越界码点）。
// 返回 1=合法 UTF-8，0=不是。空文件/全 ASCII 视为合法。
static int is_valid_utf8(const unsigned char *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = buf[i];
        if (c == 0) {
            // 文本字幕出现 NUL 视为二进制/损坏，交给上层当非 UTF-8 处理
            return 0;
        }
        if (c < 0x80) { // ASCII
            i++;
            continue;
        }
        int need;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { // 2 字节
            need = 1; cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) { // 3 字节
            need = 2; cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) { // 4 字节
            need = 3; cp = c & 0x07;
        } else { // 0x80-0xBF 或 0xF8+ 都是非法首字节
            return 0;
        }
        if (i + need >= len + 1 && i + need > len - 1) {
            // 序列被文件截断：结尾截断按非法处理（老字幕最常见的是 GBK 截在多字节中间）
            if (i + need > len - 1) return 0;
        }
        for (int k = 1; k <= need; k++) {
            unsigned char cc = buf[i + k];
            if ((cc & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // overlong / surrogate / > U+10FFFF
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp > 0x10FFFF)) {
            return 0;
        }
        i += need + 1;
    }
    return 1;
}

// 嗅探 BOM。返回：1=UTF-8 BOM，2=UTF-16LE BOM，3=UTF-16BE BOM，0=无 BOM
static int sniff_bom(const unsigned char *buf, size_t len)
{
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        return 1;
    }
    if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        return 2;
    }
    if (len >= 2 && buf[0] == 0xFE && buf[1] == 0xFF) {
        return 3;
    }
    return 0;
}

int ff_sub_detect_external_charenc(const char *file_name, char *out, size_t out_len)
{
    if (!file_name || !out || out_len < 8) {
        return -1;
    }
    out[0] = '\0';

    FILE *fp = fopen(file_name, "rb");
    if (!fp) {
        return -2;
    }

    unsigned char buf[16 * 1024];
    size_t len = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (len == 0) {
        return 0; // 空文件，不管
    }

    int bom = sniff_bom(buf, len);
    if (bom == 1) {
        return 0; // UTF-8 BOM，无需设置
    }
    if (bom == 2) {
        snprintf(out, out_len, "UTF-16LE");
        return 1;
    }
    if (bom == 3) {
        snprintf(out, out_len, "UTF-16BE");
        return 1;
    }

    // UTF-16 无 BOM：前 1KB 里出现大量 0x00 字节（ASCII 区间），判为 UTF-16
    {
        size_t scan = len < 1024 ? len : 1024;
        size_t zeros = 0;
        for (size_t i = 0; i < scan; i++) {
            if (buf[i] == 0) zeros++;
        }
        if (zeros > scan / 8) {
            // 无 BOM 的 UTF-16 猜 LE（Windows 记事本导出最常见）
            snprintf(out, out_len, "UTF-16LE");
            return 1;
        }
    }

    if (is_valid_utf8(buf, len)) {
        return 0; // 合法 UTF-8，无需设置
    }

    // 非 UTF-8 中文圈老字幕：GBK 覆盖简体绝大多数，BIG5 留给报错重试链兜底
    // （FFmpeg libavutil/charset 转 GB18030 会把 BIG5 显示成乱码，这里沿用
    // ff_subtitle.c 的取舍：先 GBK）
    snprintf(out, out_len, "GBK");
    return 1;
}
