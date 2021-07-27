/*
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, 
   BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
   IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, 
   OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
   OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
   THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "c_vector.h"

const char* kInsFileSignature = "8db42d694ccc418790edff439fe026bf";
#define kInsFileSignatureLength  32
#define kInsFileMinHeaderLength  (kInsFileSignatureLength+40)

#define kCopyBufferSize  8*1024 /* Buffer size for copy media data */

// Some info about Insta360 metadata format can be found here
// https://fossies.org/linux/Image-ExifTool/lib/Image/ExifTool/QuickTimeStream.pl

// Stitching offset string examples:
// 2_1497.030_1514.415_1501.982_0.0_0.00_0.000_1491.991_4555.739_1542.696_0.089_-0.077_179.891_6080_3040_2323
// 2_1646.662_1440.499_1419.611_0.000_0.000_0.000_1654.103_4309.465_1412.394_0.000_0.000_180.000_5760_2880_19

///////////////////
// File global structure (from start file to end)
// 0         Media file data (INSV: H.264 video in mp4 container, INSP: JPEG image)
// NNNN      File trailer (Insta360 metainfo)

///////////////////
// Trailer structure (from end file to start)
// 0         File signature       (32 bytes)    8db42d694ccc418790edff439fe026bf
// 32        InsFileTrailerHeaderType  (8 bytes)
// 40        padding zero         (32 bytes)
// 72        InsFileTrailerEntryHeaderType     (6 bytes)
// 72+N      trailer hdr data         (N bytes)
// XXX       InsFileTrailerEntryHeaderType     (6 bytes)
// MMM       trailer hdr data         (M bytes)
// .........................
// until trailer size == file pos


// Trailer header data format depends on InsFileTrailerEntryHeaderType.type value:
// 0x101     specific Insta360 info   (contains stitching offset data, serial, camera model, etc)
// 0x200     ???
// 0x300     accelerometer and angular velocity info
// 0x400     exposure time info
// 0x500     ???
// 0x600     video timestamps
// 0x700     GPS data

//////////////////
// Specific Insta360 trailer header structure
// 0         tag 0 type code  (1 byte)
// 1         tag 0 data size  (1 byte)  (value does not include tag data size and type code fields)
// 2         tag 0 data       (N bytes, tag 0 data size)
// N+0       tag 1 type code  (1 byte)
// N+1       tag 1 data size  (1 byte)
// N+M       tag 1 data       (M bytes, tag 1 data size)
// .........
// Z+0       tag 3 type code  (1 byte)
// Z+1       tag 3 data size  (1 byte)
// Z+2       tag 3 data       (A bytes, tag 3 data size)
// QQQQ      tail data      <- format unknown, usually starts from value 0x48, size calculates as (SpecificHeaderSize - 4_tags_size)


// structures used directly in file
#pragma pack(push,1)

typedef struct _InsFileTrailerEntryHeaderType {
  uint16_t type;              /** Trailer header data type (0x101, 0x200, 0x300, ...) */
  uint32_t length;            /** Trailer data length, does not include this structure size */
} InsFileTrailerEntryHeaderType;

typedef struct _InsFileTrailerHeaderType {
  uint32_t trailer_len;       /** Total length of all trailer data include signatures, headers, etc */
  uint32_t trailer_version;   /** Trailer version, usually 3 */
} InsFileTrailerHeaderType;

/** Header for each tag in specific data header */
typedef struct _InsFileSpecificDataTagHeaderType {
  uint8_t type_code;          /** Tag type code, value from enum InsFileSpecificHeaderTagTypes */
  uint8_t data_size;          /** Tag data size */
} InsFileSpecificDataTagHeaderType;

#pragma pack(pop)


// structures for work in RAM

typedef struct _InsTrailerEntryHeaderInfoType {
  const InsFileTrailerEntryHeaderType* hdr;  /** Pointer to original file entry header in trailer buffer */
  uint64_t trailer_offset_to_data;           /** Offset to data in trailer buffer */
} InsTrailerEntryHeaderInfoType;

typedef struct _InsSpecificDataTagHeaderInfoType {
  int32_t hdr_offset;                        /** Offset in specific data trailer header */
  InsFileSpecificDataTagHeaderType *hdr;     /** Pointer to original file specific data tag header in trailer buffer */
  const uint8_t* data;                       /** Pointer to tag data in trailer buffer */
} InsSpecificDataTagHeaderInfoType;

typedef vector_t(InsSpecificDataTagHeaderInfoType) InsSpecificDataTagHeaderInfoVector;
typedef vector_t(InsTrailerEntryHeaderInfoType) InsTrailerEntryHeaderInfoVector;

/** Specific header tag types */
enum InsFileSpecificHeaderTagTypes {
  kInsFileSpecificHeaderTagTypeSerial          = 0x0A,
  kInsFileSpecificHeaderTagTypeModel           = 0x12,
  kInsFileSpecificHeaderTagTypeFirmware        = 0x1A,
  kInsFileSpecificHeaderTagTypeOffset          = 0x2A,
  kInsFileSpecificHeaderTagTypeUnknown         = 0xFF
};

struct InsHdrSpecificTagNameInfoType {
  uint8_t type;
  const char* name;
};

const struct InsHdrSpecificTagNameInfoType kInsSpecificTagNameInfos[] = {
  { kInsFileSpecificHeaderTagTypeSerial,          "serial" },
  { kInsFileSpecificHeaderTagTypeFirmware,        "firmware" },
  { kInsFileSpecificHeaderTagTypeModel,           "model" },
  { kInsFileSpecificHeaderTagTypeOffset,          "stitching offset" },
  { kInsFileSpecificHeaderTagTypeUnknown,         "unknown" }   /* 0xFF must be last item */
};

/**
 * \brief    Get file size help function
 * \param    file   [in]   File handle
 * \return   File size in bytes
 */
int64_t get_file_size(FILE* file) {
  fseek(file, 0, SEEK_END);
  int64_t file_length = _ftelli64(file);
  return file_length;
}

/**
 * \brief    Return tag name by tag type code
 * \param    type_code      Tag type code
 * \return   Pointer to zero-terminated string with tag name
 */
const char* ins_get_header_field_name(uint8_t type_code) {
  int i;
  for (i = 0; ; i++) {
    if (kInsSpecificTagNameInfos[i].type == kInsFileSpecificHeaderTagTypeUnknown)
      return kInsSpecificTagNameInfos[i].name;

    if (kInsSpecificTagNameInfos[i].type == type_code)
      return kInsSpecificTagNameInfos[i].name;
  }
}

/** 
 * \brief    Check file signature and read minimal header (72 bytes)
 * \param    file                 [in]  Input file handle
 * \param    out_minimal_header   [out] Output buffer, function stores minimal header here
 * \return   0 - success, negative - fail
 */
int ins_find_and_read_minimal_header(FILE* file, uint8_t out_minimal_header[kInsFileMinHeaderLength]) {
  int64_t file_length = get_file_size(file);

  if (file_length < kInsFileMinHeaderLength)
    return -1;

  char ins_signature_buffer[kInsFileSignatureLength];

  fseek(file, -kInsFileSignatureLength, SEEK_END);
  size_t actual_read = fread(ins_signature_buffer, 1, kInsFileSignatureLength, file);

  if (actual_read != kInsFileSignatureLength)
    return -1;

  if (memcmp(ins_signature_buffer, kInsFileSignature, kInsFileSignatureLength))
    return -1;

  fseek(file, -kInsFileMinHeaderLength, SEEK_END);

  actual_read = fread(out_minimal_header, 1, kInsFileMinHeaderLength, file);
  if (actual_read != kInsFileMinHeaderLength)
    return -1;

  return 0;
}

/**
 * \brief    Decode trailer specific header
 * \param    hdr_data           [in]  Input trailer header data
 * \param    hdr_size           [in]  Input trailer header data size
 * \param    out_hdr_elements   [out] Vector for save found specific header tags
 * \param    out_tail_ptr       [out] Function saves pointer to tail. Pointed to hdr_data buffer
 * \param    out_tail_size      [out] Function saves tail size
 * \return   0 - success, negative - fail
 */
int ins_decode_trailer_specific_header(
  const uint8_t* hdr_data, 
  int32_t hdr_size, 
  InsSpecificDataTagHeaderInfoVector* out_hdr_elements, 
  const uint8_t** out_tail_ptr, 
  int* out_tail_size) {

  const int kMaxTagsCount = 4; /* Usually specific header contains 4 tags and tail data */
  int32_t position = 0;
  int hdr_elements_count = 0;

  int bytes_left = hdr_size - position;

  for(int i=0; i<kMaxTagsCount; i++) {
    InsSpecificDataTagHeaderInfoType hdr_elem;
    int32_t element_offset = position;

    InsFileSpecificDataTagHeaderType* tag_hdr = (InsFileSpecificDataTagHeaderType*)(hdr_data + position);
    position += sizeof(InsFileSpecificDataTagHeaderType);

    /* check header */
    if (tag_hdr->data_size > bytes_left)
      return -1; /* tag size greater than bytes left in header buffer */

    hdr_elem.hdr = tag_hdr;
    hdr_elem.hdr_offset = element_offset;
    hdr_elem.data = hdr_data+position;

    vector_push(InsSpecificDataTagHeaderInfoType, out_hdr_elements, hdr_elem);
    ++hdr_elements_count;

    position += tag_hdr->data_size;
    bytes_left = hdr_size - position;

    if (position >= hdr_size)
      break;
  }

  *out_tail_size = bytes_left;
  *out_tail_ptr = hdr_data + position;

  return hdr_elements_count;
}

/**
 * \brief    Decode trailer data, save trailer entries information
 * \param    trailer_data    [in]  Trailer data buffer
 * \param    trailer_info    [in]  Trailer information structure
 * \param    out_trailer_hdr_iterms  [out]  Function saves found trailer entries information to vector
 * \return   0 - success, negative - fail
 */
int ins_decode_trailer_data(
  const uint8_t* trailer_data, 
  const InsFileTrailerHeaderType* trailer_info, 
  InsTrailerEntryHeaderInfoVector* out_trailer_hdr_items) {

  const InsFileTrailerEntryHeaderType* trailer_hdr;
  uint32_t trailer_read_pos = kInsFileMinHeaderLength;

  // find trailer headers
  while (trailer_read_pos < trailer_info->trailer_len) {
    const uint8_t* trailer_data_ptr = trailer_data + trailer_info->trailer_len - trailer_read_pos - sizeof(InsFileTrailerEntryHeaderType);
    InsTrailerEntryHeaderInfoType hdr_info;

    trailer_hdr = (const InsFileTrailerEntryHeaderType*)trailer_data_ptr;

    hdr_info.hdr = trailer_hdr;
    hdr_info.trailer_offset_to_data = trailer_info->trailer_len - trailer_read_pos - trailer_hdr->length - sizeof(InsFileTrailerEntryHeaderType);

    vector_push(InsTrailerEntryHeaderInfoType, out_trailer_hdr_items, hdr_info);
    trailer_read_pos += trailer_hdr->length + sizeof(InsFileTrailerEntryHeaderType);
  }

  /* check that all headers was processed successfully: 
     calculated size must be equal to length from trailer information */
  if (trailer_read_pos != trailer_info->trailer_len)
    return -1;

  return 0;
}

/**
 * \brief    Free trailer buffer
 * \param    trailer_buf    [in]   Trailer buffer allocated by function ins_read_allocate_trailer
 */
void ins_free_trailer_buffer(uint8_t* trailer_buf) {
  free(trailer_buf);
}

/**
 * \brief    Find trailer size, allocate buffer and read full file trailer to allocated buffer
 * \param    file   File descriptor
 * \param    out_trailer_data   [out]  Function saves pointer to allocated buffer with trailer data
 * \param    out_trailer_info   [out]  Function saves information structure about trailer
 * \return   0 - success, negative value - error
 */
int ins_read_allocate_trailer(FILE* file, uint8_t** out_trailer_data, InsFileTrailerHeaderType* out_trailer_info) {
  uint8_t minimal_ins_header_data[kInsFileMinHeaderLength];
  size_t actual_read;

  /* 1. Read and check minimal trailer information
     2. Take full trailer size from minimal header, allocate buffer and read full trailer */

  if (ins_find_and_read_minimal_header(file, minimal_ins_header_data) < 0)
    return -1;  /* minimal header not found */
  
  InsFileTrailerHeaderType* trailer_info = (InsFileTrailerHeaderType*)(
    minimal_ins_header_data + kInsFileMinHeaderLength - kInsFileSignatureLength - sizeof(InsFileTrailerHeaderType));

  uint8_t* trailer_data = (uint8_t*)malloc(trailer_info->trailer_len);
  fseek(file, -((int32_t)trailer_info->trailer_len), SEEK_END);

  /* read full trailer data */
  actual_read = fread(trailer_data, 1, trailer_info->trailer_len, file);
  if (trailer_info->trailer_len > actual_read)
    return -2; // cannot read data

  *out_trailer_info = *trailer_info;
  *out_trailer_data = trailer_data;

  return 0;
}

/**
 * \brief    Change stitching offset tag value in specific Insta360 trailer entry. Function rebuilds trailer header, 
 *           new header buffer will be allocated and must be freed by caller
 * \param    in_trailer_hdr       [in]  Buffer contains specific trailer entry header data
 * \param    in_trailer_hdr_size  [in]  Size of specific trailer entry header data
 * \param    new_offset           [in]  Zero-terminated string contains new stitching offset value
 * \param    out_trailer_hdr      [out] Output trailer header buffer (allocated)
 * \param    out_trailer_hdr_size [out] Function saves output trailer header size
 * \return   0 - success, negative - fail
 */
int ins_change_stitching_offset(
  const uint8_t* in_trailer_hdr, 
  int in_trailer_hdr_size, 
  const char* new_offset, 
  uint8_t** out_trailer_hdr, 
  int* out_trailer_hdr_size) {

  InsSpecificDataTagHeaderInfoVector spec_hdr_elements;
  vector_init(&spec_hdr_elements);

  const uint8_t* tail_ptr;
  int tail_size;

  if (ins_decode_trailer_specific_header(in_trailer_hdr, in_trailer_hdr_size, &spec_hdr_elements, &tail_ptr, &tail_size) < 0) {
    printf("Process header error, wrong file format\n");
    return -3;
  }

  /* find current stitching offset size */
  int stitching_offset_current_size = 0;

  for (int i = 0; i < vector_size(&spec_hdr_elements); i++) {
    InsSpecificDataTagHeaderInfoType* spec_hdr_elem = &vector_at(&spec_hdr_elements, i);
    if (spec_hdr_elem->hdr->type_code == kInsFileSpecificHeaderTagTypeOffset) {
      stitching_offset_current_size = spec_hdr_elem->hdr->data_size;
      break;
    }
  }

  /* rebuild header with new offset */
  int new_offset_size = (int)strlen(new_offset);

  int new_header_size = in_trailer_hdr_size - stitching_offset_current_size + new_offset_size;

  uint8_t* new_trailer_hdr = (uint8_t*)malloc(new_header_size);
  memset(new_trailer_hdr, 0, new_header_size);
  int new_trailer_hdr_pos = 0;

  for (int i = 0; i < vector_size(&spec_hdr_elements); i++) {
    InsSpecificDataTagHeaderInfoType* spec_hdr_elem = &vector_at(&spec_hdr_elements, i);

    InsFileSpecificDataTagHeaderType* new_spec_data_tag_hdr = (InsFileSpecificDataTagHeaderType*)(
      new_trailer_hdr + new_trailer_hdr_pos);

    new_spec_data_tag_hdr->type_code = spec_hdr_elem->hdr->type_code;
    new_trailer_hdr_pos += sizeof(InsFileSpecificDataTagHeaderType);

    if (spec_hdr_elem->hdr->type_code == kInsFileSpecificHeaderTagTypeOffset) {
      /* tag type is kInsHeaderFieldTypeStitchingOffset, replace with new value */
      new_spec_data_tag_hdr->data_size = new_offset_size;
      memcpy(new_trailer_hdr + new_trailer_hdr_pos, new_offset, new_offset_size);
      new_trailer_hdr_pos += new_offset_size;
    }  else {
      /* tag type is not kInsHeaderFieldTypeStitchingOffset, copy it as is */
      new_spec_data_tag_hdr->data_size = spec_hdr_elem->hdr->data_size;
      memcpy(new_trailer_hdr + new_trailer_hdr_pos, spec_hdr_elem->data, spec_hdr_elem->hdr->data_size);
      new_trailer_hdr_pos += spec_hdr_elem->hdr->data_size;
    }
  }

  /* file did not contain stitching offset parameter - add it */
  if (stitching_offset_current_size == 0) {
    InsFileSpecificDataTagHeaderType new_tag_hdr;
    new_tag_hdr.type_code = kInsFileSpecificHeaderTagTypeOffset;
    new_tag_hdr.data_size = new_offset_size;

    memcpy(new_trailer_hdr + new_trailer_hdr_pos, &new_tag_hdr, sizeof(InsFileSpecificDataTagHeaderType));
    new_trailer_hdr_pos += sizeof(InsFileSpecificDataTagHeaderType);

    memcpy(new_trailer_hdr + new_trailer_hdr_pos, new_offset, new_offset_size);
    new_trailer_hdr_pos += new_offset_size;
  }

  memcpy(new_trailer_hdr + new_trailer_hdr_pos, tail_ptr, tail_size);

  *out_trailer_hdr = new_trailer_hdr;
  *out_trailer_hdr_size = new_header_size;

  return 0;
}

/** Show info mode */
int run_show_info(const char* param_file_in) {
  printf("Use file: %s\n", param_file_in);

  FILE* file = fopen(param_file_in, "rb");
  if (!file) {
    printf("Cannot open file\n");
    return -2;
  }

  uint8_t* trailer_data;
  InsFileTrailerHeaderType trailer_info;

  if (ins_read_allocate_trailer(file, &trailer_data, &trailer_info) < 0) {
    printf("Cannot decode file header\n");
    return -3;
  }

  printf("INS trailer version: %d, length: %d\n", trailer_info.trailer_version, trailer_info.trailer_len);

  InsTrailerEntryHeaderInfoVector trailer_hdr_infos;
  vector_init(&trailer_hdr_infos);

  if (ins_decode_trailer_data(trailer_data, &trailer_info, &trailer_hdr_infos) < 0) {
    printf("Cannot decode trailer header\n");
    ins_free_trailer_buffer(trailer_data);
    return -4;
  }

  printf("Trailer decoded successfully, entrys count %d\n", vector_size(&trailer_hdr_infos));

  InsSpecificDataTagHeaderInfoVector spec_hdr_elements;
  vector_init(&spec_hdr_elements);

  const uint8_t* tail_ptr;
  int tail_size;

  for (int i = 0; i < vector_size(&trailer_hdr_infos); i++) {
    InsTrailerEntryHeaderInfoType* hdr_info = &vector_at(&trailer_hdr_infos, i);

    printf("Tail entry header found, type %.4X, size %d, offset in trailer %d\n", 
      hdr_info->hdr->type, hdr_info->hdr->length, (int)hdr_info->trailer_offset_to_data);

    switch (hdr_info->hdr->type) {
    case 0x0101:
      printf("Found specific trailer header, type %.4X size %d\n", hdr_info->hdr->type, hdr_info->hdr->length);

      if (0 > ins_decode_trailer_specific_header(trailer_data + hdr_info->trailer_offset_to_data, 
                                                 hdr_info->hdr->length, 
                                                 &spec_hdr_elements, 
                                                 &tail_ptr, 
                                                 &tail_size)) {
        printf("Process header error, wrong file format\n");
        ins_free_trailer_buffer(trailer_data);
        return -3;
      }

      printf("Specific trailer decoded successully, tags count %d, tail size %d\n", vector_size(&spec_hdr_elements), tail_size);

      for (int j = 0; j < vector_size(&spec_hdr_elements); j++) {
        InsSpecificDataTagHeaderInfoType* spec_hdr = &vector_at(&spec_hdr_elements, j);

        printf("*** Tag type: %.2X (%s), size: %d, hdr offset: %d\n", 
          spec_hdr->hdr->type_code, 
          ins_get_header_field_name(spec_hdr->hdr->type_code), 
          spec_hdr->hdr->data_size, 
          spec_hdr->hdr_offset);

        printf("    Data: ");

        // show tag bytes
        for (int k = 0; k < spec_hdr->hdr->data_size; k++)
          printf("%c", spec_hdr->data[k]);

        printf("\n");
      }

      break;
    default:
      printf("Found trailer header type %.4X size %d\n", hdr_info->hdr->type, hdr_info->hdr->length);
      break;
    }
  }

  ins_free_trailer_buffer(trailer_data);
  fclose(file);
  printf("Done!\n");

  return 0;
}

/** Change stitching offset mode */
int run_change_stitching_offset(const char* param_file_in, const char* param_file_out, const char* param_new_offset) {
  uint8_t* trailer_data;
  InsFileTrailerHeaderType trailer_info;
  char copy_buffer[kCopyBufferSize];

  printf("Use file: %s\n", param_file_in);

  FILE* file = fopen(param_file_in, "rb");
  if (!file) {
    printf("Cannot open file\n");
    return -2;
  }

  if (ins_read_allocate_trailer(file, &trailer_data, &trailer_info) < 0) {
    printf("Cannot decode file header\n");
    fclose(file);
    return -3;
  }

  printf("INS trailer version: %d, length: %d\n", trailer_info.trailer_version, trailer_info.trailer_len);

  InsTrailerEntryHeaderInfoVector trailer_hdr_infos;
  vector_init(&trailer_hdr_infos);

  if (ins_decode_trailer_data(trailer_data, &trailer_info, &trailer_hdr_infos) < 0) {
    printf("Cannot decode trailer header\n");
    ins_free_trailer_buffer(trailer_data);
    fclose(file);
    return -4;
  }

  printf("Trailer decoded successfully, entrys count %d\n", vector_size(&trailer_hdr_infos));

  InsSpecificDataTagHeaderInfoVector spec_hdr_elements;
  vector_init(&spec_hdr_elements);

  uint8_t* new_spec_trailer_hdr;
  int new_spec_trailer_size;

  for (int i = 0; i < vector_size(&trailer_hdr_infos); i++) {
    InsTrailerEntryHeaderInfoType* hdr_info = &vector_at(&trailer_hdr_infos, i);

    switch (hdr_info->hdr->type) {
    case 0x0101:
      printf("Found specific trailer header type %.4X size %d, change offset data\n", hdr_info->hdr->type, hdr_info->hdr->length);
      if (0 > ins_change_stitching_offset(
        trailer_data + hdr_info->trailer_offset_to_data,
        hdr_info->hdr->length,
        param_new_offset,
        &new_spec_trailer_hdr,
        &new_spec_trailer_size)) {

        printf("ERROR: cannot change stitching offset\n");
        ins_free_trailer_buffer(trailer_data);
        fclose(file);
        return -7;
      }

      printf("Offset changed successfully, old header size %d, new size %d\n", hdr_info->hdr->length, new_spec_trailer_size);
      break;
    default:
      printf("Found trailer header type %.4X size %d\n", hdr_info->hdr->type, hdr_info->hdr->length);
      break;
    }
  }

  /* rebuild file */
  printf("Rebuilding file structure...\n");

  int64_t file_in_size = get_file_size(file);
  fseek(file, 0, SEEK_SET);

  FILE* file_out = fopen(param_file_out, "wb+");
  if (!file_out) {
    printf("Cannot create output file: %s\n", param_file_out);
    ins_free_trailer_buffer(trailer_data);
    ins_free_trailer_buffer(new_spec_trailer_hdr);
    fclose(file);
    return -5;
  }

  int write_pos = 0;

  printf("Copy media data %d bytes...\n", (int)(file_in_size - trailer_info.trailer_len));

  int error = 0;

  while (write_pos < file_in_size - trailer_info.trailer_len) {
    int bytes_to_copy = (int)(file_in_size - trailer_info.trailer_len - write_pos);
    if (bytes_to_copy > sizeof(copy_buffer))
      bytes_to_copy = sizeof(copy_buffer);

    if (fread(copy_buffer, 1, bytes_to_copy, file) != bytes_to_copy) {
      printf("Read file error\n");
      error = 1;
      break;
    }

    if (fwrite(copy_buffer, 1, bytes_to_copy, file_out) != bytes_to_copy) {
      printf("Write file error\n");
      error = 1;
      break;
    }

    write_pos += bytes_to_copy;
  }

  if (error) {
    ins_free_trailer_buffer(trailer_data);
    ins_free_trailer_buffer(new_spec_trailer_hdr);
    fclose(file_out);
    fclose(file);
    return -6;
  }

  int total_new_trailer_size = 0;

  /* rebuild trailer */
  for (int i = vector_size(&trailer_hdr_infos) - 1; i >= 0; i--) {
    InsTrailerEntryHeaderInfoType* hdr_info = &vector_at(&trailer_hdr_infos, i);

    InsFileTrailerEntryHeaderType new_trailer_elem_hdr;
    new_trailer_elem_hdr.type = hdr_info->hdr->type;

    switch (hdr_info->hdr->type) {
    case 0x0101:
      printf("Save rebuilt trailer header type %.4X, size %d bytes\n", 
        hdr_info->hdr->type, hdr_info->hdr->length);

      new_trailer_elem_hdr.length = new_spec_trailer_size;
      fwrite(new_spec_trailer_hdr, 1, new_spec_trailer_size, file_out);
      break;
    default:
      printf("Copy trailer header type %.4X, size %d bytes\n", 
        hdr_info->hdr->type, hdr_info->hdr->length);

      new_trailer_elem_hdr.length = hdr_info->hdr->length;
      fwrite(trailer_data + hdr_info->trailer_offset_to_data, 1, hdr_info->hdr->length, file_out);
      break;
    }

    fwrite(&new_trailer_elem_hdr, 1, sizeof(InsFileTrailerEntryHeaderType), file_out);
    total_new_trailer_size += new_trailer_elem_hdr.length + sizeof(InsFileTrailerEntryHeaderType);
  }

  int zero_padding_size = kInsFileMinHeaderLength - kInsFileSignatureLength - sizeof(InsFileTrailerHeaderType);

  for (int i = 0; i < zero_padding_size; i++) {
    char zero = 0;
    fwrite(&zero, 1, 1, file_out);
  }

  InsFileTrailerHeaderType new_trailer_hdr;
  new_trailer_hdr.trailer_version = trailer_info.trailer_version;
  new_trailer_hdr.trailer_len = total_new_trailer_size + kInsFileMinHeaderLength;
  fwrite(&new_trailer_hdr, 1, sizeof(InsFileTrailerHeaderType), file_out);

  fwrite(kInsFileSignature, 1, kInsFileSignatureLength, file_out);

  ins_free_trailer_buffer(trailer_data);
  ins_free_trailer_buffer(new_spec_trailer_hdr);

  vector_destroy(&spec_hdr_elements);
  vector_destroy(&trailer_hdr_infos);

  fflush(file_out);
  fclose(file_out);

  printf("Done!\n");
  return 0;
}


int main(int argc, char* argv[]) {
  printf("Insta360 file tool\n");

  if (argc < 3) {
    printf("Insufficient arguments\n");
    printf("USAGE:\n");
    printf("  ins_file_tool -s <file.insv/insp>                  Show information\n");
    printf("  ins_file_tool -c <file> <file_out> <new_offset>    Change stitching offset\n");

    return -1;
  }

  const char* param_mode = argv[1];
  const char* param_file_in = argv[2];

  if (!strcmp(param_mode, "-s"))
    return run_show_info(param_file_in);

  if (!strcmp(param_mode, "-c")) {
    if (argc < 5) {
      printf("Insufficient arguments for mode -c\n");
    }
    const char* param_file_out = argv[3];
    const char* param_new_offset = argv[4];

    return run_change_stitching_offset(param_file_in, param_file_out, param_new_offset);
  }

  printf("Invalid mode\n");
  return -1;
}
