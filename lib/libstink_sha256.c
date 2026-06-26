/* SHA-256 (FIPS 180-4) for StinkOS userland.
 *
 * Standalone and freestanding: no libc, no 64-bit arithmetic (so it links
 * without libgcc), operates on a single flat buffer already held in memory.
 * stink-pkg uses it to verify that a downloaded package matches the digest
 * published in the repo index before unpacking anything onto disk.
 *
 * The message length is carried as two 32-bit halves to build the 64-bit
 * big-endian length trailer the spec requires, avoiding any 64-bit shift the
 * freestanding toolchain would otherwise pull out of libgcc. Inputs here are
 * a few KiB at most, so the high half is effectively always zero, but the
 * split keeps the padding correct for any 32-bit length.
 */

static unsigned int rotr(unsigned int x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

/* The 64 round constants: the first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes (FIPS 180-4 section 4.2.2). */
static const unsigned int K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* Mix one 64-byte block (big-endian words) into the running state H[8]. */
static void sha256_block(unsigned int H[8], const unsigned char *p)
{
	unsigned int w[64];
	for (int t = 0; t < 16; t++)
		w[t] = ((unsigned int)p[t * 4]     << 24) |
		       ((unsigned int)p[t * 4 + 1] << 16) |
		       ((unsigned int)p[t * 4 + 2] <<  8) |
		        (unsigned int)p[t * 4 + 3];
	for (int t = 16; t < 64; t++) {
		unsigned int s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
		unsigned int s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}

	unsigned int a = H[0], b = H[1], c = H[2], d = H[3];
	unsigned int e = H[4], f = H[5], g = H[6], h = H[7];
	for (int t = 0; t < 64; t++) {
		unsigned int S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		unsigned int ch  = (e & f) ^ ((~e) & g);
		unsigned int t1  = h + S1 + ch + K[t] + w[t];
		unsigned int S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
		unsigned int t2  = S0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	H[0] += a; H[1] += b; H[2] += c; H[3] += d;
	H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

/* Hash 'len' bytes at 'data' into out[32] (raw digest, big-endian words). */
void sha256(const void *data, unsigned int len, unsigned char out[32])
{
	unsigned int H[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};
	const unsigned char *p = (const unsigned char *)data;

	unsigned int full = len & ~63u;          /* bytes in whole 64-byte blocks */
	for (unsigned int i = 0; i < full; i += 64)
		sha256_block(H, p + i);

	/* Final block(s): the leftover bytes, a 0x80 terminator, zero padding and
	 * a 64-bit big-endian bit length. Needs a second block when the leftover
	 * (plus terminator) leaves no room for the 8-byte length. */
	unsigned char tail[128];
	unsigned int rem = len - full;
	for (unsigned int j = 0; j < rem; j++)
		tail[j] = p[full + j];
	tail[rem] = 0x80;
	unsigned int tlen = (rem >= 56) ? 128u : 64u;
	for (unsigned int j = rem + 1; j < tlen - 8; j++)
		tail[j] = 0;

	unsigned int bits_hi = len >> 29;        /* len * 8, high 32 bits */
	unsigned int bits_lo = len << 3;         /* len * 8, low 32 bits  */
	tail[tlen - 8] = (unsigned char)(bits_hi >> 24);
	tail[tlen - 7] = (unsigned char)(bits_hi >> 16);
	tail[tlen - 6] = (unsigned char)(bits_hi >>  8);
	tail[tlen - 5] = (unsigned char)(bits_hi);
	tail[tlen - 4] = (unsigned char)(bits_lo >> 24);
	tail[tlen - 3] = (unsigned char)(bits_lo >> 16);
	tail[tlen - 2] = (unsigned char)(bits_lo >>  8);
	tail[tlen - 1] = (unsigned char)(bits_lo);

	sha256_block(H, tail);
	if (tlen == 128)
		sha256_block(H, tail + 64);

	for (int j = 0; j < 8; j++) {
		out[j * 4]     = (unsigned char)(H[j] >> 24);
		out[j * 4 + 1] = (unsigned char)(H[j] >> 16);
		out[j * 4 + 2] = (unsigned char)(H[j] >>  8);
		out[j * 4 + 3] = (unsigned char)(H[j]);
	}
}
