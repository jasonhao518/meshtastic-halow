#include <mbedtls/bignum.h>

#include <stddef.h>
#include <stdlib.h>

struct crypto_bignum;

extern int mmint_os_get_random(unsigned char *buf, size_t len);

static int mmint_bignum_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    return mmint_os_get_random(out, len);
}

struct crypto_bignum *mmint_crypto_bignum_init(void)
{
    mbedtls_mpi *bn = (mbedtls_mpi *)malloc(sizeof(*bn));
    if (!bn)
    {
        return NULL;
    }

    mbedtls_mpi_init(bn);
    return (struct crypto_bignum *)bn;
}

struct crypto_bignum *mmint_crypto_bignum_init_set(const unsigned char *buf, size_t len)
{
    mbedtls_mpi *bn = (mbedtls_mpi *)malloc(sizeof(*bn));
    if (!bn)
    {
        return NULL;
    }

    mbedtls_mpi_init(bn);
    if (mbedtls_mpi_read_binary(bn, buf, len) != 0)
    {
        mbedtls_mpi_free(bn);
        free(bn);
        return NULL;
    }

    return (struct crypto_bignum *)bn;
}

struct crypto_bignum *mmint_crypto_bignum_init_uint(unsigned int val)
{
    mbedtls_mpi *bn = (mbedtls_mpi *)malloc(sizeof(*bn));
    if (!bn)
    {
        return NULL;
    }

    mbedtls_mpi_init(bn);
    if (mbedtls_mpi_lset(bn, (int)val) != 0)
    {
        mbedtls_mpi_free(bn);
        free(bn);
        return NULL;
    }

    return (struct crypto_bignum *)bn;
}

void mmint_crypto_bignum_deinit(struct crypto_bignum *n, int clear)
{
    if (!n)
    {
        return;
    }

    mbedtls_mpi *bn = (mbedtls_mpi *)n;
    (void)clear;
    mbedtls_mpi_free(bn);
    free(n);
}

int mmint_crypto_bignum_to_bin(const struct crypto_bignum *a, unsigned char *buf, size_t buflen, size_t padlen)
{
    size_t n = mbedtls_mpi_size((const mbedtls_mpi *)a);

    if (n < padlen)
    {
        n = padlen;
    }
    if (n > buflen)
    {
        return -1;
    }

    return mbedtls_mpi_write_binary((const mbedtls_mpi *)a, buf, n) ? -1 : (int)n;
}

int mmint_crypto_bignum_rand(struct crypto_bignum *r, const struct crypto_bignum *m)
{
    return mbedtls_mpi_random((mbedtls_mpi *)r, 0, (const mbedtls_mpi *)m, mmint_bignum_rng, NULL) ? -1 : 0;
}

int mmint_crypto_bignum_add(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return mbedtls_mpi_add_mpi((mbedtls_mpi *)c, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ? -1 : 0;
}

int mmint_crypto_bignum_mod(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return mbedtls_mpi_mod_mpi((mbedtls_mpi *)c, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ? -1 : 0;
}

int mmint_crypto_bignum_exptmod(
    const struct crypto_bignum *a,
    const struct crypto_bignum *b,
    const struct crypto_bignum *c,
    struct crypto_bignum *d)
{
    if (b == d || c == d)
    {
        mbedtls_mpi R;
        mbedtls_mpi_init(&R);

        int rc = mbedtls_mpi_exp_mod(&R, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b, (const mbedtls_mpi *)c, NULL) ||
                 mbedtls_mpi_copy((mbedtls_mpi *)d, &R) ?
                     -1 :
                     0;
        mbedtls_mpi_free(&R);
        return rc;
    }

    return mbedtls_mpi_exp_mod((mbedtls_mpi *)d, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b,
                               (const mbedtls_mpi *)c, NULL) ? -1 : 0;
}

int mmint_crypto_bignum_inverse(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return mbedtls_mpi_inv_mod((mbedtls_mpi *)c, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ? -1 : 0;
}

int mmint_crypto_bignum_sub(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return mbedtls_mpi_sub_mpi((mbedtls_mpi *)c, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ? -1 : 0;
}

int mmint_crypto_bignum_div(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    mbedtls_mpi R;
    mbedtls_mpi_init(&R);

    int rc = mbedtls_mpi_div_mpi(&R, NULL, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ||
             mbedtls_mpi_copy((mbedtls_mpi *)c, &R) ?
                 -1 :
                 0;
    mbedtls_mpi_free(&R);
    return rc;
}

int mmint_crypto_bignum_addmod(
    const struct crypto_bignum *a,
    const struct crypto_bignum *b,
    const struct crypto_bignum *c,
    struct crypto_bignum *d)
{
    return mbedtls_mpi_add_mpi((mbedtls_mpi *)d, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ||
           mbedtls_mpi_mod_mpi((mbedtls_mpi *)d, (mbedtls_mpi *)d, (const mbedtls_mpi *)c) ?
               -1 :
               0;
}

int mmint_crypto_bignum_mulmod(
    const struct crypto_bignum *a,
    const struct crypto_bignum *b,
    const struct crypto_bignum *c,
    struct crypto_bignum *d)
{
    return mbedtls_mpi_mul_mpi((mbedtls_mpi *)d, (const mbedtls_mpi *)a, (const mbedtls_mpi *)b) ||
           mbedtls_mpi_mod_mpi((mbedtls_mpi *)d, (mbedtls_mpi *)d, (const mbedtls_mpi *)c) ?
               -1 :
               0;
}

int mmint_crypto_bignum_sqrmod(const struct crypto_bignum *a, const struct crypto_bignum *b, struct crypto_bignum *c)
{
    return mbedtls_mpi_mul_mpi((mbedtls_mpi *)c, (const mbedtls_mpi *)a, (const mbedtls_mpi *)a) ||
           mbedtls_mpi_mod_mpi((mbedtls_mpi *)c, (mbedtls_mpi *)c, (const mbedtls_mpi *)b) ?
               -1 :
               0;
}

int mmint_crypto_bignum_rshift(const struct crypto_bignum *a, int n, struct crypto_bignum *r)
{
    return mbedtls_mpi_copy((mbedtls_mpi *)r, (const mbedtls_mpi *)a) || mbedtls_mpi_shift_r((mbedtls_mpi *)r, n) ? -1 : 0;
}

int mmint_crypto_bignum_cmp(const struct crypto_bignum *a, const struct crypto_bignum *b)
{
    return mbedtls_mpi_cmp_mpi((const mbedtls_mpi *)a, (const mbedtls_mpi *)b);
}

int mmint_crypto_bignum_is_zero(const struct crypto_bignum *a)
{
    return (mbedtls_mpi_cmp_int((const mbedtls_mpi *)a, 0) == 0);
}

int mmint_crypto_bignum_is_one(const struct crypto_bignum *a)
{
    return (mbedtls_mpi_cmp_int((const mbedtls_mpi *)a, 1) == 0);
}

int mmint_crypto_bignum_is_odd(const struct crypto_bignum *a)
{
    return mbedtls_mpi_get_bit((const mbedtls_mpi *)a, 0);
}

int mmint_crypto_bignum_legendre(const struct crypto_bignum *a, const struct crypto_bignum *p)
{
    mbedtls_mpi exp;
    mbedtls_mpi tmp;
    mbedtls_mpi_init(&exp);
    mbedtls_mpi_init(&tmp);

    int res = -2;

    if (mbedtls_mpi_sub_int(&exp, (const mbedtls_mpi *)p, 1) == 0 &&
        mbedtls_mpi_shift_r(&exp, 1) == 0 &&
        mbedtls_mpi_exp_mod(&tmp, (const mbedtls_mpi *)a, &exp, (const mbedtls_mpi *)p, NULL) == 0)
    {
        if (mbedtls_mpi_cmp_int(&tmp, 1) == 0)
        {
            res = 1;
        }
        else if (mbedtls_mpi_cmp_int(&tmp, 0) == 0)
        {
            res = 0;
        }
        else
        {
            res = -1;
        }
    }

    mbedtls_mpi_free(&tmp);
    mbedtls_mpi_free(&exp);
    return res;
}
