#include "beer_hash.h"

#include <stdio.h>
#include <string.h>

/* Expected values generated with Python hashlib/hmac over the deterministic

 * pattern byte[i] = (i * 7 + seed) & 0xff. */
static void pattern(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)((i * 7 + seed) & 0xff);
}

typedef struct { BeerHashAlg alg; size_t key_len, msg_len; const char *hex; } Case;

static const Case hash_cases[] = {
    {BEER_HASH_MD5, 0, 0, "d41d8cd98f00b204e9800998ecf8427e"},
    {BEER_HASH_MD5, 0, 3, "c9aee4810523ef8658121b8d492c6b41"},
    {BEER_HASH_MD5, 0, 55, "52c0e574e1198de5fe3f8f11440dcb1b"},
    {BEER_HASH_MD5, 0, 56, "46c9907fc908ee68b1e7b8e71286a518"},
    {BEER_HASH_MD5, 0, 63, "a62f6d59e837867693f042f5b8f5a236"},
    {BEER_HASH_MD5, 0, 64, "7160b8fb5e9e4023d549c3971fbaeead"},
    {BEER_HASH_MD5, 0, 65, "70bd662e7aefbda85a0f7244167b7897"},
    {BEER_HASH_MD5, 0, 111, "13f00cb15f0c3fe588d1c17cde14b38f"},
    {BEER_HASH_MD5, 0, 112, "5fe6a9f76b541ec645559589da9a088a"},
    {BEER_HASH_MD5, 0, 119, "e84905d4214f4d1ca56c2cdcc152b143"},
    {BEER_HASH_MD5, 0, 120, "e3eb5a6c8669ea01a8c185b8abc8a5dc"},
    {BEER_HASH_MD5, 0, 127, "acce2474d6cc8302120d09c818d17ef7"},
    {BEER_HASH_MD5, 0, 128, "10b2da1a82f16d99a81a7203fe9f02cb"},
    {BEER_HASH_MD5, 0, 129, "03fefbbcebe2959cf2270241aafe0250"},
    {BEER_HASH_MD5, 0, 1000, "10046f077f2082ac19676b8079f1cb1a"},
    {BEER_HASH_SHA1, 0, 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {BEER_HASH_SHA1, 0, 3, "4201de9f98cb0a9b8cf52398be7802b55d45266a"},
    {BEER_HASH_SHA1, 0, 55, "ddf57317ef34bfee3b6df83d359098930eb278bc"},
    {BEER_HASH_SHA1, 0, 56, "a0d492bb0fc889d0eca3bc137066ab6f4f74f369"},
    {BEER_HASH_SHA1, 0, 63, "c55856749bef509bdfe6bfebfc7bf4e793e82132"},
    {BEER_HASH_SHA1, 0, 64, "bede92be29c3874e1b54ddc77988d606fc857a8e"},
    {BEER_HASH_SHA1, 0, 65, "b05a80522b053d6dc7e0a517d0e70212c7dad11f"},
    {BEER_HASH_SHA1, 0, 111, "4a01b1272627e5be00f57bc3c3029c23a452e629"},
    {BEER_HASH_SHA1, 0, 112, "54c08c2a0ed21de25e3d8606c3929454eb1d3f8b"},
    {BEER_HASH_SHA1, 0, 119, "504e27376a6e0f0dba8295b85cb25dc4dfa17d23"},
    {BEER_HASH_SHA1, 0, 120, "82134b02fb3f702491be9bed581eeab59334acb2"},
    {BEER_HASH_SHA1, 0, 127, "34d5e582029e9b9b85b2febe31da3db7cdabaaea"},
    {BEER_HASH_SHA1, 0, 128, "a09133e6730ffe899efb70204cb5646cd5dc24ee"},
    {BEER_HASH_SHA1, 0, 129, "808aea332ce367541d37adae7f94e59c5c1a934e"},
    {BEER_HASH_SHA1, 0, 1000, "4231a8a50a10fa9758db8ec71fdef855b751048a"},
    {BEER_HASH_SHA256, 0, 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {BEER_HASH_SHA256, 0, 3, "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35"},
    {BEER_HASH_SHA256, 0, 55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
    {BEER_HASH_SHA256, 0, 56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
    {BEER_HASH_SHA256, 0, 63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
    {BEER_HASH_SHA256, 0, 64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
    {BEER_HASH_SHA256, 0, 65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
    {BEER_HASH_SHA256, 0, 111, "67d9492e628fd376e0b2efec8ca2b99b123e202cf620deb270728df979b2f73e"},
    {BEER_HASH_SHA256, 0, 112, "96b928cff8528dbb99602c709a65b846cb6467acb8b722f0d758e4dc27bfc508"},
    {BEER_HASH_SHA256, 0, 119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
    {BEER_HASH_SHA256, 0, 120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
    {BEER_HASH_SHA256, 0, 127, "a8d23e75d936f303d248888d9b165ee543f4cbafcad3c9dd2a79bd84faa11d07"},
    {BEER_HASH_SHA256, 0, 128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
    {BEER_HASH_SHA256, 0, 129, "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4"},
    {BEER_HASH_SHA256, 0, 1000, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"},
    {BEER_HASH_SHA384, 0, 0, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"},
    {BEER_HASH_SHA384, 0, 3, "de5060203c58b419f136c17666d339a05705575827e3600f0475f6fb3089870aede26c5a0903dea5938bcded4ee9e619"},
    {BEER_HASH_SHA384, 0, 55, "fc2728f452750d08b5dbad1aea53202e7fc19c06894d13e36c433b0444a899277c49f16e0577c03db0f0605578836ef6"},
    {BEER_HASH_SHA384, 0, 56, "6c969a865f38d2eaac60b7963a84c97ba27bcb76e589cbe1e2e89ead6aee4ad0337a4268f054be49c604ddd99692036d"},
    {BEER_HASH_SHA384, 0, 63, "be67a9a1162c1d97f183980dc811f4f4f835b8b850eb32ecbf87397b26369bc6646a8f2234940dcff142b627c8e21ebb"},
    {BEER_HASH_SHA384, 0, 64, "99d26430f561864c2f7d90b68650b960c86c85d6bf7f54ceeee404bdd59663a7f05588b5c750b862a2a24f6a9ad4c050"},
    {BEER_HASH_SHA384, 0, 65, "cada16d798ec25243470f7827eb89a2402d52680b092dffc0f59f5a919e5058aebf7ad79dba85c61b179d23534d7c855"},
    {BEER_HASH_SHA384, 0, 111, "341388a9dc2275074e90cf394323761919761c805fd9e370977c9966a0e8c81a52135f02577670b0071638a4a26dbc31"},
    {BEER_HASH_SHA384, 0, 112, "619cc5d06138526d70659eccf602d197e63e1050e22039a7feb40a30a5b2b08fb03729e291df12f8c576e6f1cd8af22a"},
    {BEER_HASH_SHA384, 0, 119, "9c284d266fc91c33a7f6ecbb0326646b3ed15cc33fd8bf968267c21991ca1a434dd65c64ee22c7dd6f5423950f605560"},
    {BEER_HASH_SHA384, 0, 120, "96faef865acb7c55ae34422444b4380e520de69f373ddb14c7ee2420c3f93cc220fca9952c0a4fe9dbfddf350ac40127"},
    {BEER_HASH_SHA384, 0, 127, "f8234ab81f5f9641c656680825a9bbb3c2616bcc80b65d5e479a4d96742e1b8742330c9e8256ef5f03caa932487044ca"},
    {BEER_HASH_SHA384, 0, 128, "e8480e9c4dd90f88104a79cbaccec48edbd798a142b4f241d726dc252f1502350e824c7d18dadd59d7d716919fb8f9bf"},
    {BEER_HASH_SHA384, 0, 129, "430173387764874661acba1e914e50f6d0fbac534809e859b1da4396fad3d5f24c25c6a2d02486d8fbda3994713d981a"},
    {BEER_HASH_SHA384, 0, 1000, "94c38db521ca733b8904c2d14b6e82d33dcfcc26e1318c579dbee1fc2f472019034e792263b9d46e90e700de2f6e7e91"},
    {BEER_HASH_SHA512, 0, 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
    {BEER_HASH_SHA512, 0, 3, "8853167ab14c043919981a6409307ef68b5b726f4b80dcab259daa24b7cabcb8b4bbfd3e2e36ed98465059661f672e6529cdcc673e7eabfa78dd3186745b112a"},
    {BEER_HASH_SHA512, 0, 55, "14fd424b1fcadee624da946ab03f7e1def7c0d6e00f689594319881a26ff30b875ba4c622ac13100c8cc784c9c2eb23159aecbb4a02e3999062f551193e2b256"},
    {BEER_HASH_SHA512, 0, 56, "480fa85be41ef55a41208ca28ffc8743c91cf7d24758defe6f95bfb16de614fc86b701034896b047dd571de4318853d80e0809df162f1752cb26da6ddb94a0dd"},
    {BEER_HASH_SHA512, 0, 63, "ecd42a703a4e93e163d60d55e3785b1a763838b0351bc2e6f7c94b4bfb24f9aa15da5d744ebcebe11f0fc4315d45ba3a047b6e60e07448357f2795bf34b73502"},
    {BEER_HASH_SHA512, 0, 64, "8f3cc30b3fb5bf963688a46488249248ac2c67f0f85a145233c6c1e3c16dcd1df634c07d1d31da02576f65b9cf64e1c3fdb318b689b8a14e2e9552bcf30fb133"},
    {BEER_HASH_SHA512, 0, 65, "22050d1b2c5bd016c4b04c3e84f513ffbbbc057f83dc9ab196fee6684e5ee19d98b84c6538b3c5e1fc67dbc484e9d0a198254e41d0fa0cf5106991e106632419"},
    {BEER_HASH_SHA512, 0, 111, "68cffa6d0d76f309c9ce0d35280939f8e25990c43b7b086ccdf709be35b07d4ddba599541ff2b1c19d34ea49aeafb9659adb7ac3c0b078bb30a22d57fc6687ef"},
    {BEER_HASH_SHA512, 0, 112, "d0865c524d1dddf7c23b799c413f5adcd7caefd3f66a9b49750ec81066012c25a8bcf94ddea6dc525691673097ca40e0101e897fc97218cfdb0704084e2bef4b"},
    {BEER_HASH_SHA512, 0, 119, "236bdd7f38a611b5014b239245c381ae5d20a96f1e5b3178227c00056b7fa8c44ef54880085d82b7e20cd65f2bfda1326696c3f94a6a5bad0cb5ce289aa46167"},
    {BEER_HASH_SHA512, 0, 120, "4bd16fefaa35cc2df9fbb8ff379ec04a4070ffd5d4992574af239fa175534df87cbedceeb96be08e15090a78b83a328a2900c5411683ab47bab914591048548f"},
    {BEER_HASH_SHA512, 0, 127, "e0b6a20f1c0c88970a9340152cd5a1c1ecf3d3b8de55102741879438079473540133b812706e5dbec322c8c9523b6fc8c6d16ee626e87ad5fe3d2916afedc369"},
    {BEER_HASH_SHA512, 0, 128, "99b16f17aa0b969a5b8f08f367719d516e330ccd2660b6f0688ec031dbc783de50a1cd185a2568dba75070a2403d17d4741d163578515dfd2ff756ddfe4d47b1"},
    {BEER_HASH_SHA512, 0, 129, "a1556e29185778aa5991e34b8884c840d589f0fbb4b8ed590e51e9ac4eb03a008125000db2671f8fe7f485b59a77b518670078ecb41a54b4cd02a7f1d2ca4c6d"},
    {BEER_HASH_SHA512, 0, 1000, "00e36fccf193e59697a92b5ab24666ce6326d7fa16bf10832d0991ddc591112e9dfa6a636950ed9c4d67344a760654c2ff7785e1d60094d651038735b5dccabd"},
};

static const Case hmac_cases[] = {
    {BEER_HASH_MD5, 0, 10, "4989377e69d2481341603cfc759b3e4a"},
    {BEER_HASH_MD5, 4, 28, "d952fcb7a5681052b5313d7cb1de2fc2"},
    {BEER_HASH_MD5, 20, 50, "18199141013d21c2c5f11d14aa8f84ac"},
    {BEER_HASH_MD5, 64, 64, "06a8e600081019c37a916a6ceba6b9a3"},
    {BEER_HASH_MD5, 65, 5, "6a7cdd91d4f8be554803e7f5f7ebf6ab"},
    {BEER_HASH_MD5, 128, 200, "d7b24b5ec0b55d0b000946d12ff6e31f"},
    {BEER_HASH_MD5, 131, 60, "16ca65b15ca5cb312e7efbb9897a380d"},
    {BEER_HASH_MD5, 200, 300, "8ec3c9133058cebffa6c2657624a8160"},
    {BEER_HASH_SHA1, 0, 10, "9e859403b46a5fd413eabe44025d3e4207071702"},
    {BEER_HASH_SHA1, 4, 28, "2d2ffb7b6e427a000557a955d359260f696859bb"},
    {BEER_HASH_SHA1, 20, 50, "8126d66360f522bfe73b02a2fc72bb113e688282"},
    {BEER_HASH_SHA1, 64, 64, "b0734f09b926bdce704e7509ea8bef9e5b0c707a"},
    {BEER_HASH_SHA1, 65, 5, "dcf10e0c6f843ba472ce77722861ebe6141d9f0f"},
    {BEER_HASH_SHA1, 128, 200, "74b492719a78a623d3799f784b524d20fed88568"},
    {BEER_HASH_SHA1, 131, 60, "204f9585f933a5640440378bc3cdc6a7edf9fcd6"},
    {BEER_HASH_SHA1, 200, 300, "89f98a32973bed7c390fdcb4075775003bf5e3f1"},
    {BEER_HASH_SHA256, 0, 10, "b1550b05f2392bf88c7dde561ad7b934d90313496aa0b9d1364c19796c894ea4"},
    {BEER_HASH_SHA256, 4, 28, "57d5d650783c96dc0dbffa45b376df187bb5af3c6f6165c7318f54f46b8770ff"},
    {BEER_HASH_SHA256, 20, 50, "535cc1a6ede35c4b4b88235b9947d3f730261edd38acfed24bbaf3d5dec00e42"},
    {BEER_HASH_SHA256, 64, 64, "2435c066a9ca06d7e7b12705f1390044762724b4c76caa4dc3d7545937862d49"},
    {BEER_HASH_SHA256, 65, 5, "862d12d27f6ac8558b22ce4bf26425019581657146874e22dcd8823b8dc4904d"},
    {BEER_HASH_SHA256, 128, 200, "7877b60997b3ad5b8a14f6fc88f778b9e9ef722d1e4fa20575cd987782e4c24b"},
    {BEER_HASH_SHA256, 131, 60, "6d77fb5f695e9538910ac7be88ba51a0e41b59c901b1dd73fb25e47c8c69821f"},
    {BEER_HASH_SHA256, 200, 300, "f762f1d745c50dfa0d3454394b3bb4b9fd16fc4a3ed900fe625774ad9c853faf"},
    {BEER_HASH_SHA384, 0, 10, "3c2b8350e34b5bb46ace9c1dbd6235938c1193cace1900e17f2e7f15e693be5bf3217f00ba253d79f145e4ffa9318fe2"},
    {BEER_HASH_SHA384, 4, 28, "eeb7f6bd0d5ef85e6d7c27b2cb385c2e136ccf4e77616cb0982b3f959841ccde423d0b02445b112c66156b9f3ef775e5"},
    {BEER_HASH_SHA384, 20, 50, "d4021b31b72765c05374b21080b632560604399e1613f2220f738a646af7c5addfb65b10b2ca40e2b348244f3a87ccfd"},
    {BEER_HASH_SHA384, 64, 64, "c965e91b0d0696e7d9ee2843d216af050c38f8e3bce46beef2496c702f972b3be058639bf7a55c85762ba7c9effe8f7d"},
    {BEER_HASH_SHA384, 65, 5, "12b11d77c4016f6a39aaa410738ad4619935e828c764f458150b39cbae2db3e3d6fa7cd748b3c6b5e23fe16c36d337ed"},
    {BEER_HASH_SHA384, 128, 200, "51bcf6e25b9f1f68f0ecc4b2f87edfc81b9f0e4fb01973e6d6bd6c017ed19192fff3ecc4574a6efd381b4d0bb82ea3f3"},
    {BEER_HASH_SHA384, 131, 60, "857490228ef3b6582a4b0a7d81410808e80acada2fb80ff309d02a053700e51b4d337dd00d222e9b0b3b459ebca3f552"},
    {BEER_HASH_SHA384, 200, 300, "1fc844e75f61e4520f434d5cd217ba465f68fde9d33a32e6e29aca1cc78634fadef106134c23d45fa0c0056be4d1b15e"},
    {BEER_HASH_SHA512, 0, 10, "43bd460c4ba0d62df6166f7304b7af2bb91f5fb6363e265190794497ec6e6bf5b868f83e0b37633cd5f94eb6842654964d85c37a60c48c701a9f4208b45675cd"},
    {BEER_HASH_SHA512, 4, 28, "8f1d08672785fdd26c18d3510d5181040021af55b585366b3285196c51f058d248f5339b9eeefeea19a33b8c13ab30782050cce9915afe2734da697b1c876f12"},
    {BEER_HASH_SHA512, 20, 50, "b0b0998e38c5d8da3d73ce0a2b2da7889495b493dbf8814ea7ecda2a9835daebed734966e560c4f819be7bc7e614992fd64687e4a4cdde3f04d2d69c1b975378"},
    {BEER_HASH_SHA512, 64, 64, "aa172804619c206aa13a74044423c965888e3a455771945f1e0cafeb124f90ed0c13f5347ff122aa95ee118784ad144f5651bafc11b632fa2d17fd30e4e0ccb2"},
    {BEER_HASH_SHA512, 65, 5, "27cb3411e7310a52694db7d00a9116af4bcead92a1e0ac984ab993cac4e9690410d3891f8d2fc2d92b9a97c46e4677b7b78e874d9e08e14e6fde8e3b1f6b2a0a"},
    {BEER_HASH_SHA512, 128, 200, "39e1b7fbf1a820e011602f99dea882fb85b0f0fdc033f8f3ff16c1b83cc0359de3c5a1369fef38a15be8b27d5d1a911d12ed2d29306af3dc484c73c8e590c5b4"},
    {BEER_HASH_SHA512, 131, 60, "e21befec2b336f8fb863779139218253849554e12fea0d0f416cdc89b8fe353a78580fcd8564a813e6690747badc702aa66b45dab1b6ddc8bd020833e5f81435"},
    {BEER_HASH_SHA512, 200, 300, "986cf974697effca89eb745214e11e7be3051db41c828218337e1626433b7692a63d02402eeb190cd0cb85e984ddebffa9ccf1828cfb2afaa63ff9fb98e564a5"},
};

static void to_hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i) sprintf(out + 2 * i, "%02x", d[i]);
}

int main(void)
{
    int failures = 0;
    uint8_t msg[1024], key[256], digest[BEER_HASH_MAX_DIGEST];
    char hex[2 * BEER_HASH_MAX_DIGEST + 1];

    for (size_t i = 0; i < sizeof(hash_cases) / sizeof(hash_cases[0]); ++i) {
        const Case *c = &hash_cases[i];
        pattern(msg, c->msg_len, 3);
        beer_hash(c->alg, msg, c->msg_len, digest);
        to_hex(digest, beer_hash_digest_size(c->alg), hex);
        if (strcmp(hex, c->hex)) { printf("FAIL hash alg=%d len=%zu\n", c->alg, c->msg_len); ++failures; }

        /* Streaming in uneven chunks, plus a mid-stream struct copy, must match. */
        BeerHashCtx ctx;
        beer_hash_init(&ctx, c->alg);
        size_t off = 0, step = 1;
        while (off < c->msg_len) {
            size_t n = step < c->msg_len - off ? step : c->msg_len - off;
            beer_hash_update(&ctx, msg + off, n);
            off += n;
            step = step * 3 % 71 + 1;
        }
        BeerHashCtx copy = ctx;
        beer_hash_final(&copy, digest);
        to_hex(digest, beer_hash_digest_size(c->alg), hex);
        if (strcmp(hex, c->hex)) { printf("FAIL stream alg=%d len=%zu\n", c->alg, c->msg_len); ++failures; }
    }

    for (size_t i = 0; i < sizeof(hmac_cases) / sizeof(hmac_cases[0]); ++i) {
        const Case *c = &hmac_cases[i];
        pattern(key, c->key_len, 11);
        pattern(msg, c->msg_len, 3);
        beer_hmac(c->alg, key, c->key_len, msg, c->msg_len, digest);
        to_hex(digest, beer_hash_digest_size(c->alg), hex);
        if (strcmp(hex, c->hex)) { printf("FAIL hmac alg=%d key=%zu len=%zu\n", c->alg, c->key_len, c->msg_len); ++failures; }

        /* A reusable context must give the same MAC on a second pass. */
        BeerHmacCtx ctx;
        beer_hmac_init(&ctx, c->alg, key, c->key_len);
        for (int pass = 0; pass < 2; ++pass) {
            beer_hmac_update(&ctx, msg, c->msg_len);
            beer_hmac_final(&ctx, digest);
            to_hex(digest, beer_hash_digest_size(c->alg), hex);
            if (strcmp(hex, c->hex)) { printf("FAIL hmac reuse pass=%d alg=%d key=%zu\n", pass, c->alg, c->key_len); ++failures; }
        }
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("Beer hash tests passed\n");
    return 0;
}

