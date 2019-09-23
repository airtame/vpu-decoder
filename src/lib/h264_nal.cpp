/*
 * Copyright (c) 2013-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "h264_bitstream.hpp"
#include "h264_nal.hpp"
#include <cassert>
#include <math.h>
#include <stdio.h>

static bool at_h264_ignore_sps_fre_scaling_list(H264Bitstream &bs_parser, size_t size);

bool at_h264_get_nal_type(const unsigned char *data, size_t size, NalType &out_nal_type)
{
    if (size < 4) {
        return false;
    }

    const unsigned char *nal_data = data;
    unsigned char nal_unit_type_mask = 0b00011111; // First 5 lsb bits
    size_t zero_byte_count = 0;
    while (nal_data[0] == 0x00) {
        nal_data++;
        zero_byte_count++;
    }
    // Check for NAL start pattern. if not a NAL, return -1
    if ((zero_byte_count != 3 && zero_byte_count != 2) || nal_data[0] != 0x01) {
        return false;
    }

    // Go to the byte that contains nal type
    nal_data++;

    // Return the nal_unit_type value
    unsigned char nal_unit_type = nal_data[0] & nal_unit_type_mask;

    // TODO: we now know all the NAL types, but I don't want to break
    // old code, so for now I just change constant names, but will be
    // back here if new code works
    switch (nal_unit_type) {
        case 0x01:
            out_nal_type = NalType::NON_IDR_SLICE;
            return true;
        case 0x05:
            out_nal_type = NalType::IDR_SLICE;
            return true;
        case 0x07:
            out_nal_type = NalType::SPS;
            return true;
        case 0x08:
            out_nal_type = NalType::PPS;
            return true;
    }
    return false;
}

#define RETURN_IF_ERROR(bits)                                                                  \
do {                                                                                           \
    assert(bits.error == false);                                                               \
    if (bits.error)                                                                            \
        return false;                                                                          \
} while (false)

bool at_h264_get_sps_info(const unsigned char *data, size_t size, SpsNalInfo &sps_info)
{
    // TODO: use bs_parser all along
    H264Bitstream::Result bits;

    /* Skip the NAL sync units (0x00000001 or 0x000001 pattersn) */
    while (*data == 0x00) {
        data++;
        size--;
    }
    data++;
    size--;

    H264Bitstream bs_parser(data, size);
    /* Parse the NAL header, according to Table 7.3.1: Nal unit syntax */
    bits = bs_parser.read_un_bits(1); // forbidden_zero_bit
    if (bits.error || bits.value != 0) { // the forbidden_zero_bit should always be 0
        return false;
    }
    bits = bs_parser.read_un_bits(2); // nal_ref_idc
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(5); // nal_unit_type
    if (bits.error || bits.value != 0x7) {
        return false;
    }

    /* In theory, we should parse the NAL RBSP, according to Table 7.3.2.1: "
     * Sequence parameter set RBSP syntax", but there are Fidelity Range Extensions
     * we have to take care of, too. This was written by following reference
     * software for H264:
     */
    bits = bs_parser.read_un_bits(8); // profile_idc
    RETURN_IF_ERROR(bits);

    /* To avoid problems in future (getting weird dimensions from SPS) return error
     on unsupported profile */
    switch ((H264Profile)bits.value) {
        case H264Profile::BASELINE:
        case H264Profile::MAIN:
        case H264Profile::EXTENDED:
        case H264Profile::FRE_HIGH_PROFILE:
        case H264Profile::FRE_HIGH_PROFILE_10:
        case H264Profile::FRE_HIGH_PROFILE_422:
        case H264Profile::FRE_HIGH_PROFILE_444:
        case H264Profile::FRE_CAVLC_444:
            /* This is ok, known value */
            sps_info.profile_idc = (H264Profile)bits.value;
            break;
        default:
            /* Parse error, sorry */
            return false;
    }

    bits = bs_parser.read_un_bits(8); // constraints flags and 5 reserved bits
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(8); // level_idc
    RETURN_IF_ERROR(bits);
    sps_info.level_idc = bits.value;

    bits = bs_parser.read_uev_bits(); // seq_parameter_set_id
    RETURN_IF_ERROR(bits);
    sps_info.seq_parameter_set_id = bits.value;

    /* WARNING: this is where ordinary and Fidelity Extensions Profiles diverge,
     consult H.264/AVC Reference Software for details (can be found by googling
     any of following flag names */
    if ((H264Profile::FRE_HIGH_PROFILE == sps_info.profile_idc)
        || (H264Profile::FRE_HIGH_PROFILE_10 == sps_info.profile_idc)
        || (H264Profile::FRE_HIGH_PROFILE_422 == sps_info.profile_idc)
        || (H264Profile::FRE_HIGH_PROFILE_444 == sps_info.profile_idc)
        || (H264Profile::FRE_CAVLC_444 == sps_info.profile_idc)) {
        bits = bs_parser.read_uev_bits(); // chroma_format_idc
        RETURN_IF_ERROR(bits);
        int chroma_format_idc = bits.value;
        /* So I'd really like to create nice enum for that shit, but I can
         find no specs for Fidelity Range Extensions, so following the code.
         It looks like 3 is YUV444, and then one should fo this */
        if (3 == chroma_format_idc) {
            bits = bs_parser.read_un_bits(1); // separate_colour_plane_flag
            RETURN_IF_ERROR(bits);
        }
        bits = bs_parser.read_uev_bits(); // bit_depth_luma_minus8
        RETURN_IF_ERROR(bits);

        bits = bs_parser.read_uev_bits(); // bit_depth_chroma_minus8
        RETURN_IF_ERROR(bits);

        bits = bs_parser.read_un_bits(1); // lossless_qprime_y_zero_flag
        RETURN_IF_ERROR(bits);

        bits = bs_parser.read_un_bits(1); // seq_scaling_matrix_present_flag
        RETURN_IF_ERROR(bits);
        bool seq_scaling_matrix_present_flag = bits.value ? true : false;

        if (seq_scaling_matrix_present_flag) {
            size_t n_scaling_list = (3 == chroma_format_idc) ? 12 : 8;
            for (size_t i = 0; i < n_scaling_list; i++) {
                bits = bs_parser.read_un_bits(1); // seq_scaling_list_present_flag
                RETURN_IF_ERROR(bits);
                bool seq_scaling_list_present_flag = bits.value ? true : false;
                if (seq_scaling_list_present_flag) {
                    /* Now from the reference code syntax it looks like matrices of
                     size proportional to block size (either 16 or 64) are going to
                     be present in apropriate zig-zag scanning order, but it is not
                     like we are ever going to care, or see YUV444 video anyway */
                    if (i < 6) {
                        if (!at_h264_ignore_sps_fre_scaling_list(bs_parser, 16)) {
                            return false;
                        }
                    } else {
                        if (at_h264_ignore_sps_fre_scaling_list(bs_parser, 64)) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    /* Looks like we can return to H264 standard here */
    bits = bs_parser.read_uev_bits(); // log2_max_frame_num_minus4
    RETURN_IF_ERROR(bits);
    sps_info.log2_max_frame_num_minus4 = bits.value;

    bits = bs_parser.read_uev_bits(); // pic_order_cnt_type
    RETURN_IF_ERROR(bits);
    sps_info.pic_order_cnt_type = bits.value;

    if (sps_info.pic_order_cnt_type == 0) {
        bits = bs_parser.read_uev_bits(); // log2_max_pic_order_cnt_lsb_minus4
        RETURN_IF_ERROR(bits);
        sps_info.log2_max_pic_order_cnt_lsb_minus4 = bits.value;
    } else if (sps_info.pic_order_cnt_type == 1) {
        bits = bs_parser.read_un_bits(1); // delta_pic_order_always_zero_flag
        RETURN_IF_ERROR(bits);
        sps_info.delta_pic_order_always_zero_flag = bits.value ? true : false;

        bits = bs_parser.read_sev_bits(); // offset_for_non_ref_pic
        RETURN_IF_ERROR(bits);
        sps_info.offset_for_non_ref_pic = bits.value;

        bs_parser.read_sev_bits(); // offset_for_top_to_bottom_field
        RETURN_IF_ERROR(bits);
        sps_info.offset_for_top_to_bottom_field = bits.value;

        bits = bs_parser.read_uev_bits(); // num_ref_frames_in_pic_order_cnt_cycle
        RETURN_IF_ERROR(bits);
        sps_info.num_ref_frames_in_pic_order_cnt_cycle = bits.value;

        for (uint32_t i = 0; i < sps_info.num_ref_frames_in_pic_order_cnt_cycle; i++) {
            bits = bs_parser.read_sev_bits(); // offset_for_ref_frame[i]
            RETURN_IF_ERROR(bits);
            sps_info.offset_for_ref_frame[i] = bits.value;
        }
    }
    bits = bs_parser.read_uev_bits(); // num_ref_frames
    RETURN_IF_ERROR(bits);
    sps_info.num_ref_frames = bits.value;

    bits = bs_parser.read_un_bits(1); // gaps_in_frame_num_value_allowed_flag
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_uev_bits(); // pic_width_in_mbs_minus1
    RETURN_IF_ERROR(bits);
    sps_info.pic_width_in_mbs_minus1 = bits.value;

    bits = bs_parser.read_uev_bits(); // pic_height_in_map_units_minus1 (NOT MBS_UNITS)
    RETURN_IF_ERROR(bits);
    sps_info.pic_height_in_map_units_minus1 = bits.value;

    bits = bs_parser.read_un_bits(1); // frame_mbs_only_flag
    RETURN_IF_ERROR(bits);
    sps_info.frame_mbs_only_flag = bits.value ? true : false;

    if (!sps_info.frame_mbs_only_flag) {
        bits = bs_parser.read_un_bits(1); // mb_adaptive_frame_field_flag
        RETURN_IF_ERROR(bits);
        sps_info.mb_adaptive_frame_field_flag = bits.value ? true : false;
    }

    bits = bs_parser.read_un_bits(1); // direct_8x8_inference_flag
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(1); // frame_cropping_flag
    RETURN_IF_ERROR(bits);
    sps_info.frame_cropping_flag = bits.value ? true : false;

    if (sps_info.frame_cropping_flag) {
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        sps_info.frame_crop_left_offset = bits.value;
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        sps_info.frame_crop_right_offset = bits.value;
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        sps_info.frame_crop_top_offset = bits.value;
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        sps_info.frame_crop_bottom_offset = bits.value;
    } else {
        sps_info.frame_crop_left_offset = 0;
        sps_info.frame_crop_right_offset = 0;
        sps_info.frame_crop_top_offset = 0;
        sps_info.frame_crop_bottom_offset = 0;
    }

    /* OK, so now we have all that is needed to compute proper dimensions, according
     to 7.4.2.1 formulas from 7-3 on */
    /* Width is easy */
    sps_info.padded_frame_width = (sps_info.pic_width_in_mbs_minus1 + 1) * 16;
    /* Height is different - can be for "frames" (non-interlaced) or "fields" (interlaced
     half of frame). Here proper frame height is computed properly, not that we'll ever
     see interleaved stream */
    sps_info.padded_frame_height = (sps_info.pic_height_in_map_units_minus1 + 1) * 16
        * (2 - (sps_info.frame_mbs_only_flag ? 1 : 0));
    /* Now cropping (*2 is because these are for luma, and chromas can be half the size, so
     offset is simply chroma-pel aligned */
    sps_info.true_crop_offset_left = sps_info.frame_crop_left_offset * 2;
    sps_info.true_crop_offset_right = sps_info.frame_crop_right_offset * 2;
    if (sps_info.frame_mbs_only_flag) {
        /* Frame offsets */
        sps_info.true_crop_offset_bottom = sps_info.frame_crop_bottom_offset * 2;
        sps_info.true_crop_offset_top = sps_info.frame_crop_top_offset * 2;
    } else {
        /* Field values, so double for frame */
        sps_info.true_crop_offset_bottom = sps_info.frame_crop_bottom_offset * 4;
        sps_info.true_crop_offset_top = sps_info.frame_crop_top_offset * 4;
    }
    /* Finally, knowing how much has to be cropped, we can compute true picture dimensions */
    sps_info.true_frame_width = sps_info.padded_frame_width - sps_info.true_crop_offset_left
        - sps_info.true_crop_offset_right;
    sps_info.true_frame_height = sps_info.padded_frame_height - sps_info.true_crop_offset_bottom
        - sps_info.true_crop_offset_top;
    return true;
}

/* "Airtamized" code from H264/AVC reference software */
static bool at_h264_ignore_sps_fre_scaling_list(H264Bitstream &bs_parser, size_t size)
{
    H264Bitstream::Result bits;
    int last_scale = 8, next_scale = 8;
    for (size_t i = 0; i < size; i++) {
        if (next_scale) {
            bits = bs_parser.read_sev_bits();
            if (bits.error) {
                return false;
            }
            int delta_scale = bits.value;
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
    }
    return true;
}

bool at_h264_get_pps_info(const unsigned char *data, size_t size, PpsNalInfo &pps_info)
{
    // TODO: use bs_parser all along
    H264Bitstream::Result bits;

    /* Skip the NAL sync units (0x00000001 or 0x000001 pattersn) */
    while (*data == 0x00) {
        data++;
        size--;
    }
    data++;
    size--;

    H264Bitstream bs_parser(data, size);
    /* Parse the NAL header, according to Table 7.3.1: Nal unit syntax */
    bits = bs_parser.read_un_bits(1); // forbidden_zero_bit
    if (bits.error || bits.value != 0) { // the forbidden_zero_bit should always be 0
        return false;
    }
    bits = bs_parser.read_un_bits(2); // nal_ref_idc
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(5); // nal_unit_type
    if (bits.error || bits.value != 0x8) {
        return false;
    }

    bits = bs_parser.read_uev_bits();
    RETURN_IF_ERROR(bits);
    pps_info.pic_parameter_set_id = bits.value;

    if (pps_info.pic_parameter_set_id >= H264_NUMBER_OF_PPS_ALLOWED) {
        return false;
    }

    bits = bs_parser.read_uev_bits();
    RETURN_IF_ERROR(bits);
    pps_info.seq_parameter_set_id = bits.value;

    if (pps_info.seq_parameter_set_id >= H264_NUMBER_OF_SPS_ALLOWED) {
        return false;
    }

    bits = bs_parser.read_un_bits(1);
    RETURN_IF_ERROR(bits);
    pps_info.entropy_coding_mode_flag = bits.value ? true : false;

    bits = bs_parser.read_un_bits(1);
    RETURN_IF_ERROR(bits);
    pps_info.pic_order_present_flag = bits.value ? true : false;

    bits = bs_parser.read_uev_bits();
    RETURN_IF_ERROR(bits);
    pps_info.num_slice_groups_minus1 = bits.value;

    if (pps_info.num_slice_groups_minus1) {
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        pps_info.slice_group_map_type = bits.value;

        /* Note that lack of handling for slice_group_map_type == 1 here is not
         an omission, this type simply doesn't have any extra fields to read
         here */
        if (0 == pps_info.slice_group_map_type) {
            for (uint32_t i = 0; i <= pps_info.num_slice_groups_minus1; i++) {
                bits = bs_parser.read_uev_bits(); /* run_length_minus_1[i] */
                RETURN_IF_ERROR(bits);
            }
        } else if (2 == pps_info.slice_group_map_type) {
            /* Note that there was "<=" above but here Standard says "<" */
            for (uint32_t i = 0; i < pps_info.num_slice_groups_minus1; i++) {
                bits = bs_parser.read_uev_bits(); /* top_left[i] */
                RETURN_IF_ERROR(bits);
                bits = bs_parser.read_uev_bits(); /* bottom_right[i] */
                RETURN_IF_ERROR(bits);
            }
        } else if ((3 == pps_info.slice_group_map_type)
                   || (4 == pps_info.slice_group_map_type)
                   || (5 == pps_info.slice_group_map_type)) {
            bits = bs_parser.read_un_bits(1); /* slice_group_change_direction_flag */
            RETURN_IF_ERROR(bits);
            bits = bs_parser.read_uev_bits(); /* slice_group_change_rate_minus1 */
            RETURN_IF_ERROR(bits);
        } else if (6 == pps_info.slice_group_map_type) {
            bits = bs_parser.read_uev_bits(); /* pic_size_in_map_units_minus1 */
            RETURN_IF_ERROR(bits);
            pps_info.pic_size_in_map_units_minus1 = bits.value;

            uint32_t n = (uint32_t)::ceil(::log2(pps_info.num_slice_groups_minus1 + 1));
            for (uint32_t i = 0; i <= pps_info.pic_size_in_map_units_minus1; i++) {
                bits = bs_parser.read_un_bits(n);
            }
        }
    }

    bits = bs_parser.read_uev_bits(); /* num_ref_idx_l0_active_minus_1 */
    RETURN_IF_ERROR(bits);
    pps_info.num_ref_idx_l0_active_minus1 = bits.value;

    bits = bs_parser.read_uev_bits(); /* num_ref_idx_l1_active_minus_1 */
    RETURN_IF_ERROR(bits);
    pps_info.num_ref_idx_l1_active_minus1 = bits.value;

    bits = bs_parser.read_un_bits(1); /* weighted_pred_flag */
    RETURN_IF_ERROR(bits);
    pps_info.weighted_pred_flag = bits.value ? true : false;

    bits = bs_parser.read_un_bits(2); /* weighted_bipred_idc */
    RETURN_IF_ERROR(bits);
    pps_info.weighted_bipred_idc = bits.value;

    bits = bs_parser.read_sev_bits(); /* pic_init_qp_minus_26 */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_sev_bits(); /* pic_init_qs_minus_26 */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_sev_bits(); /* chroma_qp_index_offset */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(1); /* deblocking_filter_control_present_flag */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(1); /* constrained_intra_pred_flag */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_un_bits(1); /* redundant_pic_cnt_present_flag */
    RETURN_IF_ERROR(bits);
    pps_info.redundant_pic_cnt_present_flag = bits.value ? true : false;

    return true;
}

static bool at_h264_get_initial_slice_header_info_implementation(H264Bitstream &bs_parser,
                                                                 SliceHeaderInfo &slice_header_info)
{
    /* Skip zeroed out bytes  */
    H264Bitstream::Result bits = bs_parser.read_un_bits(8);
    while (!bits.error && !bits.value) {
        bits = bs_parser.read_un_bits(8);
    }
    RETURN_IF_ERROR(bits);

    /* Byte set to 0x01 should follow */
    if (1 != bits.value) {
        return false;
    }

    /* Parse the NAL header, according to Table 7.3.1: Nal unit syntax */
    bits = bs_parser.read_un_bits(1); /* forbidden_zero_bit */
    if (bits.error || bits.value != 0) { /* forbidden_zero_bit should always be 0 */
        return false;
    }

    bits = bs_parser.read_un_bits(2); /* nal_ref_idc */
    RETURN_IF_ERROR(bits);
    slice_header_info.ref_nal_idc = bits.value;

    bits = bs_parser.read_un_bits(5); /* nal_unit_type */
    RETURN_IF_ERROR(bits);
    slice_header_info.nal_unit_type = (NalType)bits.value;

    bits = bs_parser.read_uev_bits(); /* first_mb_in_slice, ignore */
    RETURN_IF_ERROR(bits);

    bits = bs_parser.read_uev_bits(); /* slice_type */
    RETURN_IF_ERROR(bits);
    slice_header_info.slice_type = bits.value;

    /* slice_type field wraps around, so type 5 is the same as type 0, 6 as 1,
     and so on, but there is some extra info associated with the fact that slice
     type is signalled either way. Thing is, we don't need that information,
     so just "unwrap" it here */
    slice_header_info.h264_slice_type
        = H264SliceType((slice_header_info.slice_type < 5)
            ? slice_header_info.slice_type
            : slice_header_info.slice_type - 5);

    bits = bs_parser.read_uev_bits(); /* pic_parameter_set_id */
    RETURN_IF_ERROR(bits);
    slice_header_info.pic_parameter_set_id = bits.value;

    if (slice_header_info.pic_parameter_set_id >= H264_NUMBER_OF_PPS_ALLOWED) {
        return false;
    }

    return true;
}

bool at_h264_get_initial_slice_header_info(const unsigned char *data, size_t size,
                                           SliceHeaderInfo &slice_header_info)
{
    H264Bitstream bs_parser(data, size);
    return at_h264_get_initial_slice_header_info_implementation(bs_parser, slice_header_info);
}

/* Very same syntax element (save for variable name postfixes occures twice
 and all we want is to skim over it properly. See 7.3.3.1 "Reference picture
 list reordering syntax" */
static bool at_h264_skip_ref_pic_list_reordering(H264Bitstream &bs_parser)
{
    H264Bitstream::Result bits = bs_parser.read_un_bits(1);
    RETURN_IF_ERROR(bits);
    bool ref_pic_list_reordering_flag_lx = bits.value ? true : false;

    if (ref_pic_list_reordering_flag_lx) {
        uint32_t reordering_of_pic_nums_idc;
        do {
            bits = bs_parser.read_uev_bits();
            RETURN_IF_ERROR(bits);
            reordering_of_pic_nums_idc = bits.value;

            if ((0 == reordering_of_pic_nums_idc)
                || (1 == reordering_of_pic_nums_idc)) {
                bits = bs_parser.read_uev_bits(); /* abs_diff_pic_num_minus1 */
                RETURN_IF_ERROR(bits);
            } else if (2 == reordering_of_pic_nums_idc) {
                bits = bs_parser.read_uev_bits(); /* long_term_pic_num */
                RETURN_IF_ERROR(bits);
            }
        } while(3 != reordering_of_pic_nums_idc);
    }
    return true;
}

static bool at_h264_skip_pred_weigh_table(H264Bitstream &bs_parser,
                                          uint32_t num_ref_idx_lx_active_minus1)
{
    H264Bitstream::Result bits;

    for (uint32_t i = 0; i <= num_ref_idx_lx_active_minus1; i++) {
        bits = bs_parser.read_un_bits(1);
        RETURN_IF_ERROR(bits);
        bool luma_weight_lx_flag = bits.value ? true : false;

        if (luma_weight_lx_flag) {
            bits = bs_parser.read_sev_bits(); /* luma_weight_lx[i] */
            RETURN_IF_ERROR(bits);

            bits = bs_parser.read_sev_bits(); /* luma_offset_lx[i] */
            RETURN_IF_ERROR(bits);
        }

        bits = bs_parser.read_un_bits(1);
        RETURN_IF_ERROR(bits);
        bool chroma_weight_lx_flag = bits.value ? true : false;

        if (chroma_weight_lx_flag) {
            for (size_t j = 0; j < 2; j++) {
                bits = bs_parser.read_sev_bits(); /* chroma_weight_lx[i][j] */
                RETURN_IF_ERROR(bits);

                bits = bs_parser.read_sev_bits(); /* chroma_offset_lx[i][j] */
                RETURN_IF_ERROR(bits);
            }
        }
    }

    return true;
}

bool at_h264_get_full_slice_header_info(const unsigned char *data, size_t size,
                                        const SpsNalInfo &sps, const PpsNalInfo &pps,
                                        SliceHeaderInfo &slice_header_info)
{
    H264Bitstream bs_parser(data, size);
    if (!at_h264_get_initial_slice_header_info_implementation(bs_parser, slice_header_info)) {
        return false;
    }

    H264Bitstream::Result bits;

    /* at_h264_get_initial_slice_header_info reads up to (including)
     pic_parameter_set_id so the next field in stream is frame_num */
    bits = bs_parser.read_un_bits(sps.log2_max_frame_num_minus4 + 4); /* frame_num */
    RETURN_IF_ERROR(bits);
    slice_header_info.frame_num = bits.value;

    /* If SPS has frame_mbs_only_flag set */
    if (!sps.frame_mbs_only_flag) {
        bits = bs_parser.read_un_bits(1); /* Then read field_pic_flag */
        RETURN_IF_ERROR(bits);
        slice_header_info.field_pic_flag = bits.value ? true : false;
        if (slice_header_info.field_pic_flag) {
            bits = bs_parser.read_un_bits(1); /* Gota read bottom_field_flag as well */
            RETURN_IF_ERROR(bits);
            slice_header_info.bottom_field_flag = bits.value ? true : false;
        }
    }

    /* If in IDR unit type */
    if (NalType::IDR_SLICE == slice_header_info.nal_unit_type) {
        bits = bs_parser.read_uev_bits(); /* idr_pic_id */
        RETURN_IF_ERROR(bits);
        slice_header_info.idr_pic_id = bits.value;
    }

    if (0 == sps.pic_order_cnt_type) {
        /* pic_order_cnt_lsb */
        bits = bs_parser.read_un_bits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
        RETURN_IF_ERROR(bits);
        slice_header_info.pic_order_cnt_lsb = bits.value;

        if (pps.pic_order_present_flag && !slice_header_info.field_pic_flag) {
            bits = bs_parser.read_sev_bits(); /* delta_pic_order_cnt_bottom */
            RETURN_IF_ERROR(bits);
            slice_header_info.delta_pic_order_cnt_bottom = bits.value;
        }
    }

    if ((1 == sps.pic_order_cnt_type)
        && !sps.delta_pic_order_always_zero_flag) {
        bits = bs_parser.read_sev_bits(); /* delta_pic_order_cnt[0] */
        RETURN_IF_ERROR(bits);
        slice_header_info.delta_pic_order_cnt[0] = bits.value;

        if (pps.pic_order_present_flag && !slice_header_info.field_pic_flag) {
            bits = bs_parser.read_sev_bits(); /* delta_pic_order_cnt[1] */
            RETURN_IF_ERROR(bits);
            slice_header_info.delta_pic_order_cnt[1] = bits.value;
        }
    }

    if (pps.redundant_pic_cnt_present_flag) {
        bits = bs_parser.read_uev_bits();
        RETURN_IF_ERROR(bits);
        slice_header_info.redundant_pic_cnt = bits.value;
    }

    if (H264SliceType::B == slice_header_info.h264_slice_type) {
        bits = bs_parser.read_un_bits(1); /* direct_spatial_mv_pred_flag */
        RETURN_IF_ERROR(bits);
    }

    /* "num_ref_idx_active_override_flag equal to 0 specifies that the values
     of the syntax elements num_ref_idx_l0_active_minus1 and
     num_ref_idx_l1_active_minus1 specified in the referred picture parameter
     set are in effect. num_ref_idx_active_override_flag equal to 1 specifies
     that the num_ref_idx_l0_active_minus1 and num_ref_idx_l1_active_minus1
     specified in the referred picture parameter set are overridden for the
     current slice (and only for the current slice) by the following values in
     the slice header" */

    /* So copy defaults first... */
    slice_header_info.num_ref_idx_l0_active_minus1
        = pps.num_ref_idx_l0_active_minus1;
    slice_header_info.num_ref_idx_l1_active_minus1
        = pps.num_ref_idx_l1_active_minus1;

    if ((H264SliceType::P == slice_header_info.h264_slice_type)
        || (H264SliceType::SP == slice_header_info.h264_slice_type)
        || (H264SliceType::B == slice_header_info.h264_slice_type)) {
        bits = bs_parser.read_un_bits(1);
        RETURN_IF_ERROR(bits);
        bool num_ref_idx_active_override_flag = bits.value ? true : false;

        /* ...and then check if we should override them */
        if (num_ref_idx_active_override_flag) {
            bits = bs_parser.read_uev_bits(); /* num_ref_idx_l0_active_minus1 */
            RETURN_IF_ERROR(bits);
            slice_header_info.num_ref_idx_l0_active_minus1 = bits.value;

            if (H264SliceType::B == slice_header_info.h264_slice_type) {
                bits = bs_parser.read_uev_bits(); /* num_fer_idx_l1_active_minus1 */
                RETURN_IF_ERROR(bits);
                slice_header_info.num_ref_idx_l1_active_minus1 = bits.value;
            }
        }
    }

    /* Here goes whole ref_pic_list_reordering() syntax element. We read it just
     to get further, no interest in this data yet. Note that I and SI pictures
     have zero elements, B pictures have two and all others have one, so I
     presume these are for forward/backward prediction */
    if ((H264SliceType::I != slice_header_info.h264_slice_type)
        && (H264SliceType::SI != slice_header_info.h264_slice_type)) {
        if (!at_h264_skip_ref_pic_list_reordering(bs_parser)) {
            return false;
        }
    }

    if (H264SliceType::B == slice_header_info.h264_slice_type) {
        if (!at_h264_skip_ref_pic_list_reordering(bs_parser)) {
            return false;
        }
    }

    /* Here goes pred_weight_table() syntax element. Just like above, we want to
     skip it */
    if ((pps.weighted_pred_flag
         && ((H264SliceType::P == slice_header_info.h264_slice_type)
             || H264SliceType::SP == slice_header_info.h264_slice_type))
        || ((1 == pps.weighted_bipred_idc)
            && (H264SliceType::B == slice_header_info.h264_slice_type))) {
        bits = bs_parser.read_uev_bits(); /* luma_log2_weight_denom */
        RETURN_IF_ERROR(bits);

        bits = bs_parser.read_uev_bits(); /* chroma_log2_weight_denom */
        RETURN_IF_ERROR(bits);

        if (!at_h264_skip_pred_weigh_table(bs_parser,
                                           slice_header_info.num_ref_idx_l0_active_minus1)) {
            return false;
        }

        if (H264SliceType::B == slice_header_info.h264_slice_type) {
            if (!at_h264_skip_pred_weigh_table(bs_parser,
                                               slice_header_info.num_ref_idx_l1_active_minus1)) {
                return false;
            }
        }
    }

    if (slice_header_info.ref_nal_idc) {
        /* This is dec_ref_pic_marking() syntax element, last one we want to
         retrieve now, to get memory_management_control_operation */
        if (NalType::IDR_SLICE == slice_header_info.nal_unit_type) {
            /* Call it a day, IDR slices don't contain memory management */
        } else {
            bits = bs_parser.read_un_bits(1);
            RETURN_IF_ERROR(bits);
            bool adaptive_ref_pic_marking_mode_flag = bits.value ? true : false;
            uint32_t memory_management_control_operation = 0;
            slice_header_info.had_memory_management_control_operation_equal_to_5 = false;
            if (adaptive_ref_pic_marking_mode_flag) {
                do {
                    bits = bs_parser.read_uev_bits();
                    RETURN_IF_ERROR(bits);
                    memory_management_control_operation = bits.value;

                    if ((1 == memory_management_control_operation)
                        || (3 == memory_management_control_operation)) {
                        bits = bs_parser.read_uev_bits(); /* difference_of_pic_nums_minus_1 */
                        RETURN_IF_ERROR(bits);
                    }

                    if (2 == memory_management_control_operation) {
                        bits = bs_parser.read_uev_bits(); /* long_term_pic_num */
                        RETURN_IF_ERROR(bits);
                    }

                    if ((3 == memory_management_control_operation)
                        || (6 == memory_management_control_operation)) {
                        bits = bs_parser.read_uev_bits(); /* long_term_frame_idx */
                        RETURN_IF_ERROR(bits);
                    }

                    if (4 == memory_management_control_operation) {
                        bits = bs_parser.read_uev_bits(); /* max_long_term_frame_idx_plus1 */
                    }

                    /* Some extra 200 or so lines of code are in essence for
                     this! It is important condition and is used in many places,
                     so OFC putting it close to begin of h264 slice header would
                     make our life too easy :/ */
                    if (5 == memory_management_control_operation) {
                        slice_header_info.had_memory_management_control_operation_equal_to_5 = true;
                    }
                } while (memory_management_control_operation);
            }
        }
    }

    return true;
}

const unsigned char *at_h264_next_start_code(const unsigned char *ptr, const unsigned char *limit)
{
    /* Make sure we have "safe" initial contents of start code state */
    uint32_t state = 0xdeadbeef;
    while (ptr != limit) {
        /* Shift start code one byte left, making place for next one
         (and getting rid of oldest) */
        state <<= 8;
        /* Load next byte */
        state |= *ptr++;
        /* H264 start codes have form:
         0x00, 0x00, 0x01, code */
        if (0x00000100 == (state & ~0xff)) {
            /* Found. It is safe to do -4, because to load start code
             ptr had to be incremented at least four times */
            return ptr - 4;
        }
    }
    /* Not found */
    return nullptr;
}

const char *at_h264_slice_type_description(int type)
{
    switch (type) {
        case 0:
            return "P";
        case 1:
            return "B";
        case 2:
            return "I";
        case 3:
            return "SP";
        case 4:
            return "SI";
        /* slice types really wrap around like that, and values in second
         range mean "all future slices in this coded picture will be of
         same type or type 5" */
        case 5:
            return "P(r)";
        case 6:
            return "B(r)";
        case 7:
            return "I(r)";
        case 8:
            return "SP(r)";
        case 9:
            return "SI(r)";
        default:
            return "unknown type";
    }
}
