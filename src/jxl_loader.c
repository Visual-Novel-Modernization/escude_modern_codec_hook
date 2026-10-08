#include "jxl_loader.h"
#include <jxl/decode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool IsJxlData(const uint8_t* data, size_t size) {
    if (!data || size < 2) return false;
    // Raw codestream
    if (data[0] == 0xFF && data[1] == 0x0A) return true;
    // Container box format
    if (size >= 12) {
        if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x0C &&
            data[4] == 'J'  && data[5] == 'X'  && data[6] == 'L'  && data[7] == ' ' &&
            data[8] == 0x0D && data[9] == 0x0A && data[10] == 0x87 && data[11] == 0x0A) {
            return true;
        }
    }
    return false;
}

void* DecodeJxlToGameSurface(const uint8_t* data, size_t size, fn_CreateImageBuffer pCreateBuffer) {
    if (!data || size == 0 || !pCreateBuffer) return NULL;

    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return NULL;

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
        JxlDecoderDestroy(dec);
        return NULL;
    }

    JxlDecoderSetInput(dec, data, size);
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info;
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
    void** surface = NULL;
    uint8_t* temp_rgba = NULL;
    size_t temp_size = 0;

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
            break;
        } else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                break;
            }
            // Allocate game surface: width, height, 32bpp
            surface = pCreateBuffer(info.xsize, info.ysize, 32);
            if (!surface) {
                break;
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &temp_size)) {
                break;
            }
            temp_rgba = (uint8_t*)malloc(temp_size);
            if (!temp_rgba) break;

            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, temp_rgba, temp_size)) {
                break;
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            if (surface && surface[1] && temp_rgba) {
                uint8_t* pBits = (uint8_t*)surface[1];
                int stride = (int)(intptr_t)surface[3];

                // Convert RGBA to Windows DIB BGRA (bottom-up scanlines)
                for (uint32_t y = 0; y < info.ysize; y++) {
                    uint8_t* dst_row = pBits + (info.ysize - 1 - y) * stride;
                    const uint8_t* src_row = temp_rgba + y * (info.xsize * 4);

                    for (uint32_t x = 0; x < info.xsize; x++) {
                        dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // B
                        dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G
                        dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // R
                        dst_row[x * 4 + 3] = src_row[x * 4 + 3]; // A
                    }
                }
            }
        } else if (status == JXL_DEC_SUCCESS) {
            break;
        }
    }

    if (temp_rgba) free(temp_rgba);
    JxlDecoderDestroy(dec);

    return surface;
}
