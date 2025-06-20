/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Protocol services - RDP encryption and licensing
   Copyright (C) Matthew Chapman <matthewc.unsw.edu.au> 1999-2008
   Copyright 2005-2011 Peter Astrand <astrand@cendio.se> for Cendio AB

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
#include "ssl.h"

extern char g_hostname[16];
extern int g_width;
extern int g_height;
extern unsigned int g_keylayout;
extern int g_keyboard_type;
extern int g_keyboard_subtype;
extern int g_keyboard_functionkeys;
extern RD_BOOL g_encryption;
extern RD_BOOL g_licence_issued;
extern RD_BOOL g_licence_error_result;
extern RDP_VERSION g_rdp_version;
extern RD_BOOL g_console_session;
extern uint32 g_redirect_session_id;
extern int g_server_depth;
extern VCHANNEL g_channels[];
extern unsigned int g_num_channels;
extern uint8 g_client_random[SEC_RANDOM_SIZE];

static int g_rc4_key_len;
static RDSSL_RC4 g_rc4_decrypt_key;
static RDSSL_RC4 g_rc4_encrypt_key;
static uint32 g_server_public_key_len;

static uint8 g_sec_sign_key[16];
static uint8 g_sec_decrypt_key[16];
static uint8 g_sec_encrypt_key[16];
static uint8 g_sec_decrypt_update_key[16];
static uint8 g_sec_encrypt_update_key[16];
static uint8 g_sec_crypted_random[SEC_MAX_MODULUS_SIZE];

uint16 g_server_rdp_version = 0;

/* These values must be available to reset state - Session Directory */
//总的加解密次数，各自4096次后更新密钥
static int g_sec_encrypt_use_count = 0;
static int g_sec_decrypt_use_count = 0;

/*
 * I believe this is based on SSLv3 with the following differences:
 *  MAC algorithm (5.2.3.1) uses only 32-bit length in place of seq_num/type/length fields
 *  MAC algorithm uses SHA1 and MD5 for the two hash functions instead of one or other
 *  key_block algorithm (6.2.2) uses 'X', 'YY', 'ZZZ' instead of 'A', 'BB', 'CCC'
 *  key_block partitioning is different (16 bytes each: MAC secret, decrypt key, encrypt key)
 *  encryption/decryption keys updated every 4096 packets
 * See http://wp.netscape.com/eng/ssl3/draft302.txt
 */

/*
 * 48-byte transformation used to generate master secret (6.1) and key material (6.2.2).
 * Both SHA1 and MD5 algorithms are used.
 */
void
sec_hash_48(uint8 * out, uint8 * in, uint8 * salt1, uint8 * salt2, uint8 salt)
{
	uint8 shasig[20];
	uint8 pad[4];
	RDSSL_SHA1 sha1;
	RDSSL_MD5 md5;
	int i;

	for (i = 0; i < 3; i++)
	{
		memset(pad, salt + i, i + 1);

		rdssl_sha1_init(&sha1);
		rdssl_sha1_update(&sha1, pad, i + 1);
		rdssl_sha1_update(&sha1, in, 48);
		rdssl_sha1_update(&sha1, salt1, 32);
		rdssl_sha1_update(&sha1, salt2, 32);
		rdssl_sha1_final(&sha1, shasig);

		rdssl_md5_init(&md5);
		rdssl_md5_update(&md5, in, 48);
		rdssl_md5_update(&md5, shasig, 20);
		rdssl_md5_final(&md5, &out[i * 16]);
	}
}

/*
 * 16-byte transformation used to generate export keys (6.2.2).
 */
void
sec_hash_16(uint8 * out, uint8 * in, uint8 * salt1, uint8 * salt2)
{
	RDSSL_MD5 md5;

	rdssl_md5_init(&md5);
	rdssl_md5_update(&md5, in, 16);
	rdssl_md5_update(&md5, salt1, 32);
	rdssl_md5_update(&md5, salt2, 32);
	rdssl_md5_final(&md5, out);
}

/*
 * 16-byte sha1 hash
 */
void
sec_hash_sha1_16(uint8 * out, uint8 * in, uint8 * salt1)
{
	RDSSL_SHA1 sha1;
	rdssl_sha1_init(&sha1);
	rdssl_sha1_update(&sha1, in, 16);
	rdssl_sha1_update(&sha1, salt1, 16);
	rdssl_sha1_final(&sha1, out);
}

/* create string from hash */
void
sec_hash_to_string(char *out, int out_size, uint8 * in, int in_size)
{
	int k;
	memset(out, 0, out_size);
	for (k = 0; k < in_size; k++, out += 2)
	{
		sprintf(out, "%.2x", in[k]);
	}
}

/* Reduce key entropy from 64 to 40 bits */
static void
sec_make_40bit(uint8 * key)
{
	key[0] = 0xd1;
	key[1] = 0x26;
	key[2] = 0x9e;
}

/* Generate encryption keys given client and server randoms */
static void
sec_generate_keys(uint8 * client_random, uint8 * server_random, int rc4_key_size)
{
	uint8 pre_master_secret[48];
	uint8 master_secret[48];
	uint8 key_block[48];

	/* Construct pre-master secret */
	//PreMasterSecret = First192Bits(ClientRandom) + First192Bits(ServerRandom)
	memcpy(pre_master_secret, client_random, 24);
	memcpy(pre_master_secret + 24, server_random, 24);

	/* Generate master secret and then key material */
	//MasterSecret = PreMasterHash(0x41) + PreMasterHash(0x4242) + PreMasterHash(0x434343)
	sec_hash_48(master_secret, pre_master_secret, client_random, server_random, 'A');
	//SessionKeyBlob = MasterHash(0x58) + MasterHash(0x5959) + MasterHash(0x5A5A5A)
	sec_hash_48(key_block, master_secret, client_random, server_random, 'X');

	/* First 16 bytes of key material is MAC secret */
	//MACKey128 = First128Bits(SessionKeyBlob)
	memcpy(g_sec_sign_key, key_block, 16);

	/* Generate export keys from next two blocks of 16 bytes */
	//InitialClientDecryptKey128 = FinalHash(Second128Bits(SessionKeyBlob))
	sec_hash_16(g_sec_decrypt_key, &key_block[16], client_random, server_random);
	//InitialClientEncryptKey128 = FinalHash(Third128Bits(SessionKeyBlob))
	sec_hash_16(g_sec_encrypt_key, &key_block[32], client_random, server_random);

	if (rc4_key_size == 1)
	{
		DEBUG(("40-bit encryption enabled\n"));
		//MACKey40 = 0xD1269E + Last40Bits(First64Bits(MACKey128))
		sec_make_40bit(g_sec_sign_key);
		//InitialClientEncryptKey40 = 0xD1269E + Last40Bits(First64Bits(InitialClientEncryptKey128))
		sec_make_40bit(g_sec_decrypt_key);
		//InitialClientDecryptKey40 = 0xD1269E + Last40Bits(First64Bits(InitialClientDecryptKey128))
		sec_make_40bit(g_sec_encrypt_key);
		g_rc4_key_len = 8;
	}
	else
	{
		DEBUG(("rc_4_key_size == %d, 128-bit encryption enabled\n", rc4_key_size));
		g_rc4_key_len = 16;
	}

	/* Save initial RC4 keys as update keys */
	memcpy(g_sec_decrypt_update_key, g_sec_decrypt_key, 16);
	memcpy(g_sec_encrypt_update_key, g_sec_encrypt_key, 16);

	/* Initialise RC4 state arrays */
	rdssl_rc4_set_key(&g_rc4_decrypt_key, g_sec_decrypt_key, g_rc4_key_len);
	rdssl_rc4_set_key(&g_rc4_encrypt_key, g_sec_encrypt_key, g_rc4_key_len);
}

static uint8 pad_54[40] = {
	54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
	54, 54, 54,
	54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
	54, 54, 54
};

static uint8 pad_92[48] = {
	92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92,
	92, 92, 92, 92, 92, 92, 92
};

/* Output a uint32 into a buffer (little-endian) */
void
buf_out_uint32(uint8 * buffer, uint32 value)
{
	buffer[0] = (value) & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[3] = (value >> 24) & 0xff;
}

/* Generate a MAC hash (5.2.3.1), using a combination of SHA1 and MD5 */
void
sec_sign(uint8 * signature, int siglen, uint8 * session_key, int keylen, uint8 * data, int datalen)
{
	uint8 shasig[20];
	uint8 md5sig[16];
	uint8 lenhdr[4];
	RDSSL_SHA1 sha1;
	RDSSL_MD5 md5;

	buf_out_uint32(lenhdr, datalen);

	//SHAComponent = SHA(MACKeyN + Pad1 + DataLength + Data)
	rdssl_sha1_init(&sha1);
	rdssl_sha1_update(&sha1, session_key, keylen);
	rdssl_sha1_update(&sha1, pad_54, 40);
	rdssl_sha1_update(&sha1, lenhdr, 4);
	rdssl_sha1_update(&sha1, data, datalen);
	rdssl_sha1_final(&sha1, shasig);

	//MACSignature = First64Bits(MD5(MACKeyN + Pad2 + SHAComponent))
	rdssl_md5_init(&md5);
	rdssl_md5_update(&md5, session_key, keylen);
	rdssl_md5_update(&md5, pad_92, 48);
	rdssl_md5_update(&md5, shasig, 20);
	rdssl_md5_final(&md5, md5sig);

	memcpy(signature, md5sig, siglen);
}

/* Update an encryption key */
static void
sec_update(uint8 * key, uint8 * update_key)
{
	uint8 shasig[20];
	RDSSL_SHA1 sha1;
	RDSSL_MD5 md5;
	RDSSL_RC4 update;

	//SHAComponent = SHA(InitialEncryptKey + Pad1 + CurrentEncryptKey)
	rdssl_sha1_init(&sha1);
	rdssl_sha1_update(&sha1, update_key, g_rc4_key_len);
	rdssl_sha1_update(&sha1, pad_54, 40);
	rdssl_sha1_update(&sha1, key, g_rc4_key_len);
	rdssl_sha1_final(&sha1, shasig);

	//TempKey128 = MD5(InitialEncryptKey + Pad2 + SHAComponent)
	rdssl_md5_init(&md5);
	rdssl_md5_update(&md5, update_key, g_rc4_key_len);
	rdssl_md5_update(&md5, pad_92, 48);
	rdssl_md5_update(&md5, shasig, 20);
	rdssl_md5_final(&md5, key);

	//S-TableEncrypt = InitRC4(TempKey128)
	rdssl_rc4_set_key(&update, key, g_rc4_key_len);
	//NewEncryptKey128 = RC4(TempKey128, S-TableEncrypt)
	rdssl_rc4_crypt(&update, key, key, g_rc4_key_len);

	if (g_rc4_key_len == 8)
		sec_make_40bit(key);
}

/* Encrypt data using RC4 */
static void
sec_encrypt(uint8 * data, int length)
{
	if (g_sec_encrypt_use_count == 4096)
	{
		sec_update(g_sec_encrypt_key, g_sec_encrypt_update_key);
		rdssl_rc4_set_key(&g_rc4_encrypt_key, g_sec_encrypt_key, g_rc4_key_len);
		g_sec_encrypt_use_count = 0;
	}

	rdssl_rc4_crypt(&g_rc4_encrypt_key, data, data, length);
	g_sec_encrypt_use_count++;
}

/* Decrypt data using RC4 */
void
sec_decrypt(uint8 * data, int length)
{
	if (length <= 0)
		return;

	if (g_sec_decrypt_use_count == 4096)
	{
		sec_update(g_sec_decrypt_key, g_sec_decrypt_update_key);
		rdssl_rc4_set_key(&g_rc4_decrypt_key, g_sec_decrypt_key, g_rc4_key_len);
		g_sec_decrypt_use_count = 0;
	}

	rdssl_rc4_crypt(&g_rc4_decrypt_key, data, data, length);
	g_sec_decrypt_use_count++;
}

/* Perform an RSA public key encryption operation */
static void
sec_rsa_encrypt(uint8 * out, uint8 * in, int len, uint32 modulus_size, uint8 * modulus,
		uint8 * exponent)
{
	rdssl_rsa_encrypt(out, in, len, modulus_size, modulus, exponent);
}

/* Initialise secure transport packet */
//申请一块内存，自动累加各层长度，返回s指向待加密的数据起始位置（签名字段后）
STREAM
sec_init(uint32 flags, int maxlen)
{
	int hdrlen;
	STREAM s;

	//根据加密标记rdp头预留8字节的签名部分，rc4算法不改变加密后的报文长度
	if (!g_licence_issued && !g_licence_error_result)
		hdrlen = (flags & SEC_ENCRYPT) ? 12 : 4;
	else
		hdrlen = (flags & SEC_ENCRYPT) ? 12 : 0;
	s = mcs_init(maxlen + hdrlen);
	//此时s指向t.125之后的rdp报文头位置
	s_push_layer(s, sec_hdr, hdrlen);
	//此时s指向rdp签名字段后的数据开始位置
	return s;
}

/* Transmit secure transport packet over specified channel */
void
sec_send_to_channel(STREAM s, uint32 flags, uint16 channel)
{
	int datalen;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_SEC);
#endif

	s_pop_layer(s, sec_hdr);
	if ((!g_licence_issued && !g_licence_error_result) || (flags & SEC_ENCRYPT))
		out_uint32_le(s, flags);

	if (flags & SEC_ENCRYPT)
	{
		unsigned char *data;
		flags &= ~SEC_ENCRYPT;
		datalen = s_remaining(s) - 8;
		inout_uint8p(s, data, datalen + 8);

#if WITH_DEBUG
		DEBUG(("Sending encrypted packet:\n"));
		hexdump(data + 8, datalen);
#endif
		//签名及加密数据
		sec_sign(data, 8, g_sec_sign_key, g_rc4_key_len, data + 8, datalen);
		sec_encrypt(data + 8, datalen);
	}

	mcs_send_to_channel(s, channel);

#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_SEC);
#endif
}

/* Transmit secure transport packet */

void
sec_send(STREAM s, uint32 flags)
{
	//根据flags标记决定是否加密rdp数据
	sec_send_to_channel(s, flags, MCS_GLOBAL_CHANNEL);
}


/* Transfer the client random to the server */
static void
sec_establish_key(void)
{
	uint32 length = g_server_public_key_len + SEC_PADDING_SIZE;
	uint32 flags = SEC_CLIENT_RANDOM;
	STREAM s;

	s = sec_init(flags, length + 4);

	out_uint32_le(s, length);
	out_uint8a(s, g_sec_crypted_random, g_server_public_key_len);
	out_uint8s(s, SEC_PADDING_SIZE);

	s_mark_end(s);
	sec_send(s, flags);
	s_free(s);
}

/* Output connect initial data blob */
static void
sec_out_mcs_data(STREAM s, uint32 selected_protocol)
{
	int hostlen = 2 * strlen(g_hostname);
	int length = 162 + 76 + 12 + 4;
	unsigned int i;

	if (g_num_channels > 0)
		length += g_num_channels * 12 + 8;

	if (hostlen > 30)
		hostlen = 30;

	/* Generic Conference Control (T.124) ConferenceCreateRequest */
	out_uint16_be(s, 5);
	out_uint16_be(s, 0x14);
	out_uint8(s, 0x7c);
	out_uint16_be(s, 1);

	out_uint16_be(s, (length | 0x8000));	/* remaining length */

	out_uint16_be(s, 8);	/* length? */
	out_uint16_be(s, 16);
	out_uint8(s, 0);
	out_uint16_le(s, 0xc001);
	out_uint8(s, 0);

	out_uint32_le(s, 0x61637544);	/* OEM ID: "Duca", as in Ducati. */
	out_uint16_be(s, ((length - 14) | 0x8000));	/* remaining length */

	/* Client information */
	out_uint16_le(s, SEC_TAG_CLI_INFO);
	out_uint16_le(s, 216);	/* length */
	out_uint16_le(s, (g_rdp_version >= RDP_V5) ? 4 : 1);	/* RDP version. 1 == RDP4, 4 >= RDP5 to RDP8 */
	out_uint16_le(s, 8);
	out_uint16_le(s, g_width);
	out_uint16_le(s, g_height);
	out_uint16_le(s, 0xca01);
	out_uint16_le(s, 0xaa03);
	out_uint32_le(s, g_keylayout);
	out_uint32_le(s, 2600);	/* Client build. We are now 2600 compatible :-) */

	/* Unicode name of client, padded to 32 bytes */
	rdp_out_unistr(s, g_hostname, hostlen);
	out_uint8s(s, 30 - hostlen);

	/* See
	   http://msdn.microsoft.com/library/default.asp?url=/library/en-us/wceddk40/html/cxtsksupportingremotedesktopprotocol.asp */
	out_uint32_le(s, g_keyboard_type);
	out_uint32_le(s, g_keyboard_subtype);
	out_uint32_le(s, g_keyboard_functionkeys);
	out_uint8s(s, 64);	/* reserved? 4 + 12 doublewords */
	out_uint16_le(s, 0xca01);	/* colour depth? */
	out_uint16_le(s, 1);

	out_uint32(s, 0);
	out_uint8(s, g_server_depth);
	out_uint16_le(s, 0x0700);
	out_uint8(s, 0);
	out_uint32_le(s, 1);
	out_uint8s(s, 64);
	out_uint32_le(s, selected_protocol);	/* End of client info */

	/* Write a Client Cluster Data (TS_UD_CS_CLUSTER) */
	uint32 cluster_flags = 0;
	out_uint16_le(s, SEC_TAG_CLI_CLUSTER);	/* header.type */
	out_uint16_le(s, 12);	/* length */

	cluster_flags |= SEC_CC_REDIRECTION_SUPPORTED;
	cluster_flags |= (SEC_CC_REDIRECT_VERSION_3 << 2);

	if (g_console_session || g_redirect_session_id != 0)
		cluster_flags |= SEC_CC_REDIRECT_SESSIONID_FIELD_VALID;

	out_uint32_le(s, cluster_flags);
	out_uint32(s, g_redirect_session_id);

	/* Client encryption settings */
	out_uint16_le(s, SEC_TAG_CLI_CRYPT);
	out_uint16_le(s, 12);	/* length */
	out_uint32_le(s, g_encryption ? 0x3 : 0);	/* encryption supported, 128-bit supported */
	out_uint32(s, 0);	/* Unknown */

	DEBUG_RDP5(("g_num_channels is %d\n", g_num_channels));
	if (g_num_channels > 0)
	{
		out_uint16_le(s, SEC_TAG_CLI_CHANNELS);
		out_uint16_le(s, g_num_channels * 12 + 8);	/* length */
		out_uint32_le(s, g_num_channels);	/* number of virtual channels */
		for (i = 0; i < g_num_channels; i++)
		{
			DEBUG_RDP5(("Requesting channel %s\n", g_channels[i].name));
			out_uint8a(s, g_channels[i].name, 8);
			out_uint32_be(s, g_channels[i].flags);
		}
	}

	s_mark_end(s);
}

/* Parse a public key structure */
static RD_BOOL
sec_parse_public_key(STREAM s, uint8 * modulus, uint8 * exponent)
{
	uint32 magic, modulus_len;

	if (!s_check_rem(s, 8)) {
		return False;
	}

	in_uint32_le(s, magic);
	if (magic != SEC_RSA_MAGIC)
	{
		error("RSA magic 0x%x\n", magic);
		return False;
	}

	in_uint32_le(s, modulus_len);
	modulus_len -= SEC_PADDING_SIZE;
	if ((modulus_len < SEC_MODULUS_SIZE) || (modulus_len > SEC_MAX_MODULUS_SIZE))
	{
		error("Bad server public key size (%u bits)\n", modulus_len * 8);
		return False;
	}

	if (!s_check_rem(s, 1 + SEC_EXPONENT_SIZE + modulus_len + SEC_PADDING_SIZE)) {
		return False;
	}

	in_uint8s(s, 8);	/* modulus_bits, unknown */
	in_uint8a(s, exponent, SEC_EXPONENT_SIZE);
	in_uint8a(s, modulus, modulus_len);
	in_uint8s(s, SEC_PADDING_SIZE);
	g_server_public_key_len = modulus_len;

	return True;
}

/* Parse a public signature structure */
static RD_BOOL
sec_parse_public_sig(STREAM s, uint32 len, uint8 * modulus, uint8 * exponent)
{
	uint8 signature[SEC_MAX_MODULUS_SIZE];
	uint32 sig_len;

	if (len != 72)
	{
		return True;
	}
	memset(signature, 0, sizeof(signature));
	sig_len = len - 8;
	in_uint8a(s, signature, sig_len);
	return rdssl_sig_ok(exponent, SEC_EXPONENT_SIZE, modulus, g_server_public_key_len,
			    signature, sig_len);
}

/* Parse a crypto information structure */
static RD_BOOL
sec_parse_crypt_info(STREAM s, uint32 * rc4_key_size,
		     uint8 ** server_random, uint8 * modulus, uint8 * exponent)
{
	uint32 crypt_level, random_len, rsa_info_len;
	uint32 cacert_len, cert_len, flags;
	RDSSL_CERT *cacert, *server_cert;
	RDSSL_RKEY *server_public_key;
	uint16 tag, length;
	size_t next_tag;

	//提取encryptionMethod
	in_uint32_le(s, *rc4_key_size);	/* 1 = 40-bit, 2 = 128-bit */
	in_uint32_le(s, crypt_level);	/* 1 = low, 2 = medium, 3 = high */
	if (crypt_level == 0)
	{
		/* no encryption */
		return False;
	}

	//提取serverRandomLen
	in_uint32_le(s, random_len);
	//提取serverCertLen
	in_uint32_le(s, rsa_info_len);

	if (random_len != SEC_RANDOM_SIZE)
	{
		error("random len %d, expected %d\n", random_len, SEC_RANDOM_SIZE);
		return False;
	}

	//提取serverRandom
	in_uint8p(s, *server_random, random_len);

	/* RSA info */
	if (!s_check_rem(s, rsa_info_len))
		return False;

	//提取证书类型
	in_uint32_le(s, flags);	/* 1 = RDP4-style, 0x80000002 = X.509 */
	if (flags & 1)
	{
		DEBUG_RDP5(("We're going for the RDP4-style encryption\n"));
		in_uint8s(s, 8);	/* unknown */

		while (!s_check_end(s))
		{
			in_uint16_le(s, tag);
			in_uint16_le(s, length);

			next_tag = s_tell(s) + length;

			switch (tag)
			{
				case SEC_TAG_PUBKEY:
					if (!sec_parse_public_key(s, modulus, exponent))
						return False;
					DEBUG_RDP5(("Got Public key, RDP4-style\n"));

					break;

				case SEC_TAG_KEYSIG:
					if (!sec_parse_public_sig(s, length, modulus, exponent))
						return False;
					break;

				default:
					unimpl("crypt tag 0x%x\n", tag);
			}

			s_seek(s, next_tag);
		}
	}
	else
	{
		uint32 certcount;
		unsigned char *certdata;

		DEBUG_RDP5(("We're going for the RDP5-style encryption\n"));
		//获取证书链中证书数量，证书的存储顺序是Root-CA → Intermediate-CA → CA → Server-Cert，最后两个为本服务器及授权服务器证书
		in_uint32_le(s, certcount);	/* Number of certificates */
		if (certcount < 2)
		{
			error("Server didn't send enough X509 certificates\n");
			return False;
		}
		//跳过前面的证书，只处理最后2个整数
		for (; certcount > 2; certcount--)
		{		/* ignore all the certificates between the root and the signing CA */
			uint32 ignorelen;
			RDSSL_CERT *ignorecert;
			unsigned char *ignoredata;

			DEBUG_RDP5(("Ignored certs left: %d\n", certcount));
			in_uint32_le(s, ignorelen);
			in_uint8p(s, ignoredata, ignorelen);
			ignorecert = rdssl_cert_read(ignoredata, ignorelen);
			if (ignorecert == NULL)
			{	/* XXX: error out? */
				DEBUG_RDP5(("got a bad cert: this will probably screw up the rest of the communication\n"));
			}

#ifdef WITH_DEBUG_RDP5
			DEBUG_RDP5(("cert #%d (ignored):\n", certcount));
			rdssl_cert_print_fp(stdout, ignorecert);
#endif
		}
		/* Do da funky X.509 stuffy

		   "How did I find out about this?  I looked up and saw a
		   bright light and when I came to I had a scar on my forehead
		   and knew about X.500"
		   - Peter Gutman in a early version of 
		   http://www.cs.auckland.ac.nz/~pgut001/pubs/x509guide.txt
		 */
		in_uint32_le(s, cacert_len);
		DEBUG_RDP5(("CA Certificate length is %d\n", cacert_len));
		in_uint8p(s, certdata, cacert_len);
		cacert = rdssl_cert_read(certdata, cacert_len);
		if (NULL == cacert)
		{
			error("Couldn't load CA Certificate from server\n");
			return False;
		}
		in_uint32_le(s, cert_len);
		DEBUG_RDP5(("Certificate length is %d\n", cert_len));
		in_uint8p(s, certdata, cert_len);
		server_cert = rdssl_cert_read(certdata, cert_len);
		if (NULL == server_cert)
		{
			rdssl_cert_free(cacert);
			error("Couldn't load Certificate from server\n");
			return False;
		}
		if (!rdssl_certs_ok(server_cert, cacert))
		{
			rdssl_cert_free(server_cert);
			rdssl_cert_free(cacert);
			error("Security error CA Certificate invalid\n");
			return False;
		}
		rdssl_cert_free(cacert);
		in_uint8s(s, 16);	/* Padding */
		server_public_key = rdssl_cert_to_rkey(server_cert, &g_server_public_key_len);
		if (NULL == server_public_key)
		{
			DEBUG_RDP5(("Didn't parse X509 correctly\n"));
			rdssl_cert_free(server_cert);
			return False;
		}
		rdssl_cert_free(server_cert);
		if ((g_server_public_key_len < SEC_MODULUS_SIZE) ||
		    (g_server_public_key_len > SEC_MAX_MODULUS_SIZE))
		{
			error("Bad server public key size (%u bits)\n",
			      g_server_public_key_len * 8);
			rdssl_rkey_free(server_public_key);
			return False;
		}
		if (rdssl_rkey_get_exp_mod(server_public_key, exponent, SEC_EXPONENT_SIZE,
					   modulus, SEC_MAX_MODULUS_SIZE) != 0)
		{
			error("Problem extracting RSA exponent, modulus");
			rdssl_rkey_free(server_public_key);
			return False;
		}
		rdssl_rkey_free(server_public_key);
		return True;	/* There's some garbage here we don't care about */
	}
	return s_check_end(s);
}

/* Process crypto information blob */
static void
sec_process_crypt_info(STREAM s)
{
	uint8 *server_random = NULL;
	uint8 modulus[SEC_MAX_MODULUS_SIZE];
	uint8 exponent[SEC_EXPONENT_SIZE];
	uint32 rc4_key_size; //存储encryptionMethod字段, 1 = 40-bit, 2 = 128-bit

	memset(modulus, 0, sizeof(modulus));
	memset(exponent, 0, sizeof(exponent));
	//从证书信息提取公钥加密参数
	if (!sec_parse_crypt_info(s, &rc4_key_size, &server_random, modulus, exponent))
	{
		DEBUG(("Failed to parse crypt info\n"));
		return;
	}
	DEBUG(("Generating client random\n"));
	//生成客户端随机数
	generate_random(g_client_random);
	//使用公钥信息加密客户端随机数，后续传递给服务端
	sec_rsa_encrypt(g_sec_crypted_random, g_client_random, SEC_RANDOM_SIZE,
			g_server_public_key_len, modulus, exponent);
	//根据双方随机数及加密类型生成加密密钥
	sec_generate_keys(g_client_random, server_random, rc4_key_size);
}


/* Process SRV_INFO, find RDP version supported by server */
static void
sec_process_srv_info(STREAM s)
{
	in_uint16_le(s, g_server_rdp_version);
	DEBUG_RDP5(("Server RDP version is %d\n", g_server_rdp_version));
	if (1 == g_server_rdp_version)
	{
		g_rdp_version = RDP_V4;
		g_server_depth = 8;
	}
}


/* Process connect response data blob */
void
sec_process_mcs_data(STREAM s)
{
	uint16 tag, length;
	size_t next_tag;
	uint8 len;

	in_uint8s(s, 21);	/* header (T.124 ConferenceCreateResponse) */
	in_uint8(s, len);
	if (len & 0x80)
		in_uint8(s, len);

	while (!s_check_end(s))
	{
		in_uint16_le(s, tag);
		in_uint16_le(s, length);

		if (length <= 4)
			return;

		next_tag = s_tell(s) + length - 4;

		switch (tag)
		{
			case SEC_TAG_SRV_INFO:
				sec_process_srv_info(s);
				break;

			case SEC_TAG_SRV_CRYPT:
				//s指向Server Security Data的encryptionMethod
				sec_process_crypt_info(s);
				break;

			case SEC_TAG_SRV_CHANNELS:
				/* FIXME: We should parse this information and
				   use it to map RDP5 channels to MCS 
				   channels */
				break;

			default:
				unimpl("response tag 0x%x\n", tag);
		}

		s_seek(s, next_tag);
	}
}

/* Receive secure transport packet */
STREAM
sec_recv(uint8 * rdpver)
{
	uint32 sec_flags;
	uint16 channel;
	STREAM s;
	struct stream packet;
	size_t data_offset;
	unsigned char *data;

	while ((s = mcs_recv(&channel, rdpver)) != NULL)
	{
		packet = *s;
		if (rdpver != NULL)
		{
			if (*rdpver != 3)
			{
				if (*rdpver & 0x80)
				{
					if (!s_check_rem(s, 8)) {
						rdp_protocol_error("consume fastpath signature from stream would overrun", &packet);
					}

					in_uint8s(s, 8);	/* signature */

					data_offset = s_tell(s);

					inout_uint8p(s, data, s_remaining(s));
					sec_decrypt(data, s_remaining(s));

					s_seek(s, data_offset);
				}
				return s;
			}
		}
		if (g_encryption || (!g_licence_issued && !g_licence_error_result))
		{
			data_offset = s_tell(s);

			in_uint32_le(s, sec_flags);

			if (g_encryption)
			{
				data_offset = s_tell(s);

				if (sec_flags & SEC_ENCRYPT)
				{
					if (!s_check_rem(s, 8)) {
						rdp_protocol_error("consume encrypt signature from stream would overrun", &packet);
					}

					in_uint8s(s, 8);	/* signature */

					data_offset = s_tell(s);

					inout_uint8p(s, data, s_remaining(s));
					sec_decrypt(data, s_remaining(s));
				}

				if (sec_flags & SEC_LICENCE_NEG)
				{
					s_seek(s, data_offset);
					licence_process(s);
					continue;
				}

				if (sec_flags & 0x0400)	/* SEC_REDIRECT_ENCRYPT */
				{
					uint8 swapbyte;

					if (!s_check_rem(s, 8)) {
						rdp_protocol_error("consume redirect signature from stream would overrun", &packet);
					}

					in_uint8s(s, 8);	/* signature */

					data_offset = s_tell(s);

					inout_uint8p(s, data, s_remaining(s));
					sec_decrypt(data, s_remaining(s));

					/* Check for a redirect packet, starts with 00 04 */
					if (data[0] == 0 && data[1] == 4)
					{
						/* for some reason the PDU and the length seem to be swapped.
						   This isn't good, but we're going to do a byte for byte
						   swap.  So the first foure value appear as: 00 04 XX YY,
						   where XX YY is the little endian length. We're going to
						   use 04 00 as the PDU type, so after our swap this will look
						   like: XX YY 04 00 */
						swapbyte = data[0];
						data[0] = data[2];
						data[2] = swapbyte;

						swapbyte = data[1];
						data[1] = data[3];
						data[3] = swapbyte;

						swapbyte = data[2];
						data[2] = data[3];
						data[3] = swapbyte;
					}
#ifdef WITH_DEBUG
					/* warning!  this debug statement will show passwords in the clear! */
					hexdump(s->p, s_remaining(s));
#endif
				}
			}
			else
			{
				if ((sec_flags & 0xffff) == SEC_LICENCE_NEG)
				{
					licence_process(s);
					continue;
				}
			}

			s_seek(s, data_offset);
		}

		if (channel != MCS_GLOBAL_CHANNEL)
		{
			channel_process(s, channel);
			if (rdpver != NULL)
				*rdpver = 0xff;
			return s;
		}

		return s;
	}

	return NULL;
}

/* Establish a secure connection */
RD_BOOL
sec_connect(char *server, char *username, char *domain, char *password, RD_BOOL reconnect)
{
	uint32 selected_proto;
	STREAM mcs_data;

	/* Start a MCS connect sequence */
	//建立X224链接
	if (!mcs_connect_start(server, username, domain, password, reconnect, &selected_proto))
		return False;

	/* We exchange some RDP data during the MCS-Connect */
	mcs_data = s_alloc(512);
	//构造Client MCS Connect Initial中的Client Data Blocks
	sec_out_mcs_data(mcs_data, selected_proto);

	/* finialize the MCS connect sequence */
	if (!mcs_connect_finalize(mcs_data))
		return False;

	/* sec_process_mcs_data(&mcs_data); */
	if (g_encryption)
		//Client Security Exchange 发送客户端随机数（已用公钥加密）
		sec_establish_key();
	s_free(mcs_data);
	return True;
}

/* Disconnect a connection */
void
sec_disconnect(void)
{
	mcs_disconnect();
}

/* reset the state of the sec layer */
void
sec_reset_state(void)
{
	g_server_rdp_version = 0;
	g_sec_encrypt_use_count = 0;
	g_sec_decrypt_use_count = 0;
	g_licence_issued = 0;
	g_licence_error_result = 0;
	mcs_reset_state();
}
