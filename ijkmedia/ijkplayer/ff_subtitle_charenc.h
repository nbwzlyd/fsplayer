/*
 * ff_subtitle_charenc.h
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

#ifndef FF_SUBTITLE_CHARENC_H
#define FF_SUBTITLE_CHARENC_H

#include <stddef.h>
#include <stdio.h>

// 探测外挂字幕文件编码（读前 16KB 做 BOM 嗅探 + 严格 UTF-8 校验）。
// 返回 1 且 out 填入了 iconv 编码名（"GBK"/"UTF-16LE"/"UTF-16BE"）时，
// 调用方应把它设为解码器 sub_charenc；返回 0 表示是合法 UTF-8/带 UTF-8 BOM，
// 无需设置；负数=打不开文件或参数错误。
int ff_sub_detect_external_charenc(const char *file_name, char *out, size_t out_len);

#endif
