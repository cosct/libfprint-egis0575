/*
 * Egis Technology Inc. (aka. LighTuning) 0575 driver for libfprint
 * Press-snapshot architecture ported from the EH577 driver
 * (championswimmer/libfprint-eh577, commit b19955e, LGPL-2.1+):
 * Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>
 * Copyright (C) 2026 Arnav Gupta <dev@championswimmer.in>
 * EH575 adaptation for the fprintdriver research project
 * (https://github.com/cosct/fprintdriver):
 * Copyright (C) 2026 cosct <cosct@outlook.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

/*
 * Device data
 */

#define EGIS0575_CONFIGURATION 1
#define EGIS0575_INTERFACE 0

/*
 * Device endpoints
 */

#define EGIS0575_EPOUT 0x01 /* ( 1 | FPI_USB_ENDPOINT_OUT ) */
#define EGIS0575_EPIN 0x82  /* ( 2 | FPI_USB_ENDPOINT_IN ) */

/*
 * Image polling sequences
 *
 * First 4 bytes of packet to be sent is "EGIS", rest are unknown but a specific pattern was observed.
 * First 4 bytes of response is "SIGE"
 *
 * These tables are the EH575 sequences from the Animeshz reverse-engineering
 * work; EH577 hardware accepts the same family (see https://github.com/cosct/fprintdriver/blob/master/docs/protocol.md).
 */

/* *INDENT-OFF* */
typedef struct Packet
{
  int            length;
  unsigned char *sequence;

  int            response_length;
} Packet;

#define EGIS0575_PRE_INIT_PACKETS_LENGTH 29
static const Packet EGIS0575_PRE_INIT_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfd}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x35, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x80, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x80, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0f, 0x03}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x09, 0x83}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xf4}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x44}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x50, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x50, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x73, 0x14, 0xec}, .response_length = 7},  /* returns 7 bytes in pre-init context, not 5356 */
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xec}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12}, .response_length = 18},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x33}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0b, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},   /* to EGIS0575_POST_INIT_PACKETS */
};

#define EGIS0575_POST_INIT_PACKETS_LENGTH 18
static const Packet EGIS0575_POST_INIT_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0xfc}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0xfc}, .response_length = 7},   /* EH575: SIGE 01 01 01 here means "pre-init required" */
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xfc}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12}, .response_length = 18},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x33}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x66}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x57}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x2d, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03}, .response_length = 10},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x0f, 0x03}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x64, 0x14, 0xec}, .response_length = 5356},   /* snapshot frame read */
};

/*
 * Calibration sequence (topni1 flow, verified on real EH575 hardware).
 *
 * Without uploading the 5356-byte calibration block the sensor returns
 * all-zero frames (confirmed on 2026-09-12 probe run: PRE_INIT/POST_INIT
 * complete cleanly but every 64 14 ec frame is zero).  The 73 14 ec command
 * enters calibration-upload mode and MUST be followed immediately by the
 * 5356-byte payload — sending it bare (as the Animeshz PRE_INIT table does)
 * wedges the transport on the second attempt.
 *
 * Flow: PHASE_1 -> poll 60 2d until resp[5]==0x05 -> PHASE_3 ->
 *       poll 60 35 until resp[5]==0x00 -> PHASE_5 -> 72 14 ec (read cal) ->
 *       PRE_RESET -> poll 60 00 until resp[5]!=0x00 -> POST_RESET ->
 *       73 14 ec + write 5356 bytes + ack -> POST_CALIBRATION polling.
 */

#define EGIS0575_CAL_PHASE_1_PACKETS_LENGTH 16
static const Packet EGIS0575_CAL_PHASE_1_PACKETS[] = {
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfd}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x35, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x80, 0x00}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x80}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0f, 0x03}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x09, 0x83}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x00, 0x00, 0x52}, .response_length = 18},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x15}, .response_length = 9},
};

/* poll until resp[5] == 0x05 */
static const Packet EGIS0575_CAL_POLL_2 = {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x2d}, .response_length = 7};

#define EGIS0575_CAL_PHASE_3_PACKETS_LENGTH 2
static const Packet EGIS0575_CAL_PHASE_3_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03}, .response_length = 10},
  {.length = 10, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x33, 0x03, 0x73, 0x10, 0x01}, .response_length = 10},
};

/* poll until resp[5] == 0x00 */
static const Packet EGIS0575_CAL_POLL_4 = {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x35}, .response_length = 7};

#define EGIS0575_CAL_PHASE_5_PACKETS_LENGTH 4
static const Packet EGIS0575_CAL_PHASE_5_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xf4}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x44}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x50, 0x01}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x50, 0x01}, .response_length = 7},
};

/* then send 72 14 ec and read the 5356-byte calibration block */
static const Packet EGIS0575_CAL_READ_REQ_PACKET = {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x72, 0x14, 0xec}, .response_length = 5356};

#define EGIS0575_PRE_RESET_PACKETS_LENGTH 3
static const Packet EGIS0575_PRE_RESET_PACKETS[] = {
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x97, 0x00, 0x00}, .response_length = 7},  /* sensor reset */
};

/* post-reset status poll: keep polling until resp[5] != 0x00 (sensor is
 * busy with the 97 reset while resp[5] stays 0x00) */
static const Packet EGIS0575_RESET_POLL_PACKET = {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00}, .response_length = 7};

#define EGIS0575_POST_RESET_PACKETS_LENGTH 13
static const Packet EGIS0575_POST_RESET_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfd}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x35, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x80, 0x00}, .response_length = 7},
  {.length = 6, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x80}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0f, 0x03}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x09, 0x83}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xf4}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x44}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x50, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x50, 0x03}, .response_length = 7},
};

/* enter calibration-upload mode; MUST be followed by the 5356-byte payload */
static const Packet EGIS0575_CAL_ENTER_PACKET = {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x73, 0x14, 0xec}, .response_length = 7};

/*
 * Capture-burst shutdown (topni1 POST_REPEAT): sent after each capture
 * burst.  Contains AGC/exposure controls (the 71-family packet); without it
 * the sensor stays in continuous-capture mode and degrades into a
 * no-response state after a few minutes of use (observed twice on real
 * hardware: idle frames flatline to coverage <=1% even for real presses).
 */
#define EGIS0575_SHUTDOWN_PACKETS_LENGTH 9
static const Packet EGIS0575_SHUTDOWN_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x2d, 0x20}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0x20}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0xf4, 0x03, 0x44, 0x03, 0x0b, 0x14, 0x20, 0x01, 0x0a, 0x72}, .response_length = 18},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0c, 0x03}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0b, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x71, 0x45, 0x06, 0x00, 0xb9, 0x87, 0x13, 0x00, 0x03}, .response_length = 13},
};

#define EGIS0575_POST_CALIBRATION_PACKETS_LENGTH 20
static const Packet EGIS0575_POST_CALIBRATION_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xec}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0b, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xfc}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12}, .response_length = 18},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x33}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x66}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x57}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x2d, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03}, .response_length = 10},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x0f, 0x03}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x64, 0x14, 0xec}, .response_length = 5356},   /* snapshot frame read (warm-up frame on first pass) */
};

/*
 * The USB frame is 103x52, row-major with a 103-byte stride.
 *
 * On EH577 the rightmost 33 columns (src_x 70..102) are returned as hard zeros
 * by the firmware on every frame, so the responsive area is 70x52 there.
 * On EH575 a 600-frame column-activity experiment found all 103 columns
 * responsive (no dead zone); full width is the settled default.  The
 * EGIS0575_ACTIVE_WIDTH environment variable is kept for re-checking
 * other units (see the fprintdriver research repo, docs/comparison.md).
 */
#define EGIS0575_SENSOR_STRIDE_X 103          /* raw row stride (active + possible zero pad) */
#define EGIS0575_SENSOR_STRIDE_Y 52           /* raw rows */
#define EGIS0575_SENSOR_ACTIVE_WIDTH_DEFAULT 103
#define EGIS0575_IMGSIZE (EGIS0575_SENSOR_STRIDE_X * EGIS0575_SENSOR_STRIDE_Y)

#define EGIS0575_RESIZE 2

/*
 * Stage-2 processed-image quality gate (runs on the final resized snapshot
 * geometry).
 *
 * Acceptance criteria (applied after median denoise + stretch5 enhancement):
 * - grain < 6.000%          (GRAIN_PCT_X1000 = 6000)
 * - minutiae >= 1           (MIN_MINUTIAE = 0; code uses strict >)
 * - minutiae < 10           (strict less-than; too many = noise/grain artefacts)
 * - ridge pixels > 4000     (strict greater-than)
 *
 * stretch5 enhancement (p5..p99 → 20..245) is applied before NBIS extraction
 * but is NOT itself a rejection gate.
 */
#define EGIS0575_ENHANCE_STRETCH_LO_PCT 5
#define EGIS0575_ENHANCE_STRETCH_HI_PCT 99
#define EGIS0575_ENHANCE_STRETCH_OUT_LO 20
#define EGIS0575_ENHANCE_STRETCH_OUT_HI 245
#define EGIS0575_STAGE2_GRAIN_DIFF_THRESHOLD 25
#define EGIS0575_STAGE2_GRAIN_PCT_X1000 6000
#define EGIS0575_STAGE2_MIN_MINUTIAE 0
#define EGIS0575_STAGE2_MAX_MINUTIAE 10
#define EGIS0575_STAGE2_RIDGE_PIXEL_THRESHOLD 180
#define EGIS0575_STAGE2_MIN_RIDGE_PIXELS 4000

/*
 * Presence gate: coverage is the primary background-subtracted signal.
 * EH575 semantics differ from EH577: an empty frame is full-frame
 * low-intensity content (~5356 raw finger-like pixels), so a real press
 * REDUCES the raw finger-like count; raw >= 5300 therefore reads as
 * "no finger" and the count also feeds the weak-press degradation
 * heuristic in the sensor-health watchdog.  (EH577 for comparison:
 * no-finger <=173, finger-present >=1047.)
 */
#define EGIS0575_PRESENCE_MIN_COVERAGE_PCT 18
#define EGIS0575_PRESENCE_MIN_INTENSITY 10
#define EGIS0575_PRESENCE_MIN_RAW_FINGER_PIXELS 800

/* Weak-press degradation heuristic (sensor-health watchdog): a real press
 * pulls the raw finger-like count below this from the ~5356 idle baseline. */
#define EGIS0575_WEAK_PRESS_MAX_RAW_PIXELS 5300

/* A broken calibration read leaves a trailing run of this many identical
 * bytes (topni1 measured 0x3f); checked at SM_CAL_CHECK. */
#define EGIS0575_CAL_BROKEN_TAIL_RUN 100

/* Consecutive timeout-driven claim recycles within one action before the
 * sensor is declared wedged mid-action (a live sensor produces a frame
 * within a couple of recycles, which resets the counter). */
#define EGIS0575_TIMEOUT_RECOVERY_MAX 3

/* The cancel watchdog re-checks the capture loop at this interval. */
#define EGIS0575_CANCEL_WATCHDOG_MS 1000

/* Upper bound on how long a newly requested action waits for the previous
 * action's SM_DONE shutdown chain to drain before failing with BUSY. That
 * chain is 9 packets × 2 transfers at EGIS0575_TIMEOUT each (~36 s worst
 * case on a wedged sensor, see the deferred-close note below), so 40 s
 * covers it with margin; anything longer means the loop is stuck and the
 * action must fail instead of polling forever. */
#define EGIS0575_ACTION_START_WAIT_MAX_MS 40000

/* Deferred close: poll interval and retry cap while waiting for the capture
 * loop to wind down before the interface is released. The shutdown chain is
 * 9 packets × 2 transfers, each with a hard EGIS0575_TIMEOUT on a wedged
 * sensor (~36 s worst case); the cap must cover that — releasing the
 * interface with URBs in flight wedges the firmware (see dev_close). */
#define EGIS0575_CLOSE_POLL_INTERVAL_MS 50
#define EGIS0575_CLOSE_POLL_MAX_RETRIES 720

/* USB commands time out after this many ms. */
#define EGIS0575_TIMEOUT 2000

/*
 * Milliseconds to pause after submitting a good image before restarting
 * polling to observe the real finger-off transition.
 */
#define EGIS0575_POST_CAPTURE_POLL_DELAY_MS 20

/*
 * Bound on frame reads per interface claim before recycling, as a wedge
 * safeguard.  EH577 measured ~8; EH575 (topni1 driver) polls hundreds of
 * frames within one claim without issue.  A low bound combined with the
 * calibration re-upload on recycle hammers the sensor (measured 1865
 * recycles in 3.5 min of idle polling -> firmware hang), so keep it loose.
 */
#define EGIS0575_MAX_FRAMES_PER_CLAIM 200
