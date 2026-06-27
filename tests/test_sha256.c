/* Host-side unit test for the SHA-256 implementation that ships in
 * lib/libstink_sha256.c. Compiled with the host's gcc against the same
 * source file, so any divergence between freestanding and hosted
 * environments would show up immediately. Test vectors are the canonical
 * FIPS 180-4 examples plus the long "million a" stress case. */
#include <stdio.h>
#include <string.h>

extern void sha256(const void *data, unsigned int len, unsigned char out[32]);

static int hex_eq(const unsigned char digest[32], const char *expected_hex)
{
	for (int i = 0; i < 32; i++) {
		char buf[3];
		snprintf(buf, sizeof(buf), "%02x", digest[i]);
		if (buf[0] != expected_hex[i * 2] || buf[1] != expected_hex[i * 2 + 1])
			return 0;
	}
	return 1;
}

static int run(const char *label, const void *data, unsigned int len,
               const char *expected_hex)
{
	unsigned char digest[32];
	sha256(data, len, digest);
	if (!hex_eq(digest, expected_hex)) {
		printf("FAIL %s: digest mismatch\n  expected %s\n  got      ",
		       label, expected_hex);
		for (int i = 0; i < 32; i++) printf("%02x", digest[i]);
		printf("\n");
		return 1;
	}
	printf("ok   %s\n", label);
	return 0;
}

int main(void)
{
	int failures = 0;

	failures += run("empty",
	    "", 0,
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	failures += run("abc",
	    "abc", 3,
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	failures += run("two-block",
	    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	    56,
	    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	/* The "one million a" canonical stress test: builds the buffer at
	 * runtime to avoid a 1 MiB string literal in the test source. */
	{
		static char million_a[1000000];
		memset(million_a, 'a', sizeof(million_a));
		failures += run("million a",
		    million_a, sizeof(million_a),
		    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
