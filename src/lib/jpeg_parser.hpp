/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

/* ISO/IEC 10918-1 : 1993(E) Table B.1 - Marker code assignments */
enum class MarkerType {
    PROHIBITED0 = 0x0,
    /* Reserved markers */
    TEM = 0x01, /* For temporary private use in arithmetic coding */
    RES0 = 0x02, /* First reserved marker */
    RES189 = 0xbf, /* Last reserved marker */

    /* Start of frame, non-differential, Huffman coding */
    SOF0 = 0xc0, /* Baseline DCT */
    SOF1 = 0xc1, /* Extended sequential DCT */
    SOF2 = 0xc2, /* Progressive DCT */
    SOF3 = 0xc3, /* Lossless (sequential) */

    /* Huffman table specification */
    DHT = 0xc4, /* Define Huffman table */

    /* Start of frame, differential, Huffman coding */
    SOF5 = 0xc5, /* Differential sequential DCT */
    SOF6 = 0xc6, /* Differential progressive DCT */
    SOF7 = 0xc7, /* Differential lossless (sequential) */

    /* Start of frame, non-differential, arithmetic coding */
    JPG = 0xc8, /* Reserved for JPEG extensions */
    SOF9 = 0xc9, /* Extended sequential DCT */
    SOF10 = 0xca, /* Progressive DCT */
    SOF11 = 0xcb, /* Lossless (sequential) */

    /* Arithmetic coding conditioning specification */
    DAC = 0xcc, /* Define arithmetic coding conditioning */

    /* Start of frame, differential, arithmetic coding */
    SOF13 = 0xcd, /* Differential sequential DCT */
    SOF14 = 0xce, /* Differential progressive DCT */
    SOF15 = 0xcf, /* Differential lossless */

    /* Restart interval termination */
    RST0 = 0xd0, /* First restart */
    RST7 = 0xd7, /* Last restart */

    /* Other markers */
    SOI = 0xd8, /* Start of image */
    EOI = 0xd9, /* End of image */
    SOS = 0xda, /* Start of scan */
    DQT = 0xdb, /* Define quantization tables */
    DNL = 0xdc, /* Define number of lines */
    DRI = 0xdd, /* Define restart interval */
    DHP = 0xde, /* Define hierarchical progression */
    EXP = 0xdf, /* Expand reference components */
    APP0 = 0xe0, /* First application segment */
    APP15 = 0xef, /* Last application segment */
    JPG0 = 0xf0, /* First JPEG extension */
    JPG13 = 0xfd, /* Last JPEG extension */
    COM = 0xfe, /* Comment */
    PROHIBITED255 = 0xff
};

const unsigned char *at_jpeg_next_marker(const unsigned char *ptr, const unsigned char *limit);
