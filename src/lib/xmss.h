// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.
#pragma once
#include <stdint.h>
#include "macros.h"
#include "parameters.h"
#include "shash.h"
#include "adrs.h"
#include "fips202.h"
#include "wotsp.h"

#pragma pack(push, 1)
union xmss_sk_t {
  uint8_t raw[132];
  struct {
    uint32_t index;
    uint8_t seed[32];
    uint8_t prf_seed[32];
    uint8_t pub_seed[32];
    uint8_t root[32];
  };
  struct {
    uint32_t _padding;
    uint8_t raw[96];
    uint8_t _padding2[32];
  } seeds;
};

union xmss_pk_t {
  uint8_t raw[64];
  struct {
    uint8_t root[32];
    uint8_t pub_seed[32];
  };
};

union xmss_digest_t {
  uint8_t raw[64];
  struct {
    uint8_t hash[32];
    uint8_t randomness[32];
  };
};

union xmss_signature_t {
  uint8_t raw[XMSS_SIGSIZE];
  struct {
    uint32_t index;
    uint8_t randomness[32];
    uint8_t wots_sig[32*67];
    uint8_t auth_path[32*(XMSS_H-1)];
  };
};
#pragma pack(pop)

__INLINE void xmss_pk(xmss_pk_t* pk_out, const xmss_sk_t* sk_in)
{
    memcpy(pk_out->root, sk_in->root, 32);
    memcpy(pk_out->pub_seed, sk_in->pub_seed, 32);
}

__INLINE void xmss_ltree_gen(uint8_t* leaf, uint8_t* tmp_wotspk, const uint8_t* pub_seed, uint16_t index)
{
    // WARNING: This functions collapses wotspk and will be destroyed after the call
    uint8_t l = WOTS_LEN;
    uint8_t tree_height = 0;

    while (l>1) {
        const uint8_t bound = l >> 1u;
        for (uint8_t i = 0; i<bound; i++) {
            union hashh_t hashh_in;
            memset(hashh_in.basic.raw, 0, 96);
            memcpy(hashh_in.basic.key, pub_seed, 32);

            hashh_in.basic.adrs.type = HtoNL(SHASH_TYPE_H);
            hashh_in.basic.adrs.trees.ltree = HtoNL(index);
            hashh_in.basic.adrs.trees.height = HtoNL(tree_height);
            hashh_in.basic.adrs.trees.index = HtoNL(i);

            const unsigned char* const in = tmp_wotspk+i*2*WOTS_N;
            unsigned char* const out = tmp_wotspk+i*WOTS_N;

            shash_h(out, in, &hashh_in);
        }

        if (l & 1u) {
            memcpy(tmp_wotspk+(l >> 1u)*WOTS_N, tmp_wotspk+(l-1u)*WOTS_N, WOTS_N);
            l = (uint8_t) ((l >> 1u)+1u);
        }
        else {
            l = (l >> 1u);
        }

        tree_height++;
    }
    memcpy(leaf, tmp_wotspk, WOTS_N);
}

__INLINE void xmss_treehash(
        uint8_t* root_out,
        uint8_t* authpath,
        const uint8_t* nodes,
        const uint8_t* pub_seed,
        const uint16_t leaf_index)
{
    union hashh_t h_in;
    uint8_t stack[XMSS_STK_SIZE];
    uint16_t stack_levels[XMSS_STK_LEVELS];
    uint32_t stack_offset = 0;

    for (uint16_t idx = 0; idx<XMSS_NUM_NODES; idx++) {
        // bring node in
        memcpy(stack+stack_offset*WOTS_N, nodes+idx*WOTS_N, WOTS_N);

        stack_levels[stack_offset] = 0;
        stack_offset++;
        if ((leaf_index ^ 0x1u)==idx) {
            memcpy(authpath, stack+(stack_offset-1)*WOTS_N, WOTS_N);
        }

        while (stack_offset>1 && stack_levels[stack_offset-1]==stack_levels[stack_offset-2]) {
            uint16_t tree_idx = (idx >> (stack_levels[stack_offset-1u]+1u));
            memset(h_in.raw, 0, 96);

            h_in.basic.adrs.type = HtoNL(SHASH_TYPE_HASH);
            h_in.basic.adrs.trees.height = HtoNL(stack_levels[stack_offset-1u]);
            h_in.basic.adrs.trees.index = HtoNL(tree_idx);

            memcpy(h_in.basic.key, pub_seed, WOTS_N);

            unsigned char* in_out = stack+(stack_offset-2)*WOTS_N;

            shash_h(in_out, in_out, &h_in);

            stack_levels[stack_offset-2]++;
            stack_offset--;

            if (((leaf_index >> stack_levels[stack_offset-1u]) ^ 0x1)==tree_idx) {
                memcpy(authpath+stack_levels[stack_offset-1]*WOTS_N, stack+(stack_offset-1)*WOTS_N, WOTS_N);
            }
        }
    }
    memcpy(root_out, stack, WOTS_N);
}

__INLINE void xmss_randombits(uint8_t* random_bits, const uint8_t sk_seed[48])
{
    const uint8_t output_size = 3*WOTS_N;
#ifdef LEDGER_SPECIFIC
    cx_sha3_t hash_sha3;
    cx_sha3_xof_init(&hash_sha3,256,output_size);
    cx_hash(&hash_sha3.header, CX_LAST, sk_seed, sizeof(sk_seed), randombits);
#else
    shake256(random_bits, output_size, sk_seed, 48);
#endif
}

__INLINE void xmss_get_seed_i(uint8_t* seed, const xmss_sk_t* sk, uint16_t idx)
{
    union shash_input_t prf_in;
    PRF_init(&prf_in, SHASH_TYPE_PRF);
    memcpy(prf_in.key, sk->seed, WOTS_N);
    prf_in.adrs.otshash.OTS = HtoNL(idx);
    shash96(seed, &prf_in);
}

__INLINE void xmss_gen_keys_1_get_seeds(xmss_sk_t* sk, const uint8_t* sk_seed)
{
    xmss_randombits(sk->seeds.raw, sk_seed);
}

__INLINE void xmss_gen_keys_2_get_nodes(uint8_t* xmss_node, const xmss_sk_t* sk, uint16_t idx)
{
    uint8_t wotspk[WOTS_LEN*WOTS_N];
    uint8_t seed[WOTS_N];

    xmss_get_seed_i(seed, sk, idx);
    wotsp_gen_pk(wotspk, seed, sk->pub_seed, idx);

    xmss_ltree_gen(xmss_node, wotspk, sk->pub_seed, idx);
}

__INLINE void xmss_gen_keys_3_get_root(const uint8_t* xmss_nodes, xmss_sk_t* sk)
{
    uint8_t authpath[(XMSS_H+1)*WOTS_N];
    xmss_treehash(sk->root, authpath, xmss_nodes, sk->pub_seed, 0);
}

__INLINE void xmss_gen_keys(xmss_sk_t* sk, const uint8_t* sk_seed)
{
    xmss_gen_keys_1_get_seeds(sk, sk_seed);

    uint8_t xmss_nodes[XMSS_NODES_BUF_SZ];
    for (uint16_t idx = 0; idx<XMSS_NUM_NODES; idx++) {
        xmss_gen_keys_2_get_nodes(xmss_nodes+idx*WOTS_N, sk, idx);
    }

    xmss_gen_keys_3_get_root(xmss_nodes, sk);
}

__INLINE void xmss_sign(
        xmss_signature_t* sig,
        const uint8_t msg[32],
        const xmss_sk_t* sk,
        const uint8_t xmss_nodes[XMSS_NODES_BUF_SZ],
        const uint16_t index)
{
    union xmss_digest_t msg_digest;
    {
        {
            // get randomness
            union shash_input_t prf_in;
            PRF_init(&prf_in, SHASH_TYPE_PRF);
            memcpy(prf_in.key, sk->prf_seed, WOTS_N);
            prf_in.R.index = HtoNL(index);
            shash96(msg_digest.randomness, &prf_in);
        }

        {
            // Digest hash
            union hashh_t h_in;
            memset(h_in.raw, 0, 160);
            h_in.digest.type[31] = SHASH_TYPE_HASH;
            memcpy(h_in.digest.R, msg_digest.randomness, WOTS_N);
            memcpy(h_in.digest.root, sk->root, 32);
            h_in.digest.index = NtoHL(index);
            memcpy(h_in.digest.msg_hash, msg, 32);
            shash160(msg_digest.hash, &h_in);
        }
    }

    // Signature xmss_signature_t
    sig->index = NtoHL(index);
    memcpy(sig->randomness, msg_digest.randomness, 32);

    {
        uint8_t seed_i[32];
        xmss_get_seed_i(seed_i, sk, index);
        wotsp_sign(sig->wots_sig, msg_digest.hash, sk->pub_seed, seed_i, index);
    }

    {
        uint8_t root[32];
        xmss_treehash(root, sig->auth_path, xmss_nodes, sk->pub_seed, index);
    }
}
