#
/*
 *    Copyright (C) 2013
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the SDR-J (JSDR).
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * 	superframer for the SDR-J DAB+ receiver
 * 	This processor handles the whole DAB+ specific part
 ************************************************************************
 *	may 15 2015. A real improvement on the code
 *	is the addition from Stefan Poeschel to create a
 *	header for the aac that matches, really a big help!!!!
 ************************************************************************
 */
#include	"mp4processor.h"
#include	<cstring>
#include	"gui.h"
//
#include	"charsets.h"
#include	"faad-decoder.h"
#include	"pad-handler.h"

//	simple crc checker
//
bool	dabPlus_crc (uint8_t *msg, int16_t len) {
int i, j;
uint16_t	accumulator	= 0xFFFF;
uint16_t	crc;
uint16_t	genpoly		= 0x1021;

	for (i = 0; i < len; i ++) {
	   int16_t data = msg [i] << 8;
	   for (j = 8; j > 0; j--) {
	      if ((data ^ accumulator) & 0x8000)
	         accumulator = ((accumulator << 1) ^ genpoly) & 0xFFFF;
	      else
	         accumulator = (accumulator << 1) & 0xFFFF;
	      data = (data << 1) & 0xFFFF;
	   }
	}
//
//	ok, now check with the crc that is contained
//	in the au
	crc	= ~((msg [len] << 8) | msg [len + 1]) & 0xFFFF;
	return (crc ^ accumulator) == 0;
}
//
//
//	Now for real
/**
  *	\class mp4Processor is the main handler for the aac frames
  *	the class proper processes input and extracts the aac frames
  *	that are processed by the "faadDecoder" class
  */

	mp4Processor::mp4Processor (RadioInterface *mr,
	                            audioSink *as,
	                            int16_t bitRate):my_padHandler (mr) {
int16_t	i;

	myRadioInterface	= mr;
	connect (this, SIGNAL (show_successRate (int)),
	         mr, SLOT (show_successRate (int)));
	ourSink			= as;
	this	-> bitRate	= bitRate;	// input rate

	superFramesize		= 110 * (bitRate / 8);
	RSDims			= bitRate / 8;
	frameBytes		= new uint8_t [RSDims * 120];	// input
	outVector		= new uint8_t [RSDims * 110];
	blockFillIndex	= 0;
	blocksInBuffer	= 0;
//
	aacDecoder		= new faadDecoder (ourSink);
	frameCount	= 0;
	frameErrors	= 0;
//
//	error display
	au_count	= 0;
	au_errors	= 0;
}

	mp4Processor::~mp4Processor (void) {
	delete		aacDecoder;
	delete[]	frameBytes;
	delete[]	outVector;
}
//
//	we add vector for vector to the superframe. Once we have
//	5 lengths of "old" frames, we check
void	mp4Processor::addtoFrame (uint8_t *V, int16_t nbits) {
int16_t	i, j;
uint8_t	temp	= 0;
//
//	Note that the packing in the entry vector is still one bit
//	per Byte, nbits is the number of Bits (i.e. containing bytes)
	for (i = 0; i < nbits / 8; i ++) {	// in bytes
	   temp = 0;
	   for (j = 0; j < 8; j ++)
	      temp = (temp << 1) | (V [i * 8 + j] & 01);
	   frameBytes [blockFillIndex * nbits / 8 + i] = temp;
	}
//
	blocksInBuffer ++;
	blockFillIndex = (blockFillIndex + 1) % 5;
//
//	we take the last five blocks to look at
	if (blocksInBuffer >= 5) {
	   if (++frameCount >= 25) {
	      frameCount = 0;
	      show_successRate (4 * (25 - frameErrors));
	      frameErrors = 0;
	   }

//	OK, we give it a try, check the fire code
	   if (fc. check (&frameBytes [blockFillIndex * nbits / 8]) &&
	       (processSuperframe (frameBytes,
	                           blockFillIndex * nbits / 8))) {
//	since we processed a full cycle of 5 blocks, we just start a
//	new sequence, beginning with block blockFillIndex
	      blocksInBuffer	= 0;
	   }
	   else {	// virtual shift to left in block sizes
	      blocksInBuffer  = 4;
	      frameErrors ++;
	   }
	}
}
//
bool	mp4Processor::processSuperframe (uint8_t frameBytes [],
	                                 int16_t base) {
uint8_t		num_aus;
int16_t		i, j, k;
int16_t		nErrors	= 0;
uint8_t		rsIn	[120];
uint8_t		rsOut	[110];
uint8_t		dacRate;
uint8_t		sbrFlag;
uint8_t		aacChannelMode;
uint8_t		psFlag;
uint16_t	mpegSurround;
int32_t		outSamples	= 0;
int32_t		tmp		= 0;

/**	apply reed-solomon error repar
  *	OK, what we now have is a vector with RSDims * 120 uint8_t's
  *	Output is a vector with RSDims * 110 uint8_t's
  */
	for (j = 0; j < RSDims; j ++) {
	   int16_t ler	= 0;
	   for (k = 0; k < 120; k ++) 
	      rsIn [k] = frameBytes [(base + j + k * RSDims) % (RSDims * 120)];
	   ler = rsDecoder. dec (rsIn, rsOut, 135);
	   if (ler > 0) {
//	      fprintf (stderr, "corrected %d errors\n", ler);
	      nErrors += ler;
	   }
	   else
	   if (ler < 0) {
//	      fprintf (stderr, "unrecoverable errors\n");
	      return false;
	   }
	   for (k = 0; k < 110; k ++) 
	      outVector [j + k * RSDims] = rsOut [k];
	}
//
//	OK, the result is N * 110 * 8 bits 
//	bits 0 .. 15 is firecode
//	bit 16 is unused
	dacRate		= (outVector [2] >> 6) & 01;	// bit 17
	sbrFlag		= (outVector [2] >> 5) & 01;	// bit 18
	aacChannelMode	= (outVector [2] >> 4) & 01;	// bit 19
	psFlag		= (outVector [2] >> 3) & 01;	// bit 20
	mpegSurround	= (outVector [2] & 07);		// bits 21 .. 23

	switch (2 * dacRate + sbrFlag) {
	   default:		// cannot happen
	   case 0:
	      num_aus = 4;
	      au_start [0] = 8;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [5];
	      au_start [3] = outVector [6] * 16 + (outVector [7] >> 4);
	      au_start [4] = 110 *  (bitRate / 8);
	      break;
//
	   case 1:
	      num_aus = 2;
	      au_start [0] = 5;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = 110 * (bitRate / 8);
	      break;
//
	   case 2:
	      num_aus = 6;
	      au_start [0] = 11;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [5];
	      au_start [3] = outVector [6] * 16 + (outVector [7] >> 4);
	      au_start [4] = (outVector [7] & 0xf) * 256 +
	                     outVector [8];
	      au_start [5] = outVector [9] * 16 +
	                     (outVector [10] >> 4);
	      au_start [6] = 110 *  (bitRate / 8);
	      break;
//
	   case 3:
	      num_aus = 3;
	      au_start [0] = 6;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 +
	                     outVector [5];
	      au_start [3] = 110 * (bitRate / 8);
	      break;
	}
//
//	extract the AU'si and prepare a buffer, sufficiently
//	long for conversion to PCM samples
	for (i = 0; i < num_aus; i ++) {
	   int16_t	aac_frame_length;
	   au_count ++;
	   uint8_t theAU [2 * 960];	// sure, large enough
	   memset (theAU, 0, sizeof (theAU));
//
//	The crc takes the two last bytes from the au vector,
//	but first some - hopefully superfluous - sanity checks
	   if (au_start [i + 1] < au_start [i]) {
//	should not happen, all errors were corrected
//	      fprintf (stderr, "%d %d\n", au_start [i + 1], au_start [i]);
	      return false;
	   }
	   aac_frame_length = au_start [i + 1] - au_start [i] - 2;
	   if ((aac_frame_length >= 960) || (aac_frame_length < 0)) {
//	should not happen, all errors were corrected
//	      fprintf (stderr, "serious error in frame, framelength = %d\n",
//	                            aac_frame_length);
	      return false;
	   }
//	but first the crc check
	   if (dabPlus_crc (&outVector [au_start [i]],
	                    aac_frame_length)) {
//
//	create a real aac vector, starting with the newly created
//	header
	      memcpy (theAU,
	              &outVector [au_start [i]],
	              aac_frame_length * sizeof (uint8_t));

	      if (((theAU [0] >> 5) & 07) == 4)
	         my_padHandler. processPAD (theAU);
//
//	just a few bytes extra, such that the decoder can look
//	beyond the last byte
	      for (j = aac_frame_length;
	           j < aac_frame_length + 10; j ++)
	         theAU [j] = 0;
	      tmp = aacDecoder -> MP42PCM (dacRate,
	                                   sbrFlag,
	                                   mpegSurround,
	                                   aacChannelMode,
	                                   theAU,
	                                   aac_frame_length);
	      if (tmp == 0)
	         frameErrors ++;
	      else
	         outSamples += tmp;
	   }
	   else {
	      au_errors ++;
//	      fprintf (stderr, "CRC failure with dab+ frame\n");
	   }
	}
//	fprintf (stderr, "%d samples good for %d nsec of music\n",
//	                 outSamples, outSamples * 1000 / 48);
//
	return true;
}

