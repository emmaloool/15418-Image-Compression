// Copyright 2012 Google Inc. All Rights Reserved.
//
// This code is licensed under the same terms as WebM:
//  Software License Agreement:  http://www.webmproject.org/license/software/
//  Additional IP Rights Grant:  http://www.webmproject.org/license/additional/
// -----------------------------------------------------------------------------
//
// main entry for the decoder
//
// Author: Vikas Arora (vikaas.arora@gmail.com)
//         jyrki@google.com (Jyrki Alakuijala)

#include <stdio.h>
#include <stdlib.h>
#include "./vp8li.h"
#include "../dsp/lossless.h"
#include "../utils/huffman.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

static const size_t kHeaderBytes = 5;
static const int kImageSizeBits = 14;

static const int kCodeLengthLiterals = 16;
static const int kCodeLengthRepeatCode = 16;
static const int kCodeLengthExtraBits[3] = { 2, 3, 7 };
static const int kCodeLengthRepeatOffsets[3] = { 3, 3, 11 };

#define NUM_LENGTH_CODES    24
#define NUM_DISTANCE_CODES  40
#define DEFAULT_CODE_LENGTH 8

// -----------------------------------------------------------------------------
//  Five Huffman codes are used at each meta code:
//  1. green + length prefix codes + color cache codes,
//  2. alpha,
//  3. red,
//  4. blue, and,
//  5. distance prefix codes.
typedef enum {
  GREEN = 0,
  RED   = 1,
  BLUE  = 2,
  ALPHA = 3,
  DIST  = 4,
} HuffIndex;

static const uint16_t kAlphabetSize[HUFFMAN_CODES_PER_META_CODE] = {
  NUM_LITERAL_CODES + NUM_LENGTH_CODES,
  NUM_LITERAL_CODES, NUM_LITERAL_CODES, NUM_LITERAL_CODES,
  NUM_DISTANCE_CODES
};


#define NUM_CODE_LENGTH_CODES       19
static const uint8_t kCodeLengthCodeOrder[NUM_CODE_LENGTH_CODES] = {
  17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

#define CODE_TO_PLANE_CODES        120
static const uint8_t code_to_plane_lut[CODE_TO_PLANE_CODES] = {
   0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a,
   0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a,
   0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b,
   0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03,
   0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c,
   0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
   0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b,
   0x32, 0x3e, 0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f,
   0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b,
   0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41,
   0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d, 0x51, 0x5f,
   0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};

static int DecodeImageStream(int xsize, int ysize,
                             int is_level0,
                             VP8LDecoder* const dec,
                             uint32_t** const decoded_data);

//------------------------------------------------------------------------------

static int ReadImageSize(VP8LBitReader* const br,
                         int* const width, int* const height) {
  const int signature = VP8LReadBits(br, 8);
  if (signature != LOSSLESS_MAGIC_BYTE &&
      signature != LOSSLESS_MAGIC_BYTE_RSVD) {
    return 0;
  }
  *width = VP8LReadBits(br, kImageSizeBits) + 1;
  *height = VP8LReadBits(br, kImageSizeBits) + 1;
  return 1;
}

int VP8LGetInfo(const uint8_t* data, size_t data_size,
                int* width, int* height) {
  if (data == NULL || data_size < kHeaderBytes) {
    return 0;         // not enough data
  } else {
    int w, h;
    VP8LBitReader br;
    VP8LInitBitReader(&br, data, data_size);
    if (!ReadImageSize(&br, &w, &h)) {
      return 0;
    }
    if (width != NULL) *width = w;
    if (height != NULL) *height = h;
    return 1;
  }
}

//------------------------------------------------------------------------------

static WEBP_INLINE int GetCopyDistance(int distance_symbol,
                                       VP8LBitReader* const br) {
  int extra_bits, offset;
  if (distance_symbol < 4) {
    return distance_symbol + 1;
  }
  extra_bits = (distance_symbol - 2) >> 1;
  offset = (2 + (distance_symbol & 1)) << extra_bits;
  return offset + VP8LReadBits(br, extra_bits) + 1;
}

static WEBP_INLINE int GetCopyLength(int length_symbol,
                                     VP8LBitReader* const br) {
  // Length and distance prefixes are encoded the same way.
  return GetCopyDistance(length_symbol, br);
}

static WEBP_INLINE int PlaneCodeToDistance(int xsize, int plane_code) {
  if (plane_code > CODE_TO_PLANE_CODES) {
    return plane_code - CODE_TO_PLANE_CODES;
  } else {
    const int dist_code = code_to_plane_lut[plane_code - 1];
    const int yoffset = dist_code >> 4;
    const int xoffset = 8 - (dist_code & 0xf);
    return yoffset * xsize + xoffset;
  }
}

//------------------------------------------------------------------------------
// Decodes the next Huffman code from bit-stream.
// FillBitWindow(br) needs to be called at minimum every second call
// to ReadSymbolUnsafe.
static int ReadSymbolUnsafe(const HuffmanTree* tree, VP8LBitReader* const br) {
  const HuffmanTreeNode* node = tree->root_;
  assert(node != NULL);
  while (!HuffmanTreeNodeIsLeaf(node)) {
    node = HuffmanTreeNextNode(node, VP8LReadOneBitUnsafe(br));
  }
  return node->symbol_;
}

static WEBP_INLINE int ReadSymbol(const HuffmanTree* tree,
                                  VP8LBitReader* const br) {
  const int read_safe = (br->pos_ > br->len_ - 8);
  if (!read_safe) {
    return ReadSymbolUnsafe(tree, br);
  } else {
    const HuffmanTreeNode* node = tree->root_;
    assert(node != NULL);
    while (!HuffmanTreeNodeIsLeaf(node)) {
      node = HuffmanTreeNextNode(node, VP8LReadOneBit(br));
    }
    return node->symbol_;
  }
}

static int ReadHuffmanCodeLengths(
    VP8LDecoder* const dec, const int* const code_length_code_lengths,
    int num_codes, int num_symbols, int* const code_lengths) {
  int ok = 0;
  VP8LBitReader* const br = &dec->br_;
  int symbol;
  int max_symbol;
  int prev_code_len = DEFAULT_CODE_LENGTH;
  HuffmanTree tree;

  if (!HuffmanTreeBuildImplicit(&tree, code_length_code_lengths, num_codes)) {
    dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
    return 0;
  }

  if (VP8LReadBits(br, 1)) {    // use length
    const int length_nbits = 2 + 2 * VP8LReadBits(br, 3);
    max_symbol = 2 + VP8LReadBits(br, length_nbits);
    if (max_symbol > num_symbols) {
      dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
      goto End;
    }
  } else {
    max_symbol = num_symbols;
  }

  symbol = 0;
  while (symbol < num_symbols) {
    int code_len;
    if (max_symbol-- == 0) break;
    VP8LFillBitWindow(br);
    code_len = ReadSymbol(&tree, br);
    if (code_len < kCodeLengthLiterals) {
      code_lengths[symbol++] = code_len;
      if (code_len != 0) prev_code_len = code_len;
    } else {
      const int use_prev = (code_len == kCodeLengthRepeatCode);
      const int slot = code_len - kCodeLengthLiterals;
      const int extra_bits = kCodeLengthExtraBits[slot];
      const int repeat_offset = kCodeLengthRepeatOffsets[slot];
      int repeat = VP8LReadBits(br, extra_bits) + repeat_offset;
      if (symbol + repeat > num_symbols) {
        dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
        goto End;
      } else {
        const int length = use_prev ? prev_code_len : 0;
        while (repeat-- > 0) code_lengths[symbol++] = length;
      }
    }
  }
  ok = 1;

 End:
  HuffmanTreeRelease(&tree);
  return ok;
}

static int ReadHuffmanCode(int alphabet_size, VP8LDecoder* const dec,
                           HuffmanTree* const tree) {
  int ok = 0;
  VP8LBitReader* const br = &dec->br_;
  const int simple_code = VP8LReadBits(br, 1);

  if (simple_code) {  // Read symbols, codes & code lengths directly.
    int symbols[2];
    int codes[2];
    int code_lengths[2];
    const int nbits = VP8LReadBits(br, 3);
    const int num_symbols = 1 + ((nbits == 0) ? 0 : VP8LReadBits(br, 1));

    if (nbits == 0) {
      symbols[0] = 0;
      codes[0] = 0;
      code_lengths[0] = 0;
    } else {
      const int num_bits = (nbits - 1) * 2 + 4;
      int i;
      for (i = 0; i < num_symbols; ++i) {
        symbols[i] = VP8LReadBits(br, num_bits);
        if (symbols[i] >= alphabet_size) {
          dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
          return 0;
        }
        codes[i] = i;
        code_lengths[i] = num_symbols - 1;
      }
    }
    ok = HuffmanTreeBuildExplicit(tree, code_lengths, codes,
                                  symbols, num_symbols);
  } else {  // Decode Huffman-coded code lengths.
    int* code_lengths = NULL;
    int i;
    int code_length_code_lengths[NUM_CODE_LENGTH_CODES] = { 0 };
    const int num_codes = VP8LReadBits(br, 4) + 4;
    if (num_codes > NUM_CODE_LENGTH_CODES) {
      dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
      return 0;
    }

    code_lengths = (int*)calloc(alphabet_size, sizeof(*code_lengths));
    if (code_lengths == NULL) {
      dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
      return 0;
    }

    for (i = 0; i < num_codes; ++i) {
      code_length_code_lengths[kCodeLengthCodeOrder[i]] = VP8LReadBits(br, 3);
    }
    ok = ReadHuffmanCodeLengths(dec, code_length_code_lengths,
                                NUM_CODE_LENGTH_CODES,
                                alphabet_size, code_lengths);
    if (ok) {
      ok = HuffmanTreeBuildImplicit(tree, code_lengths, alphabet_size);
    }
    free(code_lengths);
  }
  ok = ok && !br->error_;
  if (!ok) {
    dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
    return 0;
  }
  return 1;
}

static int ReadHuffmanCodes(VP8LDecoder* const dec, int xsize, int ysize,
                            int* const color_cache_bits_ptr) {
  int ok = 0;
  int i, j;
  int color_cache_size;
  VP8LBitReader* const br = &dec->br_;
  VP8LMetadata* const hdr = &dec->hdr_;
  uint32_t* huffman_image = NULL;
  HTreeGroup* htree_groups = NULL;
  int num_htree_groups = 1;

  if (VP8LReadBits(br, 1)) {      // use meta Huffman codes
    int meta_codes_nbits;
    const int huffman_precision = VP8LReadBits(br, 4);
    const int huffman_xsize = VP8LSubSampleSize(xsize, huffman_precision);
    const int huffman_ysize = VP8LSubSampleSize(ysize, huffman_precision);
    const int huffman_pixs = huffman_xsize * huffman_ysize;
    if (!DecodeImageStream(huffman_xsize, huffman_ysize, 0, dec,
                           &huffman_image)) {
      dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
      goto Error;
    }
    hdr->huffman_subsample_bits_ = huffman_precision;
    for (i = 0; i < huffman_pixs; ++i) {
      // The huffman data is stored in red and green bytes.
      huffman_image[i] = (huffman_image[i] >> 8) & 0xffff;
    }

    meta_codes_nbits = VP8LReadBits(br, 4);
    num_htree_groups = 2 + VP8LReadBits(br, meta_codes_nbits);
  }

  if (VP8LReadBits(br, 1)) {    // use color cache
    *color_cache_bits_ptr = VP8LReadBits(br, 4);
    color_cache_size = 1 << *color_cache_bits_ptr;
  } else {
    *color_cache_bits_ptr = 0;
    color_cache_size = 0;
  }

  htree_groups = (HTreeGroup*)calloc(num_htree_groups, sizeof(*htree_groups));
  if (htree_groups == NULL) {
    dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
    goto Error;
  }

  ok = !br->error_;
  for (i = 0; ok && i < num_htree_groups; ++i) {
    for (j = 0; j < HUFFMAN_CODES_PER_META_CODE; ++j) {
      int alphabet_size = kAlphabetSize[j];
      if (j == 0) {
        alphabet_size += color_cache_size;
      }
      ok = ReadHuffmanCode(alphabet_size, dec, &htree_groups[i].htrees_[j]);
      ok = ok && !br->error_;
    }
  }

  if (ok) {   // finalize pointers and return
    hdr->huffman_image_ = huffman_image;
    hdr->num_htree_groups_ = num_htree_groups;
    hdr->htree_groups_ = htree_groups;
    return ok;
  }

 Error:
  free(huffman_image);
  if (htree_groups != NULL) {
    for (i = 0; i < num_htree_groups; ++i) {
      for (j = 0; j < HUFFMAN_CODES_PER_META_CODE; ++j) {
        HuffmanTreeRelease(&htree_groups[i].htrees_[j]);
      }
    }
    free(htree_groups);
  }
  return 0;
}

//------------------------------------------------------------------------------
// Scaling.

static int AllocateAndInitRescaler(VP8LDecoder* const dec, VP8Io* const io) {
  const int num_channels = 4;
  const int in_width = io->mb_w;
  const int out_width = io->scaled_width;
  const int in_height = io->mb_h;
  const int out_height = io->scaled_height;
  const size_t work_size = 2 * num_channels * out_width;
  int32_t* work;        // Rescaler work area.
  const size_t scaled_data_size = num_channels * out_width;
  uint32_t* scaled_data;  // Temporary storage for scaled BGRA data.
  const size_t memory_size = sizeof(*dec->rescaler) +
                             work_size * sizeof(*work) +
                             scaled_data_size * sizeof(*scaled_data);
  uint8_t* memory = calloc(1, memory_size);
  if (memory == NULL) {
    dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
    return 0;
  }
  assert(dec->rescaler_memory == NULL);
  dec->rescaler_memory = memory;

  dec->rescaler = (WebPRescaler*)memory;
  memory += sizeof(*dec->rescaler);
  work = (int32_t*)memory;
  memory += work_size * sizeof(*work);
  scaled_data = (uint32_t*)memory;

  WebPRescalerInit(dec->rescaler, in_width, in_height, (uint8_t*)scaled_data,
                   out_width, out_height, 0, num_channels,
                   in_width, out_width, in_height, out_height, work);
  return 1;
}

// We have special "export" function since we need to convert from BGRA
static int Export(VP8LDecoder* const dec, WEBP_CSP_MODE colorspace,
                  int rgba_stride, uint8_t* const rgba) {
  WebPRescaler* const rescaler = dec->rescaler;
  const uint32_t* const src = (const uint32_t*)rescaler->dst;
  const int dst_width = rescaler->dst_width;
  int num_lines_out = 0;
  while (WebPRescalerHasPendingOutput(rescaler)) {
    uint8_t* const dst = rgba + num_lines_out * rgba_stride;
    WebPRescalerExportRow(rescaler);
    VP8LConvertFromBGRA(src, dst_width, colorspace, dst);
    ++num_lines_out;
  }
  return num_lines_out;
}

// Emit scaled rows.
static int EmitRescaledRows(VP8LDecoder* const dec, WEBP_CSP_MODE colorspace,
                            const uint32_t* const data, int in_stride, int mb_h,
                            uint8_t* const out, int out_stride) {
  const uint8_t* const in = (const uint8_t*)data;
  int num_lines_in = 0;
  int num_lines_out = 0;
  while (num_lines_in < mb_h) {
    const uint8_t* row_in = in + num_lines_in * in_stride;
    uint8_t* const row_out = out + num_lines_out * out_stride;
    num_lines_in += WebPRescalerImport(dec->rescaler, mb_h - num_lines_in,
                                       row_in, in_stride);
    num_lines_out += Export(dec, colorspace, out_stride, row_out);
  }
  return num_lines_out;
}

// Emit rows without any scaling.
static int EmitRows(WEBP_CSP_MODE colorspace,
                    const uint32_t* const data, int in_stride,
                    int mb_w, int mb_h,
                    uint8_t* const out, int out_stride) {
  int lines = mb_h;
  const uint8_t* row_in = (const uint8_t*)data;
  uint8_t* row_out = out;
  while (lines-- > 0) {
    VP8LConvertFromBGRA((const uint32_t*)row_in, mb_w, colorspace, row_out);
    row_in += in_stride;
    row_out += out_stride;
  }
  return mb_h;  // Num rows out == num rows in.
}

//------------------------------------------------------------------------------
// Cropping.

// Sets io->mb_y, io->mb_h & io->mb_w according to start row, end row and
// crop options. Also updates the input data pointer, so that it points to the
// start of the cropped window.
// Note that 'pixel_stride' is in units of 'uint32_t' (and not 'bytes).
// Returns true if the crop window is not empty.
static int SetCropWindow(VP8Io* const io, int y_start, int y_end,
                         uint32_t** const in_data, int pixel_stride) {
  assert(y_start < y_end);
  assert(io->crop_left < io->crop_right);
  if (y_end > io->crop_bottom) {
    y_end = io->crop_bottom;  // make sure we don't overflow on last row.
  }
  if (y_start < io->crop_top) {
    const int delta = io->crop_top - y_start;
    y_start = io->crop_top;
    *in_data += pixel_stride * delta;
  }
  if (y_start >= y_end) return 0;  // Crop window is empty.

  *in_data += io->crop_left;

  io->mb_y = y_start - io->crop_top;
  io->mb_w = io->crop_right - io->crop_left;
  io->mb_h = y_end - y_start;
  return 1;  // Non-empty crop window.
}

//------------------------------------------------------------------------------

static WEBP_INLINE int GetMetaIndex(
    const uint32_t* const image, int xsize, int bits, int x, int y) {
  if (bits == 0) return 0;
  return image[xsize * (y >> bits) + (x >> bits)];
}

static WEBP_INLINE HTreeGroup* GetHtreeGroupForPos(VP8LMetadata* const hdr,
                                                   int x, int y) {
  const int meta_index = GetMetaIndex(hdr->huffman_image_, hdr->huffman_xsize_,
                                      hdr->huffman_subsample_bits_, x, y);
  return hdr->htree_groups_ + meta_index;
}

// Processes (transforms, scales & color-converts) the rows decoded after the
// last call.
static WEBP_INLINE void ProcessRows(VP8LDecoder* const dec, int row) {
  int n = dec->next_transform_;
  VP8Io* const io = dec->io_;
  WebPDecParams* const params = (WebPDecParams*)io->opaque;
  const WebPDecBuffer* const output = params->output;
  const int argb_offset = dec->width_ * dec->last_row_;
  const int num_rows = row - dec->last_row_;
  const int cache_pixs = dec->width_ * num_rows;
  uint32_t* rows_data = dec->argb_cache_;
  int num_rows_out = num_rows;  // Default.

  if (num_rows <= 0) return;  // Nothing to be done.

  // Inverse transforms.
  // TODO: most transforms only need to operate on the cropped region only.
  memcpy(rows_data, dec->argb_ + argb_offset, cache_pixs * sizeof(*rows_data));
  while (n-- > 0) {
    VP8LTransform* const transform = &dec->transforms_[n];
    VP8LInverseTransform(transform, dec->last_row_, row,
                         dec->argb_ + argb_offset, rows_data);
  }

  // Emit output.
  {
    const WebPRGBABuffer* const buf = &output->u.RGBA;
    uint8_t* const rgba = buf->rgba + dec->last_out_row_ * buf->stride;
    const WEBP_CSP_MODE colorspace = output->colorspace;
    if (!SetCropWindow(io, dec->last_row_, row, &rows_data, io->width)) {
      num_rows_out = 0;  // Nothing to output.
    } else {
      const int in_stride = io->width * sizeof(*rows_data);
      num_rows_out = io->use_scaling ?
          EmitRescaledRows(dec, colorspace, rows_data, in_stride, io->mb_h,
                           rgba, buf->stride) :
          EmitRows(colorspace, rows_data, in_stride, io->mb_w, io->mb_h,
                   rgba, buf->stride);
    }
  }

  // Update 'last_row' and 'last_out_row'.
  dec->last_row_ = row;
  assert(dec->last_row_ <= io->height);
  dec->last_out_row_ += num_rows_out;
  assert(dec->last_out_row_ <= output->height);
}

static int DecodeImageData(VP8LDecoder* const dec,
                           uint32_t* const data, int width, int height,
                           int process_row) {
  int ok = 1;
  int col = 0, row = 0;
  VP8LBitReader* const br = &dec->br_;
  VP8LMetadata* const hdr = &dec->hdr_;
  VP8LColorCache* const color_cache = hdr->color_cache_;
  HTreeGroup* htree_group = hdr->htree_groups_;
  uint32_t* src = data;
  uint32_t* last_cached = data;
  uint32_t* const src_end = data + width * height;
  const int len_code_limit = NUM_LITERAL_CODES + NUM_LENGTH_CODES;
  const int color_cache_limit = len_code_limit + hdr->color_cache_size_;
  const int mask = hdr->huffman_mask_;

  assert(htree_group != NULL);

  while (!br->eos_ && src < src_end) {
    int code;
    // Only update when changing tile. Note we could use the following test:
    //   if "((((prev_col ^ col) | prev_row ^ row)) > mask)" -> tile changed
    // but that's actually slower and requires storing the previous col/row
    if ((col & mask) == 0) {
      htree_group = GetHtreeGroupForPos(hdr, col, row);
    }
    VP8LFillBitWindow(br);
    code = ReadSymbol(&htree_group->htrees_[GREEN], br);
    if (code < NUM_LITERAL_CODES) {   // Literal.
      int red, green, blue, alpha;
      red = ReadSymbol(&htree_group->htrees_[RED], br);
      green = code;
      VP8LFillBitWindow(br);
      blue = ReadSymbol(&htree_group->htrees_[BLUE], br);
      alpha = ReadSymbol(&htree_group->htrees_[ALPHA], br);
      *src = (alpha << 24) + (red << 16) + (green << 8) + blue;
 AdvanceByOne:
      ++src;
      ++col;
      if (col >= width) {
        col = 0;
        ++row;
        if (process_row && (row % NUM_ARGB_CACHE_ROWS == 0)) {
          ProcessRows(dec, row);
        }
        if (color_cache != NULL) {
          while (last_cached < src) {
            VP8LColorCacheInsert(color_cache, *last_cached++);
          }
        }
      }
    } else if (code < len_code_limit) {           // Backward reference
      int dist_code, dist;
      const int length_sym = code - NUM_LITERAL_CODES;
      const int length = GetCopyLength(length_sym, br);
      const int dist_symbol = ReadSymbol(&htree_group->htrees_[DIST], br);
      VP8LFillBitWindow(br);
      dist_code = GetCopyDistance(dist_symbol, br);
      dist = PlaneCodeToDistance(width, dist_code);
      if (src - dist < data || src + length > src_end) {
        ok = 0;
        goto Error;
      }
      {
        int i;
        for (i = 0; i < length; ++i) src[i] = src[i - dist];
        src += length;
      }
      col += length;
      while (col >= width) {
        col -= width;
        ++row;
        if (process_row && (row % NUM_ARGB_CACHE_ROWS == 0)) {
          ProcessRows(dec, row);
        }
      }
      if (src < src_end) {
        htree_group = GetHtreeGroupForPos(hdr, col, row);
        if (color_cache != NULL) {
          while (last_cached < src) {
            VP8LColorCacheInsert(color_cache, *last_cached++);
          }
        }
      }
    } else if (code < color_cache_limit) {    // Color cache.
      const int key = code - len_code_limit;
      assert(color_cache != NULL);
      while (last_cached < src) {
        VP8LColorCacheInsert(color_cache, *last_cached++);
      }
      *src = VP8LColorCacheLookup(color_cache, key);
      goto AdvanceByOne;
    } else {    // Not reached.
      ok = 0;
      goto Error;
    }
    ok = !br->error_;
    if (!ok) goto Error;
  }
  // Process the remaining rows corresponding to last row-block.
  if (process_row) ProcessRows(dec, row);

 Error:
  if (br->error_ || !ok) {
    ok = 0;
    dec->status_ = (!br->eos_) ?
        VP8_STATUS_BITSTREAM_ERROR : VP8_STATUS_SUSPENDED;
  } else if (src == src_end) {
    dec->state_ = READ_DATA;
  }

  return ok;
}

// -----------------------------------------------------------------------------
// VP8LTransform

static void ClearTransform(VP8LTransform* const transform) {
  free(transform->data_);
  transform->data_ = NULL;
}

static void ApplyInverseTransforms(VP8LDecoder* const dec, int start_idx,
                                   uint32_t* const decoded_data) {
  int n = dec->next_transform_;
  assert(start_idx >= 0);
  while (n-- > start_idx) {
    VP8LTransform* const transform = &dec->transforms_[n];
    VP8LInverseTransform(transform, 0, transform->ysize_,
                         decoded_data, decoded_data);
    ClearTransform(transform);
  }
  dec->next_transform_ = start_idx;
}

// For security reason, we need to remap the color map to span
// the total possible bundled values, and not just the num_colors.
static int ExpandColorMap(int num_colors, VP8LTransform* const transform) {
  int i;
  const int final_num_colors = 1 << (8 >> transform->bits_);
  uint32_t* const new_color_map =
      (uint32_t*)malloc(final_num_colors * sizeof(*new_color_map));
  if (new_color_map == NULL) {
    return 0;
  } else {
    uint8_t* const data = (uint8_t*)transform->data_;
    uint8_t* const new_data = (uint8_t*)new_color_map;
    new_color_map[0] = transform->data_[0];
    for (i = 4; i < 4 * num_colors; ++i) {
      // Equivalent to AddPixelEq(), on a byte-basis.
      new_data[i] = (data[i] + new_data[i - 4]) & 0xff;
    }
    for (; i < 4 * final_num_colors; ++i)
      new_data[i] = 0;  // black tail.
    free(transform->data_);
    transform->data_ = new_color_map;
  }
  return 1;
}

static int ReadTransform(int* const xsize, int const* ysize,
                         VP8LDecoder* const dec) {
  int ok = 1;
  VP8LBitReader* const br = &dec->br_;
  VP8LTransform* transform = &dec->transforms_[dec->next_transform_];
  const VP8LImageTransformType type =
      (VP8LImageTransformType)VP8LReadBits(br, 2);

  if (dec->next_transform_ == NUM_TRANSFORMS) {
    return 0;
  }
  transform->type_ = type;
  transform->xsize_ = *xsize;
  transform->ysize_ = *ysize;
  transform->data_ = NULL;
  ++dec->next_transform_;

  switch (type) {
    case PREDICTOR_TRANSFORM:
    case CROSS_COLOR_TRANSFORM:
      transform->bits_ = VP8LReadBits(br, 4);
      ok = DecodeImageStream(VP8LSubSampleSize(transform->xsize_,
                                               transform->bits_),
                             VP8LSubSampleSize(transform->ysize_,
                                               transform->bits_),
                             0, dec, &transform->data_);
      break;
    case COLOR_INDEXING_TRANSFORM: {
       const int num_colors = VP8LReadBits(br, 8) + 1;
       const int bits = (num_colors > 16) ? 0
                      : (num_colors > 4) ? 1
                      : (num_colors > 2) ? 2
                      : 3;
       *xsize = VP8LSubSampleSize(transform->xsize_, bits);
       transform->bits_ = bits;
       ok = DecodeImageStream(num_colors, 1, 0, dec, &transform->data_);
       ok = ok && ExpandColorMap(num_colors, transform);
      break;
    }
    case SUBTRACT_GREEN:
      break;
    default:
      assert(0);    // can't happen
      break;
  }

  return ok;
}

// -----------------------------------------------------------------------------
// VP8LMetadata

static void InitMetadata(VP8LMetadata* const hdr) {
  assert(hdr);
  memset(hdr, 0, sizeof(*hdr));
}

static void ClearMetadata(VP8LMetadata* const hdr) {
  assert(hdr);

  free(hdr->huffman_image_);
  if (hdr->htree_groups_ != NULL) {
    int i, j;
    for (i = 0; i < hdr->num_htree_groups_; ++i) {
      for (j = 0; j < HUFFMAN_CODES_PER_META_CODE; ++j) {
        HuffmanTreeRelease(&hdr->htree_groups_[i].htrees_[j]);
      }
    }
    free(hdr->htree_groups_);
  }

  VP8LColorCacheDelete(hdr->color_cache_);
  InitMetadata(hdr);
}

// -----------------------------------------------------------------------------
// VP8LDecoder

VP8LDecoder* VP8LNew(void) {
  VP8LDecoder* const dec = (VP8LDecoder*)calloc(1, sizeof(*dec));
  if (dec == NULL) return NULL;
  dec->status_ = VP8_STATUS_OK;
  dec->action_ = READ_DIM;
  dec->state_ = READ_DIM;
  return dec;
}

void VP8LClear(VP8LDecoder* const dec) {
  int i;
  if (dec == NULL) return;
  ClearMetadata(&dec->hdr_);

  free(dec->argb_);
  dec->argb_ = NULL;
  for (i = 0; i < dec->next_transform_; ++i) {
    ClearTransform(&dec->transforms_[i]);
  }
  dec->next_transform_ = 0;

  free(dec->rescaler_memory);
  dec->rescaler_memory = NULL;
}

void VP8LDelete(VP8LDecoder* const dec) {
  if (dec != NULL) {
    VP8LClear(dec);
    free(dec);
  }
}

static void UpdateDecoder(VP8LDecoder* const dec, int width, int height) {
  VP8LMetadata* const hdr = &dec->hdr_;
  const int num_bits = hdr->huffman_subsample_bits_;
  dec->width_ = width;
  dec->height_ = height;

  hdr->huffman_xsize_ = VP8LSubSampleSize(width, num_bits);
  hdr->huffman_mask_ = (num_bits == 0) ? ~0 : (1 << num_bits) - 1;
}

static int DecodeImageStream(int xsize, int ysize,
                             int is_level0,
                             VP8LDecoder* const dec,
                             uint32_t** const decoded_data) {
  int ok = 1;
  int transform_xsize = xsize;
  int transform_ysize = ysize;
  VP8LMetadata* const hdr = &dec->hdr_;
  uint32_t* data = NULL;
  int color_cache_bits = 0;

  VP8LBitReader* const br = &dec->br_;
  int transform_start_idx = dec->next_transform_;

  // Step#1: Read the transforms (may recurse).
  if (is_level0) {
    while (ok && VP8LReadBits(br, 1)) {
      ok = ReadTransform(&transform_xsize, &transform_ysize, dec);
    }
  }

  // Step#2: Read the Huffman codes (may recurse).
  ok = ok && ReadHuffmanCodes(dec, transform_xsize, transform_ysize,
                              &color_cache_bits);

  if (!ok) {
    dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
    goto End;
  }

  if (color_cache_bits > 0) {
    hdr->color_cache_size_ = 1 << color_cache_bits;
    hdr->color_cache_ = (VP8LColorCache*)malloc(sizeof(*hdr->color_cache_));
    if (hdr->color_cache_ == NULL ||
        !VP8LColorCacheInit(hdr->color_cache_, color_cache_bits)) {
      dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
      ok = 0;
      goto End;
    }
  }

  UpdateDecoder(dec, transform_xsize, transform_ysize);

  if (is_level0) {   // level 0 complete
    dec->state_ = READ_HDR;
    goto End;
  }

  data = (uint32_t*)malloc(transform_xsize * transform_ysize * sizeof(*data));
  if (data == NULL) {
    dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
    ok = 0;
    goto End;
  }

  // Step#3: Use the Huffman trees to decode the LZ77 encoded data.
  ok = DecodeImageData(dec, data, transform_xsize, transform_ysize, 0);
  ok = ok && !br->error_;

  // Step#4: Apply transforms on the decoded data.
  if (ok) ApplyInverseTransforms(dec, transform_start_idx, data);

 End:

  if (!ok) {
    free(data);
    ClearMetadata(hdr);
    // If not enough data (br.eos_) resulted in BIT_STREAM_ERROR, update the
    // status appropriately.
    if (dec->status_ == VP8_STATUS_BITSTREAM_ERROR && dec->br_.eos_) {
      dec->status_ = VP8_STATUS_SUSPENDED;
    }
  } else {
    *decoded_data = data;
    if (!is_level0) ClearMetadata(hdr);  // Clean up temporary data behind.
  }
  return ok;
}

//------------------------------------------------------------------------------

int VP8LDecodeHeader(VP8LDecoder* const dec, VP8Io* const io) {
  int width, height;
  uint32_t* decoded_data = NULL;

  if (dec == NULL) return 0;
  if (io == NULL) {
    dec->status_ = VP8_STATUS_INVALID_PARAM;
    return 0;
  }

  dec->io_ = io;
  dec->status_ = VP8_STATUS_OK;
  VP8LInitBitReader(&dec->br_, io->data, io->data_size);
  if (!ReadImageSize(&dec->br_, &width, &height)) {
    dec->status_ = VP8_STATUS_BITSTREAM_ERROR;
    goto Error;
  }
  dec->state_ = READ_DIM;
  io->width = width;
  io->height = height;

  dec->action_ = READ_HDR;
  if (!DecodeImageStream(width, height, 1, dec, &decoded_data)) {
    free(decoded_data);
    goto Error;
  }
  return 1;

 Error:
   VP8LClear(dec);
   assert(dec->status_ != VP8_STATUS_OK);
   return 0;
}

int VP8LDecodeImage(VP8LDecoder* const dec) {
  VP8Io* io = NULL;
  WebPDecParams* params = NULL;
  WebPDecBuffer* output = NULL;

  // Sanity checks.
  if (dec == NULL) return 0;

  io = dec->io_;
  params = (WebPDecParams*)io->opaque;
  assert(params != NULL);
  output = params->output;
  // RGBA_4444 & RGB_565 are unsupported for now & YUV modes are invalid.
  if (output->colorspace >= MODE_RGBA_4444) {
    dec->status_ = VP8_STATUS_INVALID_PARAM;
    goto Err;
  }

  // Initialization.
  if (!WebPIoInitFromOptions(params->options, io, MODE_BGRA)) {
    dec->status_ = VP8_STATUS_INVALID_PARAM;
    goto Err;
  }

  {
    const int num_pixels = dec->width_ * dec->height_;
    // Scratch buffer corresponding to top-prediction row for transforming the
    // first row in the row-blocks.
    const int cache_top_pixels = io->width;
    // Scratch buffer for temporary BGRA storage.
    const int cache_pixels = io->width * NUM_ARGB_CACHE_ROWS;
    const int total_num_pixels =
        num_pixels + cache_top_pixels + cache_pixels;
    dec->argb_ = (uint32_t*)malloc(total_num_pixels * sizeof(*dec->argb_));
    if (dec->argb_ == NULL) {
      dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
      goto Err;
    }
    dec->argb_cache_ = dec->argb_ + num_pixels + cache_top_pixels;
  }

  if (io->use_scaling && !AllocateAndInitRescaler(dec, io)) {
    dec->status_ = VP8_STATUS_OUT_OF_MEMORY;
    goto Err;
  }

  // Decode.
  dec->action_ = READ_DATA;
  if (!DecodeImageData(dec, dec->argb_, dec->width_, dec->height_, 1)) {
    goto Err;
  }

  // Cleanup.
  params->last_y = dec->last_out_row_;
  VP8LClear(dec);
  return 1;

 Err:
  VP8LClear(dec);
  assert(dec->status_ != VP8_STATUS_OK);
  return 0;
}

//------------------------------------------------------------------------------

#if defined(__cplusplus) || defined(c_plusplus)
}    // extern "C"
#endif