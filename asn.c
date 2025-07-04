/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   ASN.1 utility functions
   Copyright 2012 Henrik Andersson <hean01@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rdesktop.h"


/* Parse an ASN.1 BER header */
RD_BOOL
ber_parse_header(STREAM s, int tagval, uint32 *length)
{
	int tag, len;

	if (tagval > 0xff)
	{
		if (!s_check_rem(s, 2)) {
			return False;
		}
		in_uint16_be(s, tag);
	}
	else
	{
		if (!s_check_rem(s, 1)) {
			return False;
		}
		in_uint8(s, tag);
	}

	if (tag != tagval)
	{
		error("expected tag %d, got %d\n", tagval, tag);
		return False;
	}

	if (!s_check_rem(s, 1)) {
		return False;
	}
	in_uint8(s, len);

	if (len & 0x80)
	{
		len &= ~0x80;
		if (!s_check_rem(s, len)) {
			return False;
		}
		*length = 0;
		while (len--)
			next_be(s, *length);
	}
	else
		*length = len;

	return True;
}

/* Output an ASN.1 BER header */
void
ber_out_header(STREAM s, int tagval, int length)
{
	if (tagval > 0xff)
	{
		out_uint16_be(s, tagval);
	}
	else
	{
		out_uint8(s, tagval);
	}

	if (length >= 0x80)
	{
		out_uint8(s, 0x82);
		out_uint16_be(s, length);
	}
	else
		out_uint8(s, length);
}

/* Output an ASN.1 BER integer */
void
ber_out_integer(STREAM s, int value)
{
	ber_out_header(s, BER_TAG_INTEGER, 2);
	out_uint16_be(s, value);
}

RD_BOOL
ber_in_header(STREAM s, int *tagval, int *decoded_len)
{
	in_uint8(s, *tagval);
	in_uint8(s, *decoded_len);

	if (*decoded_len < 0x80)
		return True;
	else if (*decoded_len == 0x81)
	{
		in_uint8(s, *decoded_len);
		return True;
	}
	else if (*decoded_len == 0x82)
	{
		in_uint16_be(s, *decoded_len);
		return True;
	}

	return False;
}
