/*
 * Copyright (c) 2013-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once
#include <cstdlib>
#include <cstdint>

/* H264 standard, Table 7-1, "NAL unit type codes" */
enum class NalType {
    /* Unspecified NAL, for app use */
    UNSPECIFIED_ZERO = 0,
    /* Image slices */
    NON_IDR_SLICE = 1,
    PARTITION_A_SLICE = 2,
    PARTITION_B_SLICE = 3,
    PARTITION_C_SLICE = 4,
    IDR_SLICE = 5,
    /* SEI NAL */
    SUPPLEMENTAL_ENHANCED_INFORMATION = 6,
    /* Parameter sets */
    SEQUENCE_PARAMETER_SET = 7,
    SPS = 7,
    PICTURE_PARAMETER_SET = 8,
    PPS = 8,
    /* Delimiters and filler */
    ACCESS_UNIT_DELIMITER = 9,
    END_OF_SEQUENCE = 10,
    END_OF_STREAM = 11,
    FILLER = 12,
    /* Reserved range - for future extensions */
    FIRST_RESERVED_TYPE = 13,
    LAST_RESERVED_TYPE = 23,
    /* Unspecified range - for app use */
    FIRST_UNSPECIFIED_TYPE = 24,
    LAST_UNSPECIFIED_TYPE = 31
};

enum class H264Profile {
    // Standard profiles
    BASELINE = 66,
    MAIN = 77,
    EXTENDED = 88,
    // Fidelity Range Extensions profiles
    FRE_HIGH_PROFILE = 100, // YUV 4:2:0/8 "High".
    FRE_HIGH_PROFILE_10 = 110, // YUV 4:2:0/10 "High 10"
    FRE_HIGH_PROFILE_422 = 122, // YUV 4:2:2/10 "High 4:2:2"
    FRE_HIGH_PROFILE_444 = 244, // YUV 4:4:4/14 "High 4:4:4"
    FRE_CAVLC_444 = 44 // YUV 4:4:4/14 "CAVLC 4:4:4"
};

/* Table 7-3 contains more slice types, but they wrap around after 4 (i.e.
 5 is again P type, 6 is B and so on). Those wrapped around carry some extra
 information we don't need as of now */
enum class H264SliceType {
    P = 0,
    B = 1,
    I = 2,
    SP = 3,
    SI = 4
};

/* H264 standard: 7.4.2.1 on SPS
 "seq_parameter_set_id identifies the sequence parameter set that is referred
 to by the picture parameter set. The value of seq_parameter_set_id shall be in
 the range of 0 to 31, inclusive" */
#define H264_NUMBER_OF_SPS_ALLOWED 32

/* H264 standard: 7.4.2.2 on PPS:
 "pic_parameter_set_id identifies the picture parameter set that is referred to
 in the slice header. The value of pic_parameter_set_id shall be in the range of
 0 to 255, inclusive." */
#define H264_NUMBER_OF_PPS_ALLOWED 256

struct SpsNalInfo {
    /*** First part - what is read from stream ***/

    /* Profile, level, SPS id */
    H264Profile profile_idc;
    uint32_t level_idc;
    uint32_t seq_parameter_set_id;

    /* Type of picture order count encoding (0, 1 or 2) */
    uint32_t pic_order_cnt_type;

    /* See ITU-T Rec. H.264 (05/2003, 7.4.2.1 Sequence parameter set RBSP semantics:
     * num_ref_frames specifies the maximum number of short-term and long-term reference frames,
     * complementary reference field pairs, and non-paired reference fields that may be used by the
     * decoding process for inter prediction of any picture in the sequence. num_ref_frames also
     * determines the size of the sliding window operation as specified in subclause 8.2.5.3. The
     * value of num_ref_frames shall be in the range of 0 to MaxDpbSize (as specified in subclause
     * A.3.1), inclusive.
     *
     * The extremely-simplified (possibly incomplete) description is:
     * if this is more than 1, the stream will have B frames.
     */
    uint32_t num_ref_frames;

    /* This will probably be needed should we do our own reordering */
    uint32_t log2_max_frame_num_minus4;

    uint32_t log2_max_pic_order_cnt_lsb_minus4;
    bool delta_pic_order_always_zero_flag;
    uint32_t offset_for_non_ref_pic;
    uint32_t offset_for_top_to_bottom_field;
    uint32_t num_ref_frames_in_pic_order_cnt_cycle;
    uint32_t offset_for_ref_frame[256];

    /* Width and height in mbs/maps, be careful to use computed dimensions at the end of this
     struct and DO NOT interpret these "by hand"! */
    bool frame_mbs_only_flag, mb_adaptive_frame_field_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;

    /* Cropping */
    bool frame_cropping_flag;
    uint32_t frame_crop_left_offset;
    uint32_t frame_crop_right_offset;
    uint32_t frame_crop_top_offset;
    uint32_t frame_crop_bottom_offset;

    /*** Second part - what is computed using values read from stream, these will
     get computed by at_h264_get_sps_info ***/

    /* Computed dimensions of frames. All values in luma component pels (chromas will be /2 unless
     some form of 444 YUV is used), _and_ these are frame sizes (H264 can carry noninterlaced
     "frames" or interlaced "fields" - in latter case dimensions will be given in "frame" dimensions
     still. Not that we should seriously take into account dealing with interlaced video! */

    /* Internally, H264 can only carry complete blocks. So if video dimensions are not multiples
     of that block size, it should get padded to next blocks size multiply, usually by repeating
     last pel values, to avoid creating very high frequencies and screwing the compression. This
     is padded size:
     */
    size_t padded_frame_width, padded_frame_height;

    /* True picture size, if padded, may be smaller than padded frame dimensions. This is what one
     really wants to display.
     WARNING: if true dimensions != padded dimensions one should use crop parameters below -
     there is no guarantee that padding will be at right or bottom */
    size_t true_frame_width, true_frame_height;

    /* Crop offsets. It will probably be convenient to use just two of these, and true frame
     dimensions when displaying, however all four are computed because different display engines
     may use different coordinate systems */
    size_t true_crop_offset_left, true_crop_offset_right,
        true_crop_offset_bottom, true_crop_offset_top;
};

struct PpsNalInfo {
    uint32_t pic_parameter_set_id;
    uint32_t seq_parameter_set_id;
    bool entropy_coding_mode_flag;
    bool pic_order_present_flag;
    uint32_t num_slice_groups_minus1;
    uint32_t slice_group_map_type;
    uint32_t pic_size_in_map_units_minus1;
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;
    bool weighted_pred_flag;
    uint32_t weighted_bipred_idc;
    bool redundant_pic_cnt_present_flag;
};

struct SliceHeaderInfo {
    /* Original values */
    uint32_t ref_nal_idc;
    NalType nal_unit_type;
    uint32_t slice_type;
    uint32_t pic_parameter_set_id;
    uint32_t frame_num;
    uint32_t idr_pic_id;

    uint32_t pic_order_cnt_lsb;
    uint32_t delta_pic_order_cnt_bottom;
    uint32_t delta_pic_order_cnt[2];
    uint32_t redundant_pic_cnt;

    // TODO: these stem from PPS and are optionally overridden in SliceHeader?
    uint32_t num_ref_idx_l0_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1;

    bool field_pic_flag;
    bool bottom_field_flag;

    /* Generated values */
    H264SliceType h264_slice_type;
    bool had_memory_management_control_operation_equal_to_5;
};

bool at_h264_get_nal_type(const unsigned char *data, size_t size, NalType &out_nal_type);
bool at_h264_get_sps_info(const unsigned char *data, size_t size, SpsNalInfo &out_sps_info);
bool at_h264_get_pps_info(const unsigned char *data, size_t size, PpsNalInfo &out_pps_info);
/* This function reads just enough of slice header to fill in
 pic_parameter_set_id field. Caller needs this field to find proper SPS/PPS and
 call "full" version of function then (below) */
bool at_h264_get_initial_slice_header_info(const unsigned char *data, size_t size,
                                           SliceHeaderInfo &slice_header_info);
bool at_h264_get_full_slice_header_info(const unsigned char *data, size_t size,
                                        const SpsNalInfo &sps, const PpsNalInfo &pps,
                                        SliceHeaderInfo &slice_header_info);
const unsigned char *at_h264_next_start_code(const unsigned char *ptr, const unsigned char *limit);
const char *at_h264_slice_type_description(int type);
